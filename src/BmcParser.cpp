#ifdef CPAPDASH_WITH_BMC

// BMC / React Health Luna (G2S family). docs/BMC_FORMAT.md is the format, with
// the source of every fact; this file is the reading of it.

#include "cpapdash/parser/BmcParser.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>

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

}  // namespace

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
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        const std::string ext = toLower(entry.path().extension().string());
        const std::string stem = entry.path().stem().string();
        if (isRawExtension(ext)) raw[stem][ext] = entry.path();
        else if (ext == ".evt") evt[stem] = entry.path();
    }

    for (const auto& [stem, files] : raw) {
        const auto e = evt.find(stem);
        if (e == evt.end()) continue;  // a numbered file with no events beside it is not a card
        std::vector<std::vector<uint8_t>> raws;
        for (const auto& [ext, path] : files) raws.push_back(readFile(path));  // .000, .001, ...
        return buildBmcCard(stem, raws, readFile(e->second));
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

    // Per minute, from the per-second words this parser reads. Flow and the two
    // 25 Hz pressure channels are not reported: their physical scale is not known
    // (docs/BMC_FORMAT.md), and a number in the wrong unit is worse than none.
    struct Minute {
        double leak_sum = 0, ipap_sum = 0, rr_sum = 0, ie_sum = 0;
        int leak_n = 0, ipap_n = 0, rr_n = 0, ie_n = 0;
        double leak_min = 0, leak_max = 0;
    };
    std::map<int64_t, Minute> minutes;
    bool bilevel = false;
    for (auto it = first; it != card.packets.end() && it->stamp <= span.end; ++it) {
        const auto& w = it->words;
        auto& m = minutes[(it->stamp - span.start) / 60];
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
        if (w[bmc_word::kRespRate] != kBmcInvalid) {
            m.rr_sum += w[bmc_word::kRespRate];
            ++m.rr_n;
        }
        if (w[bmc_word::kIeRatio] != kBmcInvalid) {
            m.ie_sum += w[bmc_word::kIeRatio] / 10.0;
            ++m.ie_n;
        }
    }
    for (const auto& [index, m] : minutes) {
        BreathingSummary row(bmcToTimePoint(span.start + index * 60));
        if (m.leak_n) {
            row.leak_rate = m.leak_sum / m.leak_n;
            row.leak_min = m.leak_min;
            row.leak_max = m.leak_max;
        }
        if (m.ipap_n) row.therapy_pressure = m.ipap_sum / m.ipap_n;
        if (m.rr_n) row.respiratory_rate = m.rr_sum / m.rr_n;
        if (m.ie_n) row.ie_ratio = m.ie_sum / m.ie_n;
        session->breathing_summary.push_back(row);
    }
    session->has_summary = !session->breathing_summary.empty();

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

    session->calculateMetrics();
    // No hypopnea record has been seen on a BMC card yet, so this index is
    // apneas only: computed and stored, never graded (Models.h, IndexKind).
    if (session->metrics) session->metrics->index_kind = SessionMetrics::IndexKind::Ungraded;
    return session;
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
        }
    }
    if (raws.empty()) {
        notes_ = BmcNotes{};
        return nullptr;
    }
    return pick(buildBmcCard(serial, raws, evt), civilFromHint(session_start_str), device_id,
                device_name);
}

std::unique_ptr<ISessionParser> createBmcParser() { return std::make_unique<BmcParser>(); }

}  // namespace cpapdash::parser

#endif  // CPAPDASH_WITH_BMC
