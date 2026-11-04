#ifdef CPAPDASH_WITH_PHILIPS

// Identity, session discovery and assembly.
// PROVENANCE: docs/PHILIPS_FORMAT.md sections 1-2 ([A] wiki card layout,
// [B] cpap-lib Properties/model table @f760b21, MIT, see THIRD_PARTY.md).
// No OSCAR source.

#include "cpapdash/parser/PhilipsParser.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <cctype>
#include <unordered_map>

namespace cpapdash::parser {

namespace fs = std::filesystem;

// ── Factory ─────────────────────────────────────────────────────────────────

std::unique_ptr<ISessionParser> createPhilipsParser() {
    return std::make_unique<PhilipsParser>();
}

// ── Properties.txt ──────────────────────────────────────────────────────────

std::optional<PhilipsDeviceInfo> PhilipsParser::parseProperties(const std::string& filepath) {
    std::ifstream f(filepath);
    if (!f.is_open()) return std::nullopt;

    PhilipsDeviceInfo info;
    std::string line;
    bool any = false;
    // key=value per line, reading stops at the first empty line (doc section 2)
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string value = line.substr(eq + 1);
        any = true;

        auto toInt = [](const std::string& s) -> std::optional<int> {
            try { return std::stoi(s); } catch (...) { return std::nullopt; }
        };

        if (key == "SerialNumber")      info.serial_number = value;
        else if (key == "ModelNumber")  info.model_number = value;
        else if (key == "Family")       info.family = toInt(value);
        else if (key == "FamilyVersion") info.family_version = toInt(value);
        else if (key == "DataFormatVersion") info.data_format_version = value;
        else if (key == "PatientFolderNum")  info.patient_folder_num = toInt(value);
    }

    if (!any) return std::nullopt;
    return info;
}

// ── ModelNumber -> product name ─────────────────────────────────────────────
// Table ported from MIT cpap-lib (doc section 2); attribution in THIRD_PARTY.md.

std::optional<std::string> PhilipsParser::modelName(const std::string& model_number) {
    static const std::unordered_map<std::string, std::string> kModels = {
        { "760P", "BiPAP Auto (System One 60 Series)" },
        { "761P", "BiPAP Auto (System One 60 Series)" },
        { "750P", "BiPAP Auto (System One)" },
        { "960P", "BiPAP autoSV Advanced (System One 60 Series)" },
        { "961P", "BiPAP autoSV Advanced (System One 60 Series)" },
        { "960T", "BiPAP autoSV Advanced 30 (System One 60 Series)" },
        { "961TCA", "BiPAP autoSV Advanced 30 (System One 60 Series)" },
        { "950P", "BiPAP AutoSV Advanced System One" },
        { "951P", "BiPAP AutoSV Advanced System One" },
        { "1160P", "BiPAP AVAPS 30 (System One 60 Series)" },
        { "660P", "BiPAP Pro (System One 60 Series)" },
        { "650P", "BiPAP Pro (System One)" },
        { "1061401", "BiPAP S/T (C Series)" },
        { "1061T", "BiPAP S/T 30 (System One 60 Series)" },
        { "501V", "Dorma 500 Auto (System One 60 Series)" },
        { "420X150C", "DreamStation 2 Advanced CPAP" },
        { "410X150C", "DreamStation 2 CPAP" },
        { "700X110", "DreamStation Auto BiPAP" },
        { "700X120", "DreamStation Auto BiPAP" },
        { "700X130", "DreamStation Auto BiPAP" },
        { "700X150", "DreamStation Auto BiPAP" },
        { "500X110", "DreamStation Auto CPAP" },
        { "500X120", "DreamStation Auto CPAP" },
        { "500X130", "DreamStation Auto CPAP" },
        { "500X150", "DreamStation Auto CPAP" },
        { "500X180", "DreamStation Auto CPAP" },
        { "500X140", "DreamStation Auto CPAP with A-Flex" },
        { "501X120", "DreamStation Auto CPAP with P-Flex" },
        { "900X110", "DreamStation BiPAP autoSV" },
        { "900X120", "DreamStation BiPAP autoSV" },
        { "900X150", "DreamStation BiPAP autoSV" },
        { "1130X110", "DreamStation BiPAP AVAPS 30" },
        { "1130X200", "DreamStation BiPAP AVAPS 30" },
        { "1131X150", "DreamStation BiPAP AVAPS 30 AE" },
        { "600X110", "DreamStation BiPAP Pro" },
        { "600X150", "DreamStation BiPAP Pro" },
        { "1030X110", "DreamStation BiPAP S/T 30" },
        { "1030X150", "DreamStation BiPAP S/T 30 with AAM" },
        { "200X110", "DreamStation CPAP" },
        { "400X110", "DreamStation CPAP Pro" },
        { "400X120", "DreamStation CPAP Pro" },
        { "400X130", "DreamStation CPAP Pro" },
        { "400X150", "DreamStation CPAP Pro" },
        { "401X150", "DreamStation CPAP Pro with Auto-Trial" },
        { "400G110", "DreamStation Go" },
        { "500G110", "DreamStation Go Auto" },
        { "500G120", "DreamStation Go Auto" },
        { "500G150", "DreamStation Go Auto" },
        { "502G150", "DreamStation Go Auto" },
        { "560P", "REMstar Auto (System One 60 Series)" },
        { "560PBT", "REMstar Auto (System One 60 Series)" },
        { "561P", "REMstar Auto (System One 60 Series)" },
        { "562P", "REMstar Auto (System One 60 Series)" },
        { "550P", "REMstar Auto (System One)" },
        { "551P", "REMstar Auto (System One)" },
        { "552P", "REMstar Auto (System One)" },
        { "261CA", "REMstar Plus (System One 60 Series)" },
        { "261P", "REMstar Plus (System One 60 Series)" },
        { "251P", "REMstar Plus (System One)" },
        { "460P", "REMstar Pro (System One 60 Series)" },
        { "460PBT", "REMstar Pro (System One 60 Series)" },
        { "461CA", "REMstar Pro (System One 60 Series)" },
        { "461P", "REMstar Pro (System One 60 Series)" },
        { "462P", "REMstar Pro (System One 60 Series)" },
        { "450P", "REMstar Pro (System One)" },
        { "451P", "REMstar Pro (System One)" },
        { "452P", "REMstar Pro (System One)" },
    };

    auto it = kModels.find(model_number);
    if (it == kModels.end()) return std::nullopt;
    return it->second;
}

// ── Session discovery ───────────────────────────────────────────────────────

std::vector<PhilipsSessionFiles> PhilipsParser::discoverSessions(const std::string& dir) {
    std::map<std::string, PhilipsSessionFiles> groups;  // ordered by stem

    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        const auto path = entry.path();
        std::string stem = path.stem().string();
        std::string ext = path.extension().string();

        if (stem.empty() ||
            !std::all_of(stem.begin(), stem.end(),
                         [](unsigned char c) { return std::isdigit(c); })) {
            continue;
        }

        auto& g = groups[stem];
        g.session_id = stem;
        if (ext == ".001") g.summary_path = path.string();
        else if (ext == ".002") g.events_path = path.string();
        else if (ext == ".005") g.waveform_path = path.string();
    }

    std::vector<PhilipsSessionFiles> out;
    for (auto& [stem, g] : groups) {
        if (g.summary_path.empty() && g.events_path.empty() && g.waveform_path.empty())
            continue;
        out.push_back(std::move(g));
    }
    // std::map iterates lexicographically; zero-padded 10-digit stems make
    // that numeric order too.
    return out;
}

// ── parseSession ────────────────────────────────────────────────────────────

namespace {

std::vector<uint8_t> readFileBytes(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
}

// First chunk header timestamp of a session's best file, for hint matching.
std::optional<std::chrono::system_clock::time_point>
sessionTimestamp(const PhilipsSessionFiles& g) {
    const std::string& path = !g.summary_path.empty() ? g.summary_path
                            : !g.events_path.empty()  ? g.events_path
                                                      : g.waveform_path;
    auto bytes = readFileBytes(path);
    PhilipsParseWarnings scratch;
    auto chunks = PhilipsParser::readChunks(bytes.data(), bytes.size(), scratch);
    if (chunks.empty()) return std::nullopt;
    return chunks.front().timestamp;
}

// Case-insensitive Properties.txt lookup in dir, then its parent (doc: card
// layout puts it in the <serial> folder above the Pn session folders).
std::string findPropertiesFile(const std::string& session_dir) {
    auto scan = [](const fs::path& dir) -> std::string {
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (!entry.is_regular_file(ec)) continue;
            std::string name = entry.path().filename().string();
            std::transform(name.begin(), name.end(), name.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            if (name == "properties.txt") return entry.path().string();
        }
        return "";
    };

    fs::path dir(session_dir);
    std::string found = scan(dir);
    if (found.empty() && dir.has_parent_path()) found = scan(dir.parent_path());
    return found;
}

} // namespace

std::unique_ptr<ParsedSession> PhilipsParser::parseSession(
    const std::string& session_dir,
    const std::string& device_id,
    const std::string& device_name,
    std::optional<std::chrono::system_clock::time_point> session_start)
{
    std::error_code ec;
    if (!fs::is_directory(session_dir, ec)) {
        std::cerr << "PhilipsParser: not a directory: " << session_dir << std::endl;
        return nullptr;
    }

    auto groups = discoverSessions(session_dir);
    if (groups.empty()) {
        std::cerr << "PhilipsParser: no PRS1 session files in " << session_dir << std::endl;
        return nullptr;
    }

    // Select the session: closest to the hint when given, else the latest
    // group that has a summary, else the latest group.
    const PhilipsSessionFiles* selected = nullptr;
    if (session_start.has_value()) {
        constexpr auto kHintTolerance = std::chrono::hours(18);
        std::chrono::seconds best{std::chrono::seconds::max()};
        for (const auto& g : groups) {
            auto ts = sessionTimestamp(g);
            if (!ts) continue;
            auto diff = std::chrono::duration_cast<std::chrono::seconds>(
                ts.value() > session_start.value() ? ts.value() - session_start.value()
                                                   : session_start.value() - ts.value());
            if (diff < best && diff <= kHintTolerance) {
                best = diff;
                selected = &g;
            }
        }
    }
    if (selected == nullptr) {
        for (auto it = groups.rbegin(); it != groups.rend(); ++it) {
            if (!it->summary_path.empty()) { selected = &*it; break; }
        }
    }
    if (selected == nullptr) selected = &groups.back();

    auto session = std::make_unique<ParsedSession>();
    session->device_id = device_id;
    session->device_name = device_name;
    session->manufacturer = DeviceManufacturer::PHILIPS;

    // Identity (missing Properties.txt is non-fatal)
    std::string props = findPropertiesFile(session_dir);
    if (!props.empty()) {
        if (auto info = parseProperties(props)) {
            session->serial_number = info->serial_number;
            // model_id is numeric in the unified model; keep the numeric
            // prefix of the alphanumeric ModelNumber ("560P" -> 560).
            std::string digits;
            for (char c : info->model_number) {
                if (std::isdigit(static_cast<unsigned char>(c))) digits.push_back(c);
                else break;
            }
            if (!digits.empty()) {
                try { session->model_id = std::stoi(digits); } catch (...) {}
            }
            if (session->device_name.empty()) {
                auto name = modelName(info->model_number);
                session->device_name = name.value_or("Philips Respironics " + info->model_number);
            }
        }
    }

    PhilipsParseWarnings warnings;
    std::vector<EventStat> stats;

    if (!selected->summary_path.empty()) {
        auto bytes = readFileBytes(selected->summary_path);
        auto chunks = readChunks(bytes.data(), bytes.size(), warnings);
        if (parseSummary(chunks, *session, warnings)) {
            session->has_summary = true;
        }
        session->data_records += static_cast<int>(chunks.size());
    }

    if (!selected->events_path.empty()) {
        auto bytes = readFileBytes(selected->events_path);
        auto chunks = readChunks(bytes.data(), bytes.size(), warnings);
        if (parseEvents(chunks, *session, stats, warnings)) {
            session->has_events = true;
        }
        session->data_records += static_cast<int>(chunks.size());
    }

    if (!selected->waveform_path.empty()) {
        auto bytes = readFileBytes(selected->waveform_path);
        auto chunks = readChunks(bytes.data(), bytes.size(), warnings);
        parseWaveform(chunks, *session, warnings);
        session->data_records += static_cast<int>(chunks.size());
    }

    if (!session->session_start.has_value()) {
        std::cerr << "PhilipsParser: no usable data for session " << selected->session_id
                  << " in " << session_dir << std::endl;
        return nullptr;
    }

    // Fold machine-graphed stats (leak / snore / EPAP, .002 code 0x11) into the
    // per-minute summaries. Machine data wins over flow-derived estimates,
    // same precedent as the ResMed PLD overwrite.
    if (!stats.empty() && !session->breathing_summary.empty()) {
        auto base = session->breathing_summary.front().timestamp;
        for (const auto& s : stats) {
            auto idx = std::chrono::duration_cast<std::chrono::minutes>(
                s.timestamp - base).count();
            if (idx < 0 || idx >= static_cast<long>(session->breathing_summary.size()))
                continue;
            auto& summary = session->breathing_summary[static_cast<size_t>(idx)];
            summary.leak_rate = s.total_leak;
            summary.snore_index = s.snore;
            summary.epr_pressure = s.epap;
        }
    }

    if (warnings.total() > 0) {
        std::cerr << "PhilipsParser: session " << selected->session_id
                  << " parse warnings: crc=" << warnings.crc_failures
                  << " truncated=" << warnings.truncated_chunks
                  << " unknown_events=" << warnings.unknown_event_codes
                  << " unsupported=" << warnings.unsupported_chunks << std::endl;
    }
    session->parse_warnings = warnings.total();

    session->status = ParsedSession::Status::COMPLETED;
    session->file_complete = true;
    session->calculateMetrics();

    return session;
}

// ── parseSessionFromBuffers ─────────────────────────────────────────────────

std::unique_ptr<ParsedSession> PhilipsParser::parseSessionFromBuffers(
    const std::map<std::string, std::pair<const uint8_t*, size_t>>& buffers,
    const std::string& device_id,
    const std::string& device_name,
    const std::string& /*session_start_str*/)
{
    // Buffer keys are literal filenames (Properties.txt, <id>.001, ...);
    // write to temp files and delegate, exact Prisma pattern.
    auto tmp = fs::temp_directory_path() / ("philips_" + std::to_string(
        std::chrono::system_clock::now().time_since_epoch().count()));
    fs::create_directories(tmp);

    for (const auto& [name, buf] : buffers) {
        std::ofstream out(tmp / name, std::ios::binary);
        out.write(reinterpret_cast<const char*>(buf.first), buf.second);
    }

    auto result = parseSession(tmp.string(), device_id, device_name);

    fs::remove_all(tmp);
    return result;
}

} // namespace cpapdash::parser

#endif // CPAPDASH_WITH_PHILIPS
