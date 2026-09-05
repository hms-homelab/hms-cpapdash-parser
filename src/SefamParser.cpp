#ifdef CPAPDASH_WITH_SEFAM

#include "cpapdash/parser/SefamParser.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace cpapdash::parser {

namespace fs = std::filesystem;

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

// Seconds of data a decoded channel represents.
double spanSeconds(const std::vector<double>& v, int freq) {
    if (v.empty() || freq <= 0) return 0;
    return static_cast<double>(v.size()) / static_cast<double>(freq);
}

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

    // Extension -> path. The channel files are named by the channel they carry,
    // so the extension is the key to everything in the folder.
    std::map<std::string, std::string> by_ext;
    std::string ini_path;

    for (const auto& entry : fs::directory_iterator(session_dir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        const std::string name = entry.path().filename().string();
        const std::string ext = extensionOf(name);
        if (ext.empty()) continue;
        if (ext == "INI") { ini_path = entry.path().string(); continue; }
        by_ext[ext] = entry.path().string();
    }

    if (ini_path.empty()) {
        std::cerr << "SefamParser: no .INI in " << session_dir << std::endl;
        return nullptr;
    }

    const SefamIni ini = parseIni(ini_path);
    if (!ini.valid) {
        std::cerr << "SefamParser: INI declares no channels: " << ini_path << std::endl;
        return nullptr;
    }

    // Sizes for the channels the INI declares and the folder actually has.
    std::map<std::string, size_t> sizes;
    for (const auto& ch : ini.channels) {
        auto it = by_ext.find(upper(ch.name));
        if (it == by_ext.end()) continue;
        const auto sz = fs::file_size(it->second, ec);
        if (ec) { ec.clear(); continue; }
        sizes[ch.name] = static_cast<size_t>(sz);
    }

    if (sizes.empty()) {
        std::cerr << "SefamParser: no declared channel has a file in " << session_dir << std::endl;
        return nullptr;
    }

    // Files present that the INI never mentions. Recorded, not read: a channel we
    // cannot describe is a channel we cannot interpret.
    for (const auto& [ext, path] : by_ext) {
        (void)path;
        if (!ini.find(ext)) notes_.undeclared_files.push_back(ext);
    }

    auto header = deduceHeaderLength(ini, sizes);
    if (!header) {
        std::cerr << "SefamParser: cannot establish the channel header length in "
                  << session_dir << "; refusing the session" << std::endl;
        return nullptr;
    }
    notes_.header_bytes = *header;

    // Read every declared channel that has a file, and pool the 8-bit payloads to
    // work out the scrambling from.
    std::map<std::string, std::vector<uint8_t>> raw;
    std::vector<std::vector<uint8_t>> payloads;

    for (const auto& ch : ini.channels) {
        auto it = by_ext.find(upper(ch.name));
        if (it == by_ext.end()) continue;

        auto bytes = readFile(it->second);
        if (bytes.size() <= *header) { raw[ch.name] = std::move(bytes); continue; }

        if (ch.bytesPerSample() == 1)
            payloads.emplace_back(bytes.begin() + static_cast<long>(*header), bytes.end());

        raw[ch.name] = std::move(bytes);
    }

    notes_.descrambler = deduceDescrambler(payloads);

    // Smoothness gets the scrambling key down to two candidates -- a key and its
    // complement read the same -- and an idle stretch picks between them. With
    // nothing to pick on, the card could be read mirrored: a pressure of 10 comes
    // out as 15.5 and still looks like breathing, which is worse than no reading
    // at all.
    if (notes_.descrambler.ambiguous) {
        std::cerr << "SefamParser: cannot establish how the samples are encoded in "
                  << session_dir << "; refusing the session" << std::endl;
        return nullptr;
    }

    // ── Build the session ────────────────────────────────────────────────────

    auto session = std::make_unique<ParsedSession>();
    session->device_id = device_id;
    session->device_name = device_name.empty() ? ini.created_by : device_name;
    session->manufacturer = DeviceManufacturer::SEFAM;
    session->serial_number = ini.serial_number;

    session->session_start = ini.start ? ini.start : session_start;
    if (!session->session_start) {
        std::cerr << "SefamParser: no recording start in " << ini_path << std::endl;
        return nullptr;
    }
    const auto start = *session->session_start;

    // Decode a channel by name, or an empty vector when it is absent or a stub.
    auto decode = [&](const char* name) -> std::pair<std::vector<double>, int> {
        const SefamChannel* ch = ini.find(name);
        if (!ch) return {{}, 0};
        auto it = raw.find(ch->name);
        if (it == raw.end()) return {{}, ch->freq};
        return {decodeChannel(it->second, *header, *ch, notes_.descrambler), ch->freq};
    };

    auto [flow,     flow_hz]  = decode("FLW");
    auto [pressure, press_hz] = decode("PRE");
    auto [leak,     leak_hz]  = decode("LK");
    auto [spo2,     spo2_hz]  = decode("SPO");
    auto [pulse,    hr_hz]    = decode("HRT");

    // Which declared channels we read but have nowhere to put. Named rather than
    // dropped silently, so a card carrying effort belts or position says so.
    static const char* kMapped[] = {"FLW", "PRE", "LK", "SPO", "HRT", "DET"};
    for (const auto& ch : ini.channels) {
        const std::string up = upper(ch.name);
        bool mapped = false;
        for (const char* m : kMapped) if (up == m) { mapped = true; break; }
        if (mapped) continue;
        auto it = sizes.find(ch.name);
        if (it != sizes.end() && it->second > *header) notes_.unmapped_channels.push_back(ch.name);
    }

    // ── Events ───────────────────────────────────────────────────────────────

    if (const SefamChannel* det = ini.find("DET")) {
        auto it = raw.find(det->name);
        if (it != raw.end()) {
            // Detection codes are an enumeration, not a measurement, so they are
            // read at face value. toPhysical() would rescale them onto the
            // channel's declared range and turn code 3 into something that is not
            // 3, which is meaningless for a code.
            SefamChannel as_codes = *det;
            as_codes.min = 0;
            as_codes.max = std::pow(2.0, as_codes.bits > 0 ? as_codes.bits : 8) - 1.0;

            auto codes = decodeChannel(it->second, *header, as_codes, notes_.descrambler);
            session->events = decodeEvents(codes, det->freq, start, &notes_.unknown_event_codes);
            session->has_events = !session->events.empty();
        }
    }

    // ── Duration ─────────────────────────────────────────────────────────────
    //
    // From the samples, never from the INI's duration fields. A card that
    // declares an intended eight hours and holds four would otherwise put an
    // eight-hour denominator under every index. That exact mistake reported an
    // 11h22m night as therapy when 3h42m had been delivered (support ticket 87).

    double span = 0;
    for (const auto& ch : ini.channels) {
        auto it = raw.find(ch.name);
        if (it == raw.end() || it->second.size() <= *header || ch.freq <= 0) continue;
        const size_t samples = (it->second.size() - *header) /
                               static_cast<size_t>(ch.bytesPerSample());
        span = std::max(span, static_cast<double>(samples) / ch.freq);
    }

    if (span <= 0) {
        std::cerr << "SefamParser: every declared channel is a stub in " << session_dir
                  << std::endl;
        return nullptr;
    }

    session->duration_seconds = static_cast<int>(std::llround(span));
    session->session_end = start + std::chrono::seconds(*session->duration_seconds);

    if (ini.real_duration_s > 0 &&
        std::abs(static_cast<double>(ini.real_duration_s) - span) > 60.0) {
        std::cerr << "SefamParser: INI reports " << ini.real_duration_s
                  << "s of recording but the channels hold " << static_cast<long long>(span)
                  << "s; trusting the samples" << std::endl;
    }

    // ── Per-minute rows ──────────────────────────────────────────────────────

    const int minutes = static_cast<int>(std::ceil(span / 60.0));
    for (int m = 0; m < minutes; ++m) {
        BreathingSummary row(start + std::chrono::seconds(m * 60));

        const Stats f = windowStats(flow, flow_hz, m);
        const Stats p = windowStats(pressure, press_hz, m);
        const Stats l = windowStats(leak, leak_hz, m);

        if (!f.any && !p.any && !l.any) continue;

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

    // ── Vitals ───────────────────────────────────────────────────────────────

    if (!spo2.empty() || !pulse.empty()) {
        const double vital_span = std::max(spanSeconds(spo2, spo2_hz),
                                           spanSeconds(pulse, hr_hz));
        const int seconds = static_cast<int>(std::floor(vital_span));

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

    // The channel files only mean anything as a set -- the header length and the
    // scrambling are both derived by comparing them against each other -- so the
    // buffers are laid back out as a folder and read the same way a card is.
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
