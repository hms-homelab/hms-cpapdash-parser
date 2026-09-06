#ifdef CPAPDASH_WITH_SEFAM

#include "cpapdash/parser/SefamParser.h"
#include "cpapdash/parser/EDFParser.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace cpapdash::parser {

namespace fs = std::filesystem;

// Defined in SefamParser_Events.cpp, along with how the bits were identified.
std::map<int, double> sefamDetBitShare(const std::vector<double>& codes);
std::vector<SleepEvent> sefamApneasFromDet(const std::vector<double>& codes, int freq,
                                           std::chrono::system_clock::time_point start);

namespace {

std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::toupper(c); });
    return s;
}

std::string extensionOf(const std::string& filename) {
    auto dot = filename.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= filename.size()) return "";
    return upper(filename.substr(dot + 1));
}

std::vector<uint8_t> readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                                 std::istreambuf_iterator<char>());
}

struct Stats {
    double avg = 0, min = 0, max = 0;
    bool any = false;
};

// One minute's worth of a channel, at whatever rate that channel runs.
Stats windowStats(const std::vector<double>& v, int freq, int minute) {
    Stats s;
    if (v.empty() || freq <= 0) return s;

    const size_t from = static_cast<size_t>(minute) * 60u * static_cast<size_t>(freq);
    if (from >= v.size()) return s;
    const size_t to = std::min(v.size(), from + 60u * static_cast<size_t>(freq));

    s.min = s.max = v[from];
    double sum = 0;
    for (size_t i = from; i < to; ++i) {
        sum += v[i];
        s.min = std::min(s.min, v[i]);
        s.max = std::max(s.max, v[i]);
    }
    s.avg = sum / static_cast<double>(to - from);
    s.any = true;
    return s;
}

// How far apart two channels' spans may be and still be one recording. Block
// framing means a channel ends on a ten-second boundary or at a truncated block,
// so a little slack is normal and a minute of disagreement is not.
constexpr double kSpanToleranceSeconds = 15.0;

} // anonymous namespace

// ── Factory ─────────────────────────────────────────────────────────────────

std::unique_ptr<ISessionParser> createSefamParser() {
    return std::make_unique<SefamParser>();
}

// ── parseSession ────────────────────────────────────────────────────────────
//
// `session_dir` is one DATA_<N> folder, not the card root: the INI that describes
// the channels lives inside the session folder, and there is one session per
// folder.

std::vector<std::string> SefamParser::listSessionStems(const std::string& dir) {
    std::vector<std::string> out;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return out;

    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        const std::string name = entry.path().filename().string();
        if (extensionOf(name) != "INI") continue;
        out.push_back(name.substr(0, name.find_last_of('.')));
    }

    std::sort(out.begin(), out.end());
    return out;
}

std::unique_ptr<ParsedSession> SefamParser::parseSession(
    const std::string& session_dir,
    const std::string& device_id,
    const std::string& device_name,
    std::optional<std::chrono::system_clock::time_point> session_start)
{
    notes_ = SefamSessionNotes{};

    std::error_code ec;
    if (!fs::is_directory(session_dir, ec)) {
        std::cerr << "SefamParser: not a directory: " << session_dir << std::endl;
        return nullptr;
    }

    const auto stems = listSessionStems(session_dir);
    if (stems.empty()) {
        std::cerr << "SefamParser: no .INI in " << session_dir << std::endl;
        return nullptr;
    }

    // A DATA_<n> folder holds one session. A <YYMMDD> folder on the older
    // SleepBox layout holds every recording that started that day, and picking
    // one of them here would be arbitrary -- the caller names it.
    if (stems.size() > 1) {
        std::cerr << "SefamParser: " << stems.size() << " sessions in " << session_dir
                  << "; name one with parseSessionNamed()" << std::endl;
        return nullptr;
    }

    return parseSessionNamed(session_dir, stems.front(), device_id, device_name,
                             session_start);
}

std::unique_ptr<ParsedSession> SefamParser::parseSessionNamed(
    const std::string& session_dir,
    const std::string& stem,
    const std::string& device_id,
    const std::string& device_name,
    std::optional<std::chrono::system_clock::time_point> session_start)
{
    notes_ = SefamSessionNotes{};
    notes_.stem = stem;

    std::error_code ec;
    if (!fs::is_directory(session_dir, ec)) {
        std::cerr << "SefamParser: not a directory: " << session_dir << std::endl;
        return nullptr;
    }

    // Extension -> path, for the files belonging to THIS session. The channel
    // files are named <stem>.<CHANNEL>, so the stem selects the session and the
    // extension selects the channel.
    const std::string want = upper(stem);
    std::map<std::string, std::string> by_ext;
    std::string ini_path;

    for (const auto& entry : fs::directory_iterator(session_dir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        const std::string name = entry.path().filename().string();
        const auto dot = name.find_last_of('.');
        if (dot == std::string::npos) continue;
        if (upper(name.substr(0, dot)) != want) continue;

        const std::string ext = extensionOf(name);
        if (ext.empty()) continue;
        if (ext == "INI") { ini_path = entry.path().string(); continue; }
        by_ext[ext] = entry.path().string();
    }

    if (ini_path.empty()) {
        std::cerr << "SefamParser: no " << stem << ".INI in " << session_dir << std::endl;
        return nullptr;
    }

    const SefamIni ini = parseIni(ini_path);
    if (!ini.valid) {
        std::cerr << "SefamParser: INI declares no channels: " << ini_path << std::endl;
        return nullptr;
    }

    // Files present that the INI never mentions. Recorded, not read: a channel we
    // cannot describe is a channel we cannot interpret.
    for (const auto& [ext, path] : by_ext) {
        (void)path;
        if (!ini.find(ext)) notes_.undeclared_files.push_back(ext);
    }

    // ── Read every declared channel that has a file ──────────────────────────

    std::map<std::string, std::vector<uint8_t>> raw_by_channel;
    bool header_seen = false;

    for (const auto& ch : ini.channels) {
        auto it = by_ext.find(upper(ch.name));
        if (it == by_ext.end()) continue;

        auto raw = readFile(it->second);
        if (raw.size() < kSefamFileHeaderBytes) continue;

        // Every file opens with the same 38-byte header, and it names the device
        // and the recording time. Checking it once per session is a free guard
        // against an INI and a pile of data that do not belong together.
        if (!header_seen) {
            notes_.header = decodeFileHeader(raw);
            header_seen = true;
        }

        if (raw.size() == kSefamFileHeaderBytes) {
            notes_.stub_channels.push_back(ch.name);
            continue;
        }

        raw_by_channel[ch.name] = std::move(raw);
    }

    if (raw_by_channel.empty()) {
        std::cerr << "SefamParser: every declared channel is a stub in " << session_dir
                  << std::endl;
        return nullptr;
    }

    // ── How long a block is ──────────────────────────────────────────────────
    //
    // Derived, not assumed. The S.Box AUTO donor card frames its samples in ten
    // second blocks and Sefam's own SleepBox_AUTO demo recording uses thirty, so
    // a parser that hard-codes either reads the other as corrupt. The channel
    // with the most samples answers it most confidently.

    const SefamChannel* pilot = nullptr;
    size_t widest = 0;
    for (const auto& ch : ini.channels) {
        auto it = raw_by_channel.find(ch.name);
        if (it == raw_by_channel.end()) continue;
        const size_t weight = it->second.size() * static_cast<size_t>(ch.freq);
        if (weight > widest) { widest = weight; pilot = &ch; }
    }

    std::optional<SefamBlockLayout> layout;
    if (pilot) layout = deduceBlockLayout(raw_by_channel[pilot->name], *pilot);

    if (!layout) {
        std::cerr << "SefamParser: cannot establish the block framing in " << session_dir
                  << "; refusing the session" << std::endl;
        return nullptr;
    }
    notes_.layout = *layout;

    std::map<std::string, std::vector<double>> values;
    for (const auto& ch : ini.channels) {
        auto it = raw_by_channel.find(ch.name);
        if (it == raw_by_channel.end()) continue;

        SefamFraming framing;
        values[ch.name] = readChannel(it->second, ch, *layout, &framing);
        notes_.framing[ch.name] = framing;
    }

    // The block checksums are the reason to trust any of this. A bad one means
    // the framing was misread, and everything after it in that channel is
    // suspect, so the session is refused rather than reported.
    for (const auto& [name, framing] : notes_.framing) {
        if (framing.checksum_failures == 0) continue;
        std::cerr << "SefamParser: " << framing.checksum_failures << " of "
                  << framing.blocks << " block checksums failed on " << name
                  << " in " << session_dir << "; refusing the session" << std::endl;
        return nullptr;
    }

    if (notes_.header.valid && !notes_.header.identity.empty() &&
        !ini.serial_number.empty() &&
        notes_.header.identity != ini.serial_number) {
        std::cerr << "SefamParser: the data says " << notes_.header.identity
                  << " and the INI says " << ini.serial_number << " in "
                  << session_dir << "; refusing the session" << std::endl;
        return nullptr;
    }

    // ── Spans ────────────────────────────────────────────────────────────────
    //
    // From the samples, never from the INI's duration fields. The donor card
    // makes the point better than any argument: every session declares
    // "Programmed Record Duration=28800" and "Real Record Duration=28800", eight
    // hours, and the actual recordings run from half an hour to six and a half.
    // Believing the declaration would put an eight-hour denominator under every
    // index on the card.

    double span_lo = 0, span_hi = 0;
    bool any_span = false;
    for (const auto& ch : ini.channels) {
        auto it = values.find(ch.name);
        if (it == values.end() || it->second.empty() || ch.freq <= 0) continue;
        const double span = static_cast<double>(it->second.size()) / ch.freq;
        if (!any_span) { span_lo = span_hi = span; any_span = true; }
        span_lo = std::min(span_lo, span);
        span_hi = std::max(span_hi, span);
    }

    if (!any_span || span_hi <= 0) {
        std::cerr << "SefamParser: no channel holds any samples in " << session_dir
                  << std::endl;
        return nullptr;
    }

    if (span_hi - span_lo > kSpanToleranceSeconds) {
        std::cerr << "SefamParser: channels disagree on the recording length in "
                  << session_dir << " (" << span_lo << "s to " << span_hi
                  << "s); refusing the session" << std::endl;
        return nullptr;
    }

    // ── Build the session ────────────────────────────────────────────────────

    auto session = std::make_unique<ParsedSession>();
    session->device_id = device_id;
    session->device_name = device_name.empty() ? ini.created_by : device_name;
    session->manufacturer = DeviceManufacturer::SEFAM;
    session->serial_number = ini.serial_number;

    session->session_start = ini.start ? ini.start
                           : (notes_.header.stamp ? notes_.header.stamp : session_start);
    if (!session->session_start) {
        std::cerr << "SefamParser: no recording start in " << ini_path << std::endl;
        return nullptr;
    }
    const auto start = *session->session_start;

    session->duration_seconds = static_cast<int>(std::llround(span_hi));
    session->session_end = start + std::chrono::seconds(*session->duration_seconds);

    auto rateOf = [&](const char* name) {
        const SefamChannel* c = ini.find(name);
        return c ? c->freq : 0;
    };
    auto samplesOf = [&](const char* name) -> const std::vector<double>& {
        static const std::vector<double> empty;
        const SefamChannel* c = ini.find(name);
        if (!c) return empty;
        auto it = values.find(c->name);
        return it == values.end() ? empty : it->second;
    };

    const std::vector<double>& flow = samplesOf("FLW");
    const std::vector<double>& pressure = samplesOf("PRE");
    const std::vector<double>& leak = samplesOf("LK");
    const std::vector<double>& spo2 = samplesOf("SPO");
    const std::vector<double>& pulse = samplesOf("HRT");

    // Which declared channels we read but have nowhere to put.
    static const char* kMapped[] = {"FLW", "PRE", "LK", "SPO", "HRT", "DET"};
    for (const auto& [name, samples] : values) {
        if (samples.empty()) continue;
        const std::string up = upper(name);
        bool mapped = false;
        for (const char* m : kMapped) if (up == m) { mapped = true; break; }
        if (!mapped) notes_.unmapped_channels.push_back(name);
    }

    // ── Detections ───────────────────────────────────────────────────────────
    //
    // Apneas only. DET is a bitfield of concurrent flags, and bit 2 is the one
    // whose long runs coincide with the airflow actually stopping -- see
    // SefamParser_Events.cpp for how that was established and for what the other
    // seven bits appear to be.
    //
    // No hypopneas, so this is an APNEA INDEX and not an AHI. Consumers must not
    // grade it against AHI severity thresholds.
    {
        const std::vector<double>& det = samplesOf("DET");
        notes_.det_bit_share = sefamDetBitShare(det);
        session->events = sefamApneasFromDet(det, rateOf("DET"), start);
        session->has_events = !session->events.empty();
    }

    // ── Per-minute rows ──────────────────────────────────────────────────────

    const int minutes = static_cast<int>(std::ceil(span_hi / 60.0));
    const int flow_hz = rateOf("FLW"), press_hz = rateOf("PRE"), leak_hz = rateOf("LK");

    // ── Respiratory mechanics from the flow waveform ─────────────────────────
    //
    // The same breath detector ResMed's 25 Hz BRP flow runs through. Sefam gives
    // us 25 Hz flow, so there is nothing device-specific about this beyond
    // pointing one at the other.
    //
    // Detect once over the whole night and bucket by the minute each breath
    // STARTS in, so a breath straddling a boundary is counted exactly once
    // (SDD-003 D2).
    //
    // What this is worth, and what it is not: respiratory rate, inspiratory and
    // expiratory time and the I:E ratio are TIMINGS. They come out of where the
    // flow crosses zero and are completely independent of what one unit of flow
    // means, so they are as good here as they are for ResMed. Tidal volume and
    // minute ventilation are INTEGRALS of flow, so they carry the unconfirmed
    // scale factor in scaleFor() and are proportional to the truth rather than
    // equal to it. Until a SEFAM Analyze export settles that factor, read them
    // as trends and not as millilitres.
    std::vector<EDFParser::BreathCycle> all_breaths;
    std::vector<std::vector<EDFParser::BreathCycle>> breaths_by_minute(
        minutes > 0 ? static_cast<size_t>(minutes) : 0);

    if (!flow.empty() && flow_hz > 0) {
        all_breaths = EDFParser::detectBreaths(flow, static_cast<double>(flow_hz));
        const int samples_per_minute = flow_hz * 60;
        for (const auto& b : all_breaths) {
            const int m = b.start_idx / samples_per_minute;
            if (m >= 0 && m < minutes) breaths_by_minute[static_cast<size_t>(m)].push_back(b);
        }
    }

    for (int m = 0; m < minutes; ++m) {
        BreathingSummary row(start + std::chrono::seconds(m * 60));

        const Stats f = windowStats(flow, flow_hz, m);
        const Stats p = windowStats(pressure, press_hz, m);
        const Stats l = windowStats(leak, leak_hz, m);

        if (!f.any && !p.any && !l.any) continue;

        // Before the channel statistics below, deliberately. This also derives a
        // leak estimate from the flow waveform, and Sefam measures leak on its
        // own channel -- the machine's own number wins, so it is written after.
        if (!breaths_by_minute[static_cast<size_t>(m)].empty()) {
            EDFParser::calculateRespiratoryMetrics(
                flow, /*pressure_data=*/{}, breaths_by_minute[static_cast<size_t>(m)],
                static_cast<double>(flow_hz), m, row);
        }

        if (f.any) {
            row.avg_flow_rate = f.avg;
            row.min_flow_rate = f.min;
            row.max_flow_rate = f.max;
        }
        if (p.any) {
            row.avg_pressure = p.avg;
            row.min_pressure = p.min;
            row.max_pressure = p.max;
            // What the machine delivered, which is the OSCAR-comparable number.
            row.therapy_pressure = p.avg;
        }
        if (l.any) {
            row.leak_rate = l.avg;
            row.leak_min = l.min;
            row.leak_max = l.max;
        }

        session->breathing_summary.push_back(std::move(row));
    }
    session->has_summary = !session->breathing_summary.empty();

    // Leak at the rate the machine wrote it, so a blow-out inside one minute
    // survives into the night's percentiles instead of averaging away.
    session->native_samples.leak = leak;

    // Breath-by-breath detail, from the same single detection pass the
    // per-minute rows were built from.
    session->breaths.reserve(all_breaths.size());
    for (const auto& b : all_breaths) {
        Breath br;
        br.onset = start + std::chrono::milliseconds(
            static_cast<long long>(1000.0 * b.start_idx / flow_hz));
        br.tidal_volume = b.tidal_volume;
        br.inspiratory_time = b.inspiratory_time;
        br.expiratory_time = b.expiratory_time;
        br.flow_limitation = b.flow_limitation;
        session->breaths.push_back(br);
    }

    // ── Vitals ───────────────────────────────────────────────────────────────

    if (!spo2.empty() || !pulse.empty()) {
        const int spo2_hz = rateOf("SPO"), hr_hz = rateOf("HRT");
        const int seconds = static_cast<int>(std::floor(span_hi));

        for (int s = 0; s < seconds; ++s) {
            VitalSample v(start + std::chrono::seconds(s));
            bool any = false;

            if (spo2_hz > 0) {
                const size_t i = static_cast<size_t>(s) * static_cast<size_t>(spo2_hz);
                if (i < spo2.size()) { v.spo2 = spo2[i]; any = true; }
            }
            if (hr_hz > 0) {
                const size_t i = static_cast<size_t>(s) * static_cast<size_t>(hr_hz);
                if (i < pulse.size()) {
                    v.heart_rate = static_cast<int>(std::lround(pulse[i]));
                    any = true;
                }
            }

            if (any) session->vitals.push_back(std::move(v));
        }
    }

    session->data_records = static_cast<int>(session->breathing_summary.size());
    session->status = ParsedSession::Status::COMPLETED;
    session->file_complete = true;
    session->calculateMetrics();

    return session;
}

// ── parseSessionFromBuffers ─────────────────────────────────────────────────

std::unique_ptr<ParsedSession> SefamParser::parseSessionFromBuffers(
    const std::map<std::string, std::pair<const uint8_t*, size_t>>& buffers,
    const std::string& device_id,
    const std::string& device_name,
    const std::string& session_start_str)
{
    (void)session_start_str;

    // The channel files only mean anything as a set -- the INI describes them and
    // their spans are checked against each other -- so the buffers are laid back
    // out as a folder and read the same way a card is.
    auto tmp = fs::temp_directory_path() / ("sefam_" + std::to_string(
        std::chrono::system_clock::now().time_since_epoch().count()));
    std::error_code ec;
    fs::create_directories(tmp, ec);

    for (const auto& [name, buf] : buffers) {
        std::ofstream out(tmp / name, std::ios::binary);
        out.write(reinterpret_cast<const char*>(buf.first),
                  static_cast<std::streamsize>(buf.second));
    }

    auto result = parseSession(tmp.string(), device_id, device_name);

    fs::remove_all(tmp, ec);
    return result;
}

} // namespace cpapdash::parser

#endif // CPAPDASH_WITH_SEFAM
