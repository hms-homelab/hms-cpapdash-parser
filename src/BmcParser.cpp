#ifdef CPAPDASH_WITH_BMC

// BMC / React Health Luna (G2S family). docs/BMC_FORMAT.md is the format, with
// the source of every fact; this file is the reading of it.

#include "cpapdash/parser/BmcParser.h"
#include "cpapdash/parser/EDFParser.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>

namespace cpapdash::parser {

namespace {

namespace fs = std::filesystem;

constexpr int64_t kDay = 86400;
constexpr int64_t kNoon = 43200;

uint16_t le16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint32_t le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

// Days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant's
// days_from_civil), and back.
int64_t daysFromCivil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

void civilFromDays(int64_t z, int& y, int& m, int& d) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    y = static_cast<int>(static_cast<int64_t>(yoe) + era * 400 + (m <= 2));
}

int daysInMonth(int y, int m) {
    static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return (m == 2 && leap) ? 29 : kDays[m - 1];
}

int64_t floorDiv(int64_t a, int64_t b) {
    return a / b - ((a % b != 0) && ((a < 0) != (b < 0)));
}

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

// ".000" .. ".999": a raw data file.
bool isRawExtension(const std::string& ext) {
    return ext.size() == 4 && ext[0] == '.' && std::isdigit(static_cast<unsigned char>(ext[1])) &&
           std::isdigit(static_cast<unsigned char>(ext[2])) &&
           std::isdigit(static_cast<unsigned char>(ext[3]));
}

std::vector<uint8_t> readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
}

// A local time_point back to civil seconds, for a caller's hint.
BmcCivilSeconds civilFromTimePoint(std::chrono::system_clock::time_point tp) {
    const std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm lt{};
#if defined(_WIN32)
    localtime_s(&lt, &t);
#else
    localtime_r(&t, &lt);
#endif
    return daysFromCivil(lt.tm_year + 1900, static_cast<unsigned>(lt.tm_mon + 1),
                         static_cast<unsigned>(lt.tm_mday)) * kDay +
           lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec;
}

// "YYYYMMDD_HHMMSS".
std::optional<BmcCivilSeconds> civilFromHint(const std::string& s) {
    if (s.size() != 15 || s[8] != '_') return std::nullopt;
    for (size_t i = 0; i < s.size(); ++i)
        if (i != 8 && !std::isdigit(static_cast<unsigned char>(s[i]))) return std::nullopt;
    const auto num = [&](size_t pos, size_t len) { return std::atoi(s.substr(pos, len).c_str()); };
    return bmcCivilSeconds(num(0, 4), num(4, 2), num(6, 2), num(9, 2), num(11, 2), num(13, 2));
}

std::optional<BmcCivilSeconds> eventOnset(const BmcEventRecord& e,
                                          const std::map<int, BmcCivilSeconds>& noon) {
    const auto it = noon.find(e.session_number);
    if (it == noon.end()) return std::nullopt;
    return it->second + static_cast<int64_t>(e.onset_seconds);
}

// Noon of each session number's sleep day, from its first packet by TIME.
std::map<int, BmcCivilSeconds> sleepDayNoons(const BmcCard& card) {
    std::map<int, BmcCivilSeconds> out;
    for (const auto& p : card.packets) {
        const int n = p.words[bmc_word::kSessionNumber];
        if (!out.count(n)) out[n] = bmcSleepDayNoon(p.stamp);
    }
    return out;
}

bool isAllFill(const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; ++i)
        if (p[i] != 0xFF) return false;
    return true;
}

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// The leak level under the total flow, in codes: the mean flow over each whole
// breath, which is what leaves the circuit other than the patient's breathing
// (vent and leak). First a centred 12 s mean; breaths found against it with a
// 25-code (2.5 L/min) hysteresis; then each breath's own mean, placed at its
// centre and joined linearly. NaN samples are skipped throughout.
std::vector<double> leakLevel(const std::vector<double>& total) {
    const size_t n = total.size();
    std::vector<double> base(n, kNaN);
    if (n == 0) return base;

    // Pass 1: centred moving mean over [i - 150, i + 149], valid samples only.
    constexpr int kWindow = 12 * kBmcWaveHz;
    std::vector<double> sum(n + 1, 0.0);
    std::vector<int> cnt(n + 1, 0);
    for (size_t i = 0; i < n; ++i) {
        const bool ok = !std::isnan(total[i]);
        sum[i + 1] = sum[i] + (ok ? total[i] : 0.0);
        cnt[i + 1] = cnt[i] + (ok ? 1 : 0);
    }
    for (size_t i = 0; i < n; ++i) {
        const size_t lo = i >= kWindow / 2 ? i - kWindow / 2 : 0;
        const size_t hi = std::min(n, i + kWindow - kWindow / 2);
        const int c = cnt[hi] - cnt[lo];
        if (c > 0) base[i] = (sum[hi] - sum[lo]) / c;
    }

    // Inspiration starts: the last upward zero crossing before the flow clears
    // +h, once the previous breath has gone below -h.
    constexpr double kHysteresis = 25.0;
    std::vector<size_t> starts;
    bool inspiring = false;
    long last_up = -1;
    for (size_t i = 1; i < n; ++i) {
        const double a = total[i - 1] - base[i - 1], b = total[i] - base[i];
        if (std::isnan(a) || std::isnan(b)) continue;
        if (a <= 0 && b > 0) last_up = static_cast<long>(i);
        if (!inspiring && b > kHysteresis && last_up >= 0) {
            if (starts.empty() || static_cast<size_t>(last_up) > starts.back())
                starts.push_back(static_cast<size_t>(last_up));
            inspiring = true;
        } else if (inspiring && b < -kHysteresis) {
            inspiring = false;
        }
    }

    // Pass 2: each whole breath's mean at its centre, joined linearly.
    std::vector<double> centre, level;
    for (size_t k = 0; k + 1 < starts.size(); ++k) {
        double s = 0;
        int c = 0;
        for (size_t i = starts[k]; i < starts[k + 1]; ++i)
            if (!std::isnan(total[i])) s += total[i], ++c;
        if (c == 0) continue;
        centre.push_back((starts[k] + starts[k + 1]) / 2.0);
        level.push_back(s / c);
    }
    if (centre.size() < 2) return base;
    size_t seg = 0;
    for (size_t i = 0; i < n; ++i) {
        const double x = static_cast<double>(i);
        if (x <= centre.front()) {
            base[i] = level.front();
        } else if (x >= centre.back()) {
            base[i] = level.back();
        } else {
            while (centre[seg + 1] < x) ++seg;
            const double f = (x - centre[seg]) / (centre[seg + 1] - centre[seg]);
            base[i] = level[seg] + f * (level[seg + 1] - level[seg]);
        }
    }
    return base;
}

// One session as 25 Hz series from its first second, one slot per second of
// its span whether or not a packet landed there.
struct SessionWave {
    BmcCivilSeconds start = 0;
    std::vector<double> total;     // flow, signed codes; NaN = no reading
    std::vector<double> patient;   // L/min, leak level removed
    std::vector<double> pressure;  // cmH2O; empty when the model is not measured
    std::vector<int> tidal;        // word 99 per second; -1 = no reading
};

SessionWave sessionWave(const BmcCard& card, const BmcSessionSpan& span) {
    SessionWave w;
    w.start = span.start;
    const size_t seconds = static_cast<size_t>(span.end - span.start + 1);
    const size_t n = seconds * kBmcWaveHz;
    w.total.assign(n, kNaN);
    w.tidal.assign(seconds, -1);
    const auto scale = bmcPressureScale(card.model);
    if (scale) w.pressure.assign(n, kNaN);

    auto it = std::lower_bound(card.packets.begin(), card.packets.end(), span.start,
                               [](const BmcPacket& p, BmcCivilSeconds s) { return p.stamp < s; });
    for (; it != card.packets.end() && it->stamp <= span.end; ++it) {
        const auto& words = it->words;
        const size_t sec = static_cast<size_t>(it->stamp - span.start);
        const size_t at = sec * kBmcWaveHz;
        if (words[bmc_word::kTidalVolume] != kBmcInvalid && words[bmc_word::kTidalVolume] != 0)
            w.tidal[sec] = words[bmc_word::kTidalVolume];

        // At the leak cap the mask is off: what flows is not breathing.
        const uint16_t leak = words[bmc_word::kLeak];
        if (leak == kBmcInvalid || leak < kBmcLeakCap) {
            for (int i = 0; i < kBmcWaveHz; ++i)
                w.total[at + i] = static_cast<int16_t>(words[bmc_word::kFlow + i]);
        }

        if (scale) {
            // A second whose 25 samples are all exactly 0 is the machine not
            // writing the channel (seen with the blower running at a set
            // pressure), not a reading of 0 cmH2O.
            bool all_zero = true;
            for (int i = 0; i < kBmcWaveHz; ++i)
                if (words[bmc_word::kPressure + i] != 0) all_zero = false;
            if (all_zero) continue;
            for (int i = 0; i < kBmcWaveHz; ++i) {
                const int16_t code = static_cast<int16_t>(words[bmc_word::kPressure + i]);
                w.pressure[at + i] = (code - scale->zero) / scale->codes_per_cmh2o;
            }
        }
    }

    const auto base = leakLevel(w.total);
    w.patient.assign(n, kNaN);
    for (size_t i = 0; i < n; ++i)
        if (!std::isnan(w.total[i]) && !std::isnan(base[i]))
            w.patient[i] = (w.total[i] - base[i]) * kBmcFlowLpmPerCode;
    return w;
}

// The machine's own tidal volume for a breath ending at second `end_s`: word 99
// is written once per breath, 1 to 2 s after it ends, so the first change in
// the three seconds after the end; with no change (two equal breaths), the
// value 2 s after. -1 when there is none.
int machineTidalVolume(const std::vector<int>& tidal, size_t end_s) {
    if (end_s >= tidal.size()) return -1;
    const int before = tidal[end_s];
    for (size_t s = end_s + 1; s <= end_s + 3 && s < tidal.size(); ++s)
        if (tidal[s] >= 0 && tidal[s] != before) return tidal[s];
    return end_s + 2 < tidal.size() ? tidal[end_s + 2] : -1;
}

}  // namespace

std::optional<BmcPressureScale> bmcPressureScale(const std::string& model) {
    // Each fitted on end-expiratory pauses against the set pressure, and the
    // G3 also against its own pressure display (docs/BMC_FORMAT.md, "Pressure").
    if (model == "G3 A20") return BmcPressureScale{0.0, 10.0};
    if (model == "G2S B20A") return BmcPressureScale{102.0, 23.23};
    return std::nullopt;
}

std::string bmcModelFromUsr(const uint8_t* data, size_t len) {
    if (!data || len <= kBmcUsrModelOffset) return {};
    std::string out;
    for (size_t i = 0; i < kBmcUsrModelBytes && kBmcUsrModelOffset + i < len; ++i) {
        const uint8_t c = data[kBmcUsrModelOffset + i];
        if (c == 0) break;
        if (c < 0x20 || c > 0x7E) return {};
        out.push_back(static_cast<char>(c));
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

std::optional<BmcCivilSeconds> bmcCivilSeconds(int year, int month, int day, int hour, int minute,
                                               int second) {
    if (year < 1970 || year > 2200 || month < 1 || month > 12) return std::nullopt;
    if (day < 1 || day > daysInMonth(year, month)) return std::nullopt;
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59)
        return std::nullopt;
    return daysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day)) * kDay +
           hour * 3600 + minute * 60 + second;
}

std::chrono::system_clock::time_point bmcToTimePoint(BmcCivilSeconds s) {
    int y = 0, m = 0, d = 0;
    civilFromDays(floorDiv(s, kDay), y, m, d);
    const int64_t tod = s - floorDiv(s, kDay) * kDay;
    std::tm t{};
    t.tm_year = y - 1900;
    t.tm_mon = m - 1;
    t.tm_mday = d;
    t.tm_hour = static_cast<int>(tod / 3600);
    t.tm_min = static_cast<int>((tod % 3600) / 60);
    t.tm_sec = static_cast<int>(tod % 60);
    t.tm_isdst = -1;
    return std::chrono::system_clock::from_time_t(std::mktime(&t));
}

std::optional<BmcPacket> decodeBmcPacket(const uint8_t* packet) {
    BmcPacket out;
    for (size_t i = 0; i < kBmcPacketWords; ++i) out.words[i] = le16(packet + 2 * i);
    if (out.words[bmc_word::kSync] != kBmcSync) return std::nullopt;
    const uint8_t* ts = packet + 2 * kBmcPacketWords;
    const auto stamp = bmcCivilSeconds(le16(ts), ts[2], ts[3], ts[4], ts[5], ts[6]);
    if (!stamp) return std::nullopt;
    out.stamp = *stamp;
    return out;
}

std::vector<BmcEventRecord> decodeBmcEvents(const uint8_t* data, size_t len, int* refused) {
    std::vector<BmcEventRecord> out;
    int bad = 0;
    for (size_t off = kBmcEventRegionBytes; off + kBmcEventRecordBytes <= len;
         off += kBmcEventRecordBytes) {
        const uint8_t* r = data + off;
        if (le16(r) != kBmcSync) {
            // An unwritten slot is fill, not a broken record.
            if (!isAllFill(r, kBmcEventRecordBytes)) ++bad;
            continue;
        }
        BmcEventRecord e;
        e.session_number = le16(r + 2);
        e.type = le16(r + 4);
        e.onset_seconds = le32(r + 8);
        e.duration_seconds = le32(r + 12);
        out.push_back(e);
    }
    if (refused) *refused = bad;
    return out;
}

BmcCivilSeconds bmcSleepDayNoon(BmcCivilSeconds stamp) {
    int64_t day = floorDiv(stamp, kDay);
    if (stamp - day * kDay < kNoon) day -= 1;
    return day * kDay + kNoon;
}

BmcCard buildBmcCard(const std::string& serial, const std::vector<std::vector<uint8_t>>& raw_files,
                     const std::vector<uint8_t>& evt_file) {
    BmcCard card;
    card.serial = serial;
    card.notes.serial = serial;

    BmcCivilSeconds last = 0;
    bool have_last = false;
    for (const auto& file : raw_files) {
        for (size_t off = 0; off + kBmcPacketBytes <= file.size(); off += kBmcPacketBytes) {
            ++card.notes.packets;
            auto p = decodeBmcPacket(file.data() + off);
            if (!p) {
                ++card.notes.refused_packets;
                continue;
            }
            if (have_last && p->stamp < last) card.notes.out_of_order = true;
            last = p->stamp;
            have_last = true;
            card.packets.push_back(*p);
        }
    }

    // Time order, not file order: the machine's clock can be set mid-card, and
    // what is written after that sits after packets stamped later. Stable, so of
    // two packets for the same second the one written first comes first.
    std::stable_sort(card.packets.begin(), card.packets.end(),
                     [](const BmcPacket& a, const BmcPacket& b) { return a.stamp < b.stamp; });
    std::vector<BmcPacket> unique;
    unique.reserve(card.packets.size());
    for (auto& p : card.packets) {
        if (!unique.empty() && unique.back().stamp == p.stamp) {
            ++card.notes.duplicate_seconds;
            continue;
        }
        unique.push_back(p);
    }
    card.packets = std::move(unique);

    for (const auto& p : card.packets) {
        if (card.sessions.empty() ||
            p.stamp - card.sessions.back().end > kBmcSessionGapSeconds) {
            BmcSessionSpan s;
            s.start = p.stamp;
            s.session_number = p.words[bmc_word::kSessionNumber];
            card.sessions.push_back(s);
        }
        auto& s = card.sessions.back();
        s.end = p.stamp;
        ++s.seconds;
    }
    card.notes.sessions = static_cast<int>(card.sessions.size());

    if (!evt_file.empty())
        card.events = decodeBmcEvents(evt_file.data(), evt_file.size(),
                                      &card.notes.refused_event_records);

    // An apnea that falls in no session means the anchor is wrong for this card,
    // which is worth seeing rather than silently dropping.
    const auto noon = sleepDayNoons(card);
    for (const auto& e : card.events) {
        if (e.type != bmc_event::kClearAirway && e.type != bmc_event::kObstructive) continue;
        const auto at = eventOnset(e, noon);
        const bool inside = at && std::any_of(card.sessions.begin(), card.sessions.end(),
                                              [&](const BmcSessionSpan& s) {
                                                  return *at >= s.start && *at <= s.end;
                                              });
        if (!inside) ++card.notes.events_outside_session;
    }
    return card;
}

std::optional<BmcCard> BmcParser::readCard(const std::string& dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return std::nullopt;

    // stem -> (raw files by extension, evt path)
    std::map<std::string, std::map<std::string, fs::path>> raw;
    std::map<std::string, fs::path> evt;
    std::map<std::string, fs::path> usr;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        const std::string ext = toLower(entry.path().extension().string());
        const std::string stem = entry.path().stem().string();
        if (isRawExtension(ext)) raw[stem][ext] = entry.path();
        else if (ext == ".evt") evt[stem] = entry.path();
        else if (ext == ".usr") usr[stem] = entry.path();
    }

    for (const auto& [stem, files] : raw) {
        const auto e = evt.find(stem);
        if (e == evt.end()) continue;  // a numbered file with no events beside it is not a card
        std::vector<std::vector<uint8_t>> raws;
        for (const auto& [ext, path] : files) raws.push_back(readFile(path));  // .000, .001, ...
        auto card = buildBmcCard(stem, raws, readFile(e->second));
        const auto u = usr.find(stem);
        if (u != usr.end()) {
            // Only the bytes up to the end of the model string: the rest of the
            // file is patient details and is never read.
            std::vector<uint8_t> head(kBmcUsrModelOffset + kBmcUsrModelBytes, 0);
            std::ifstream in(u->second, std::ios::binary);
            in.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size()));
            card.model = bmcModelFromUsr(head.data(), static_cast<size_t>(in.gcount()));
        }
        return card;
    }
    return std::nullopt;
}

std::vector<BmcSessionSpan> BmcParser::listSessions(const std::string& dir) {
    const auto card = readCard(dir);
    return card ? card->sessions : std::vector<BmcSessionSpan>{};
}

std::unique_ptr<ParsedSession> BmcParser::sessionFromCard(const BmcCard& card,
                                                          const BmcSessionSpan& span,
                                                          const std::string& device_id,
                                                          const std::string& device_name,
                                                          BmcNotes* notes) {
    auto session = std::make_unique<ParsedSession>();
    session->device_id = device_id;
    session->device_name = device_name;
    session->serial_number = card.serial;
    session->manufacturer = DeviceManufacturer::BMC;
    session->session_start = bmcToTimePoint(span.start);
    session->session_end = bmcToTimePoint(span.end + 1);
    session->duration_seconds = span.seconds;
    session->data_records = span.seconds;
    session->file_complete = true;
    session->status = ParsedSession::Status::COMPLETED;

    const auto first = std::lower_bound(
        card.packets.begin(), card.packets.end(), span.start,
        [](const BmcPacket& p, BmcCivilSeconds s) { return p.stamp < s; });

    // Per minute, from the per-second words and the 25 Hz flow and pressure
    // (docs/BMC_FORMAT.md). Channel 29-53 is not read: its meaning is unknown.
    struct Minute {
        double leak_sum = 0, ipap_sum = 0, epap_sum = 0, rr_sum = 0, ie_sum = 0;
        int leak_n = 0, ipap_n = 0, epap_n = 0, rr_n = 0, ie_n = 0;
        double leak_min = 0, leak_max = 0;
        double vt_sum = 0, mv_sum = 0;
        int vt_n = 0, mv_n = 0;
        int seconds = 0;  // packets in the minute
    };
    std::map<int64_t, Minute> minutes;
    bool bilevel = false;
    for (auto it = first; it != card.packets.end() && it->stamp <= span.end; ++it) {
        const auto& w = it->words;
        auto& m = minutes[(it->stamp - span.start) / 60];
        ++m.seconds;
        // The machine's own volumes, held from one breath to the next. 0 is
        // written before the first breath and while nothing is breathed: no
        // reading, never a volume of 0.
        if (w[bmc_word::kTidalVolume] != kBmcInvalid && w[bmc_word::kTidalVolume] != 0) {
            m.vt_sum += w[bmc_word::kTidalVolume];
            ++m.vt_n;
        }
        if (w[bmc_word::kMinuteVent] != kBmcInvalid && w[bmc_word::kMinuteVent] != 0) {
            m.mv_sum += w[bmc_word::kMinuteVent] / 10.0;
            ++m.mv_n;
        }
        if (w[bmc_word::kLeak] != kBmcInvalid) {
            const double leak = w[bmc_word::kLeak] / 10.0;
            m.leak_min = m.leak_n ? std::min(m.leak_min, leak) : leak;
            m.leak_max = m.leak_n ? std::max(m.leak_max, leak) : leak;
            m.leak_sum += leak;
            ++m.leak_n;
            session->native_samples.leak.push_back(leak);
        }
        if (w[bmc_word::kIpap] != kBmcInvalid) {
            // IPAP is what an independent reader plots as "Pressure" for this
            // machine, so it is the therapy pressure here.
            const double ipap = w[bmc_word::kIpap] / 2.0;
            m.ipap_sum += ipap;
            ++m.ipap_n;
            session->native_samples.therapy.push_back(ipap);
            if (w[bmc_word::kEpap] != kBmcInvalid && w[bmc_word::kIpap] > w[bmc_word::kEpap])
                bilevel = true;
        }
        if (w[bmc_word::kEpap] != kBmcInvalid) {
            // EPAP lands where an AirCurve's does (PLD EprPress.2s), so a consumer's
            // IPAP/EPAP reading of a bi-level is the same for both machines.
            m.epap_sum += w[bmc_word::kEpap] / 2.0;
            ++m.epap_n;
        }
        // 0, like 0xFFFF, is the machine with no rate yet (it writes 0 while it
        // warms up), not a rate of 0: a short session of nothing else read 0.0.
        if (w[bmc_word::kRespRate] != kBmcInvalid && w[bmc_word::kRespRate] != 0) {
            m.rr_sum += w[bmc_word::kRespRate];
            ++m.rr_n;
        }
        if (w[bmc_word::kIeRatio] != kBmcInvalid && w[bmc_word::kIeRatio] != 0) {
            m.ie_sum += w[bmc_word::kIeRatio] / 10.0;  // Te/Ti
            ++m.ie_n;
        }
    }

    // The 25 Hz series, and the breaths found ONCE over the whole session on the
    // patient flow (zero-centred L/min, what the shared detector expects), then
    // bucketed by the minute their onset falls in, as the ResMed BRP path does.
    const SessionWave wave = sessionWave(card, span);
    const int per_minute = 60 * kBmcWaveHz;
    auto breaths = EDFParser::detectBreaths(wave.patient, kBmcWaveHz);
    // A breath lies wholly on flow the card measured. The detector reads a
    // sample with no reading (a second missing from the card, or one at the
    // leak cap) as zero flow and carries a breath across the gap: a 4 s session
    // spread over 12 s got one "breath" of Ti 9.2 s and Te 0.36 s, an I:E of 25.
    breaths.erase(std::remove_if(breaths.begin(), breaths.end(),
                                 [&wave](const EDFParser::BreathCycle& b) {
                                     for (int i = b.start_idx; i <= b.end_idx; ++i)
                                         if (i < 0 || static_cast<size_t>(i) >= wave.patient.size() ||
                                             std::isnan(wave.patient[i]))
                                             return true;
                                     return false;
                                 }),
                  breaths.end());
    std::map<int64_t, std::vector<EDFParser::BreathCycle>> breaths_by_minute;
    for (const auto& b : breaths) breaths_by_minute[b.start_idx / per_minute].push_back(b);

    for (const auto& [index, m] : minutes) {
        BreathingSummary row(bmcToTimePoint(span.start + index * 60));
        const size_t lo = static_cast<size_t>(index) * per_minute;
        const size_t hi = std::min(wave.patient.size(), lo + per_minute);

        // Flow: the minute counts when at least half of its seconds on the card
        // carry a flow reading; otherwise it is marked unmeasured, never 0.
        double fsum = 0, tsum = 0, fmax = 0, fmin = 0;
        int fn = 0;
        for (size_t i = lo; i < hi; ++i) {
            if (std::isnan(wave.patient[i])) continue;
            const double f = wave.patient[i];
            fmax = fn ? std::max(fmax, f) : f;
            fmin = fn ? std::min(fmin, f) : f;
            fsum += f;
            tsum += wave.total[i];
            ++fn;
        }
        row.flow_measured = fn > 0 && 2 * fn >= m.seconds * kBmcWaveHz;
        if (row.flow_measured) {
            row.avg_flow_rate = fsum / fn;
            row.max_flow_rate = fmax;
            row.min_flow_rate = fmin;
            // Everything leaving the circuit (vent and leak): the mean TOTAL
            // flow, which needs the zero that patient flow does not.
            row.total_leak = (tsum / fn - kBmcFlowZero) * kBmcFlowLpmPerCode;
        }

        // Pressure: only for a model whose scale has been measured.
        double psum = 0, pmax = 0, pmin = 0;
        int pn = 0;
        for (size_t i = lo; i < hi && !wave.pressure.empty(); ++i) {
            if (std::isnan(wave.pressure[i])) continue;
            const double p = wave.pressure[i];
            pmax = pn ? std::max(pmax, p) : p;
            pmin = pn ? std::min(pmin, p) : p;
            psum += p;
            ++pn;
        }
        row.pressure_measured = pn > 0 && 2 * pn >= m.seconds * kBmcWaveHz;
        if (row.pressure_measured) {
            row.avg_pressure = psum / pn;
            row.max_pressure = pmax;
            row.min_pressure = pmin;
        }

        // Ti, Te, I:E (Ti/Te, as for every other machine) and the flow
        // percentile from the breaths, through the shared pipeline.
        if (row.flow_measured) {
            const auto b = breaths_by_minute.find(index);
            EDFParser::calculateRespiratoryMetrics(
                wave.patient, row.pressure_measured ? wave.pressure : std::vector<double>{},
                b == breaths_by_minute.end() ? std::vector<EDFParser::BreathCycle>{} : b->second,
                kBmcWaveHz, static_cast<int>(index), row);
        }
        // ...and the machine's own words for what it reports itself. The
        // shared pipeline's estimates of these are replaced, not kept.
        row.respiratory_rate = m.rr_n ? std::optional<double>(m.rr_sum / m.rr_n) : std::nullopt;
        row.tidal_volume = m.vt_n ? std::optional<double>(m.vt_sum / m.vt_n) : std::nullopt;
        row.minute_ventilation =
            m.mv_n ? std::optional<double>(m.mv_sum / m.mv_n) : std::nullopt;
        row.leak_rate = std::nullopt;
        if (m.leak_n) {
            row.leak_rate = m.leak_sum / m.leak_n;
            row.leak_min = m.leak_min;
            row.leak_max = m.leak_max;
        }
        // Flow limitation is not reported: the score depends on the leak
        // baseline, and it has not been shown to hold on this machine.
        row.flow_limitation = std::nullopt;
        // A minute with no breaths found still has the machine's I:E: word 105
        // is Te/Ti x 10, turned round to the Ti/Te the field carries.
        if (!row.ie_ratio && m.ie_n) row.ie_ratio = m.ie_n / m.ie_sum;
        if (m.ipap_n) row.therapy_pressure = m.ipap_sum / m.ipap_n;
        if (m.epap_n) row.epr_pressure = m.epap_sum / m.epap_n;
        session->breathing_summary.push_back(row);
    }
    session->has_summary = !session->breathing_summary.empty();

    // Breath by breath, from the same detection pass. The tidal volume is the
    // machine's word 99 for that breath where there is one, else the integral.
    const auto onset0 = bmcToTimePoint(span.start);
    session->breaths.reserve(breaths.size());
    for (const auto& b : breaths) {
        Breath out;
        out.onset = onset0 + std::chrono::microseconds(
                                 static_cast<long long>(b.start_idx * (1e6 / kBmcWaveHz)));
        const int vt = machineTidalVolume(wave.tidal, static_cast<size_t>(b.end_idx / kBmcWaveHz));
        out.tidal_volume = vt > 0 ? vt : b.tidal_volume;
        out.inspiratory_time = b.inspiratory_time;
        out.expiratory_time = b.expiratory_time;
        out.flow_limitation = 0;  // not reported for this machine (above)
        session->breaths.push_back(out);
    }

    if (bilevel) {
        DeviceSettings settings;
        settings.therapy_mode = 2;  // bi-level
        session->settings = settings;
    }

    // The machine's own events in this session's window.
    const auto noon = sleepDayNoons(card);
    int emitted = 0;
    for (const auto& e : card.events) {
        const auto at = eventOnset(e, noon);
        if (!at || *at < span.start || *at > span.end) continue;
        if (e.type == bmc_event::kClearAirway || e.type == bmc_event::kObstructive) {
            session->events.emplace_back(e.type == bmc_event::kClearAirway
                                             ? EventType::CLEAR_AIRWAY
                                             : EventType::OBSTRUCTIVE,
                                         bmcToTimePoint(*at),
                                         static_cast<double>(e.duration_seconds));
            ++emitted;
        } else if (notes) {
            ++notes->span_types[e.type];
        }
    }
    std::sort(session->events.begin(), session->events.end(),
              [](const SleepEvent& a, const SleepEvent& b) { return a.timestamp < b.timestamp; });
    session->has_events = !session->events.empty();
    if (notes) notes->events_emitted = emitted;

    // A real AHI (the default IndexKind). The machine classifies its own apneas,
    // and on the night an independent reader also scored, this index equals its
    // AHI exactly. No hypopnea record has been seen on a BMC card yet; if one turns
    // up under a type not handled here, it lands in span_types and is visible.
    session->calculateMetrics();
    return session;
}

BmcWaveform BmcParser::waveform(const BmcCard& card, BmcCivilSeconds from, BmcCivilSeconds to) {
    BmcWaveform out;
    out.start = from;
    if (to <= from) return out;
    const size_t n = static_cast<size_t>(to - from) * kBmcWaveHz;
    const bool with_pressure = bmcPressureScale(card.model).has_value();
    out.flow.assign(n, kNaN);
    if (with_pressure) out.pressure.assign(n, kNaN);
    for (const auto& span : card.sessions) {
        if (span.end < from || span.start >= to) continue;
        const SessionWave w = sessionWave(card, span);
        const BmcCivilSeconds a = std::max(from, span.start);
        const BmcCivilSeconds b = std::min(to, span.end + 1);
        const size_t src = static_cast<size_t>(a - span.start) * kBmcWaveHz;
        const size_t dst = static_cast<size_t>(a - from) * kBmcWaveHz;
        const size_t len = static_cast<size_t>(b - a) * kBmcWaveHz;
        std::copy_n(w.patient.begin() + src, len, out.flow.begin() + dst);
        if (with_pressure) std::copy_n(w.pressure.begin() + src, len, out.pressure.begin() + dst);
    }
    return out;
}

std::unique_ptr<ParsedSession> BmcParser::pick(BmcCard card, std::optional<BmcCivilSeconds> hint,
                                               const std::string& device_id,
                                               const std::string& device_name) {
    notes_ = card.notes;
    if (card.sessions.empty()) return nullptr;

    const BmcSessionSpan* chosen = &card.sessions.back();
    if (hint) {
        const BmcSessionSpan* nearest = nullptr;
        int64_t best = 0;
        for (const auto& s : card.sessions) {
            if (*hint >= s.start && *hint <= s.end) {
                nearest = &s;
                break;
            }
            const int64_t d = std::llabs(s.start - *hint);
            if (!nearest || d < best) {
                nearest = &s;
                best = d;
            }
        }
        chosen = nearest;
    }
    return sessionFromCard(card, *chosen, device_id, device_name, &notes_);
}

std::unique_ptr<ParsedSession> BmcParser::parseSession(
    const std::string& session_dir, const std::string& device_id, const std::string& device_name,
    std::optional<std::chrono::system_clock::time_point> session_start) {
    auto card = readCard(session_dir);
    if (!card) {
        notes_ = BmcNotes{};
        return nullptr;
    }
    std::optional<BmcCivilSeconds> hint;
    if (session_start) hint = civilFromTimePoint(*session_start);
    return pick(std::move(*card), hint, device_id, device_name);
}

std::unique_ptr<ParsedSession> BmcParser::parseSessionFromBuffers(
    const std::map<std::string, std::pair<const uint8_t*, size_t>>& buffers,
    const std::string& device_id, const std::string& device_name,
    const std::string& session_start_str) {
    std::vector<std::vector<uint8_t>> raws;
    std::vector<uint8_t> evt;
    std::string serial;
    std::string model;
    for (const auto& [key, buf] : buffers) {  // std::map: keys in order, so .000 before .001
        const auto dot = key.find_last_of('.');
        if (dot == std::string::npos) continue;
        const std::string ext = toLower(key.substr(dot));
        const auto slash = key.find_last_of("/\\");
        const std::string stem =
            key.substr(slash == std::string::npos ? 0 : slash + 1,
                       dot - (slash == std::string::npos ? 0 : slash + 1));
        if (isRawExtension(ext)) {
            raws.emplace_back(buf.first, buf.first + buf.second);
            if (serial.empty()) serial = stem;
        } else if (ext == ".evt") {
            evt.assign(buf.first, buf.first + buf.second);
        } else if (ext == ".usr") {
            model = bmcModelFromUsr(buf.first, buf.second);
        }
    }
    if (raws.empty()) {
        notes_ = BmcNotes{};
        return nullptr;
    }
    auto card = buildBmcCard(serial, raws, evt);
    card.model = model;
    return pick(std::move(card), civilFromHint(session_start_str), device_id, device_name);
}

std::unique_ptr<ISessionParser> createBmcParser() { return std::make_unique<BmcParser>(); }

}  // namespace cpapdash::parser

#endif  // CPAPDASH_WITH_BMC
