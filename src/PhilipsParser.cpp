#ifdef CPAPDASH_WITH_PHILIPS

// Identity, session discovery and assembly.
// PROVENANCE: docs/PHILIPS_FORMAT.md sections 1-2 ([A] wiki card layout,
// [B] cpap-lib Properties/model table @f760b21, MIT, see THIRD_PARTY.md) and
// section 9.1/9.3 (v3 card layout, PROP keys, grouping; [C] checked on [S]).
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

// ── Identity manifest: Properties.txt / PROP.TXT / PROP.BIN ─────────────────

namespace {

// A manifest key is letters and digits only. Holding to that keeps the random
// bytes of a still-encrypted PROP.BIN from passing as key=value lines.
bool isManifestKey(const std::string& key) {
    return !key.empty() &&
           std::all_of(key.begin(), key.end(),
                       [](unsigned char c) { return std::isalnum(c) != 0; });
}

} // namespace

std::optional<PhilipsDeviceInfo> PhilipsParser::parseProperties(const std::string& filepath) {
    std::ifstream f(filepath, std::ios::binary);
    if (!f.is_open()) return std::nullopt;

    PhilipsDeviceInfo info;
    std::string line;
    bool any = false;
    // key=value per line, reading stops at the first empty line (doc 2, 9.1)
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string value = line.substr(eq + 1);
        if (!isManifestKey(key)) continue;
        any = true;

        auto toInt = [](const std::string& s) -> std::optional<int> {
            try { return std::stoi(s); } catch (...) { return std::nullopt; }
        };

        // v2 long keys (doc 2) and v3 short keys (doc 9.1)
        if (key == "SerialNumber" || key == "SN")            info.serial_number = value;
        else if (key == "ModelNumber" || key == "MN")        info.model_number = value;
        else if (key == "Family" || key == "F")              info.family = toInt(value);
        else if (key == "FamilyVersion" || key == "FV")      info.family_version = toInt(value);
        else if (key == "DataFormatVersion" || key == "DFV") info.data_format_version = value;
        else if (key == "PatientFolderNum" || key == "FN")   info.patient_folder_num = toInt(value);
        else if (key == "SV")  info.software_version = value;
        // Key-like fields, kept for a separate decryption step (SDD-009 D2).
        else if (key == "SID") info.sid = value;
        else if (key == "BK")  info.bk = value;
        else if (key == "SK")  info.sk = value;
        else if (key == "EK")  info.ek = value;
        else if (key == "DK")  info.dk = value;
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
        // Not in [B]'s table; ModelNumber measured on [S] (doc 2, 9.1).
        { "520X110C", "DreamStation 2 Auto CPAP" },
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

namespace {

enum class FileKind { None, Summary, Events, Waveform };

std::string lowercase(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool allOf(const std::string& s, int (*pred)(int)) {
    return !s.empty() &&
           std::all_of(s.begin(), s.end(), [&](unsigned char c) { return pred(c) != 0; });
}

// v2: decimal-digit stem, .001/.002/.005 (doc 1). v3: 8-hex-digit stem, with
// .B01/.B02/.B05 (DreamStation 2) or .001/.002/.005 (DreamStation 1 on v3
// firmware) (doc 9.1).
FileKind kindOf(const fs::path& path) {
    const std::string stem = path.stem().string();
    const std::string ext = lowercase(path.extension().string());
    const bool decimal_stem = allOf(stem, ::isdigit);
    const bool hex_stem = stem.size() == 8 && allOf(stem, ::isxdigit);
    if (!decimal_stem && !hex_stem) return FileKind::None;

    if (ext == ".001") return FileKind::Summary;
    if (ext == ".002") return FileKind::Events;
    if (ext == ".005") return FileKind::Waveform;
    if (hex_stem) {
        if (ext == ".b01") return FileKind::Summary;
        if (ext == ".b02") return FileKind::Events;
        if (ext == ".b05") return FileKind::Waveform;
    }
    return FileKind::None;
}

// The session number a file NAME implies: hex for an 8-character v3 stem, else
// decimal. Only a fallback, for files whose chunk headers cannot be walked.
std::optional<uint32_t> stemSessionNumber(const std::string& stem) {
    try {
        if (stem.size() == 8 && allOf(stem, ::isxdigit))
            return static_cast<uint32_t>(std::stoul(stem, nullptr, 16));
        if (allOf(stem, ::isdigit))
            return static_cast<uint32_t>(std::stoul(stem, nullptr, 10));
    } catch (...) {}
    return std::nullopt;
}

// Session numbers of the chunks in a file, in file order, without validating
// checksums: a cheap header walk by BlockLength (doc 3.1). Stops at the first
// header that does not look like a v2/v3 chunk.
std::vector<uint32_t> sessionNumbersInFile(const fs::path& path) {
    std::vector<uint32_t> out;
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return out;
    f.seekg(0, std::ios::end);
    const auto size = static_cast<uint64_t>(f.tellg());
    uint64_t pos = 0;
    uint8_t h[15];
    while (pos + sizeof(h) <= size) {
        f.seekg(static_cast<std::streamoff>(pos));
        if (!f.read(reinterpret_cast<char*>(h), sizeof(h))) break;
        const uint16_t block_len = static_cast<uint16_t>(h[1] | (h[2] << 8));
        if ((h[0] != 2 && h[0] != 3) || block_len < 18 || pos + block_len > size) break;
        const uint32_t session = static_cast<uint32_t>(h[7]) | (static_cast<uint32_t>(h[8]) << 8) |
                                 (static_cast<uint32_t>(h[9]) << 16) |
                                 (static_cast<uint32_t>(h[10]) << 24);
        if (std::find(out.begin(), out.end(), session) == out.end()) out.push_back(session);
        pos += block_len;
    }
    return out;
}

} // namespace

std::vector<PhilipsSessionFiles> PhilipsParser::discoverSessions(const std::string& dir) {
    // Ordered by session number. Files are visited in name order so the *_path
    // "first file" of each kind is deterministic.
    std::map<uint32_t, PhilipsSessionFiles> groups;

    std::vector<fs::path> files;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (entry.is_regular_file(ec) && kindOf(entry.path()) != FileKind::None)
            files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());

    for (const auto& path : files) {
        const FileKind kind = kindOf(path);
        const std::string stem = path.stem().string();
        auto sessions = sessionNumbersInFile(path);
        if (sessions.empty()) {
            // Unframeable (corrupt, or still encrypted): fall back to the name so
            // the file is still accounted for; its chunks will not parse.
            auto n = stemSessionNumber(stem);
            if (!n) continue;
            sessions.push_back(*n);
        }
        const auto stem_number = stemSessionNumber(stem);
        for (uint32_t session : sessions) {
            auto& g = groups[session];
            g.session_number = session;
            if (g.session_id.empty() || (stem_number && *stem_number == session))
                g.session_id = stem;
            auto add = [&](std::string& first, std::vector<std::string>& all) {
                if (first.empty()) first = path.string();
                all.push_back(path.string());
            };
            switch (kind) {
                case FileKind::Summary:  add(g.summary_path, g.summary_files); break;
                case FileKind::Events:   add(g.events_path, g.events_files); break;
                case FileKind::Waveform: add(g.waveform_path, g.waveform_files); break;
                case FileKind::None:     break;
            }
        }
    }

    std::vector<PhilipsSessionFiles> out;
    out.reserve(groups.size());
    for (auto& [number, g] : groups) out.push_back(std::move(g));
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

// Every valid chunk of ONE session across the given files (doc 9.3): chunks of
// other sessions sharing a file are left out, and a byte-identical repeat of a
// chunk already taken is dropped as a duplicate.
std::vector<PhilipsChunk> sessionChunks(const std::vector<std::string>& files,
                                        uint32_t session_number,
                                        PhilipsParseWarnings& warnings) {
    std::vector<PhilipsChunk> out;
    for (const auto& path : files) {
        auto bytes = readFileBytes(path);
        for (auto& c : PhilipsParser::readChunks(bytes.data(), bytes.size(), warnings)) {
            if (c.session_number != session_number) continue;
            const bool duplicate = std::any_of(out.begin(), out.end(), [&](const PhilipsChunk& k) {
                return k.timestamp_raw == c.timestamp_raw &&
                       k.file_extension == c.file_extension &&
                       k.block_length == c.block_length && k.data == c.data;
            });
            if (!duplicate) out.push_back(std::move(c));
        }
    }
    return out;
}

// A session can straddle patient folders: the machine opens P<n+1> mid-night and
// writes the rest of the session there, so one folder holds the summary and a
// sibling the events or waveform (doc 9.3; [S]: 4 nights split this way). Add
// the session's files found in sibling P* folders under the same parent, matched
// by file stem; sessionChunks() still filters by the header session number.
PhilipsSessionFiles withSiblingFolders(const std::string& session_dir,
                                       const PhilipsSessionFiles& group) {
    PhilipsSessionFiles out = group;
    const fs::path dir(session_dir);
    if (!dir.has_parent_path() || group.session_id.empty()) return out;

    auto isPatientFolder = [](const std::string& name) {
        return name.size() >= 2 && (name[0] == 'P' || name[0] == 'p') &&
               std::all_of(name.begin() + 1, name.end(),
                           [](unsigned char c) { return std::isdigit(c) != 0; });
    };
    std::error_code ec;
    const auto self = fs::weakly_canonical(dir, ec);
    for (const auto& entry : fs::directory_iterator(dir.parent_path(), ec)) {
        if (!entry.is_directory(ec)) continue;
        if (!isPatientFolder(entry.path().filename().string())) continue;
        if (fs::weakly_canonical(entry.path(), ec) == self) continue;
        for (const auto& file : fs::directory_iterator(entry.path(), ec)) {
            if (!file.is_regular_file(ec) || file.path().stem().string() != group.session_id)
                continue;
            switch (kindOf(file.path())) {
                case FileKind::Summary:  out.summary_files.push_back(file.path().string()); break;
                case FileKind::Events:   out.events_files.push_back(file.path().string()); break;
                case FileKind::Waveform: out.waveform_files.push_back(file.path().string()); break;
                case FileKind::None:     break;
            }
        }
    }
    return out;
}

// Header timestamp of a session's first chunk, for hint matching.
std::optional<std::chrono::system_clock::time_point>
sessionTimestamp(const PhilipsSessionFiles& g) {
    const auto& files = !g.summary_files.empty() ? g.summary_files
                      : !g.events_files.empty()  ? g.events_files
                                                 : g.waveform_files;
    PhilipsParseWarnings scratch;
    auto chunks = sessionChunks(files, g.session_number, scratch);
    if (chunks.empty()) return std::nullopt;
    return chunks.front().timestamp;
}

// Case-insensitive identity-manifest lookup in dir, then its parent (the card
// layout puts it in the folder above the Pn session folders, doc 1 and 9.1).
// Properties.txt (v2), PROP.TXT (v3 DreamStation 1), PROP.BIN (DreamStation 2).
std::string findPropertiesFile(const std::string& session_dir) {
    auto scan = [](const fs::path& dir) -> std::string {
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (!entry.is_regular_file(ec)) continue;
            const std::string name = lowercase(entry.path().filename().string());
            if (name == "properties.txt" || name == "prop.txt" || name == "prop.bin")
                return entry.path().string();
        }
        return "";
    };

    fs::path dir(session_dir);
    std::string found = scan(dir);
    if (found.empty() && dir.has_parent_path()) found = scan(dir.parent_path());
    return found;
}

// A decrypted manifest is printable text (doc 9.1). A still-encrypted PROP.BIN
// is not.
bool looksLikePlainManifest(const std::vector<uint8_t>& bytes) {
    if (bytes.empty()) return false;
    for (uint8_t b : bytes) {
        if (b == '\r' || b == '\n' || b == '\t') continue;
        if (b < 0x20 || b > 0x7e) return false;
    }
    return true;
}

} // namespace

bool PhilipsParser::isEncryptedCard(const std::string& dir) {
    // 1. A PROP.BIN that is not plain text.
    const std::string props = findPropertiesFile(dir);
    if (!props.empty() && lowercase(fs::path(props).filename().string()) == "prop.bin") {
        auto bytes = readFileBytes(props);
        if (!looksLikePlainManifest(bytes) || !parseProperties(props)) return true;
    }

    // 2. Session files whose v3 chunk headers frame but whose data never passes
    //    its CRC (the header in the clear, the data encrypted).
    int examined = 0, all_crc_failed = 0;
    for (const auto& g : discoverSessions(dir)) {
        for (const auto* files : {&g.summary_files, &g.events_files}) {
            for (const auto& path : *files) {
                if (examined >= 8) break;
                auto bytes = readFileBytes(path);
                if (bytes.empty() || bytes[0] != 3) continue;
                PhilipsParseWarnings w;
                auto chunks = readChunks(bytes.data(), bytes.size(), w);
                ++examined;
                if (!chunks.empty()) return false;   // something decodes: not encrypted
                if (w.crc_failures > 0) ++all_crc_failed;
            }
        }
        if (examined >= 8) break;
    }
    return examined > 0 && all_crc_failed == examined;
}

std::optional<PhilipsParser::FlowSeries> PhilipsParser::sessionFlow(
    const std::string& session_dir, uint32_t session_number,
    PhilipsParseWarnings& warnings) {
    std::error_code ec;
    if (!fs::is_directory(session_dir, ec)) return std::nullopt;
    for (const auto& g : discoverSessions(session_dir)) {
        if (g.session_number != session_number) continue;
        const PhilipsSessionFiles files = withSiblingFolders(session_dir, g);
        if (files.waveform_files.empty()) return std::nullopt;
        return flowSeries(sessionChunks(files.waveform_files, session_number, warnings),
                          warnings);
    }
    return std::nullopt;
}

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
        if (isEncryptedCard(session_dir)) {
            std::cerr << "PhilipsParser: encrypted DreamStation 2 card in " << session_dir
                      << " (decrypt it first; this parser never decrypts)" << std::endl;
        } else {
            std::cerr << "PhilipsParser: no PRS1 session files in " << session_dir << std::endl;
        }
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

    // Identity (a missing manifest is non-fatal). Only the serial, model and
    // firmware cross into the session; the key-like fields stay on
    // PhilipsDeviceInfo (SDD-009 D2).
    std::string props = findPropertiesFile(session_dir);
    if (!props.empty()) {
        if (auto info = parseProperties(props)) {
            session->serial_number = info->serial_number;
            session->firmware = info->software_version;
            if (auto name = modelName(info->model_number)) session->product_name = *name;
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

    const PhilipsSessionFiles files = withSiblingFolders(session_dir, *selected);
    const uint32_t number = files.session_number;

    if (!files.summary_files.empty()) {
        auto chunks = sessionChunks(files.summary_files, number, warnings);
        if (parseSummary(chunks, *session, warnings)) {
            session->has_summary = true;
        }
        session->data_records += static_cast<int>(chunks.size());
    }

    if (!files.events_files.empty()) {
        auto chunks = sessionChunks(files.events_files, number, warnings);
        if (parseEvents(chunks, *session, stats, warnings)) {
            session->has_events = true;
        }
        session->data_records += static_cast<int>(chunks.size());
    }

    if (!files.waveform_files.empty()) {
        auto chunks = sessionChunks(files.waveform_files, number, warnings);
        parseWaveform(chunks, *session, warnings);
        session->data_records += static_cast<int>(chunks.size());
    }

    if (!session->session_start.has_value()) {
        if (isEncryptedCard(session_dir)) {
            std::cerr << "PhilipsParser: encrypted DreamStation 2 card in " << session_dir
                      << " (decrypt it first; this parser never decrypts)" << std::endl;
        } else {
            std::cerr << "PhilipsParser: no usable data for session " << selected->session_id
                      << " in " << session_dir << std::endl;
        }
        return nullptr;
    }

    // Fold machine-graphed stats (total leak / snore / EPAP, .002 code 0x11)
    // into the per-minute summaries, same precedent as the ResMed PLD overwrite.
    if (!stats.empty() && !session->breathing_summary.empty()) {
        auto base = session->breathing_summary.front().timestamp;
        for (const auto& s : stats) {
            auto idx = std::chrono::duration_cast<std::chrono::minutes>(
                s.timestamp - base).count();
            if (idx < 0 || idx >= static_cast<long>(session->breathing_summary.size()))
                continue;
            auto& summary = session->breathing_summary[static_cast<size_t>(idx)];
            // TOTAL leak, the mask's vent flow included: its own slot, never
            // leak_rate, which is the UNINTENTIONAL leak everywhere else (a
            // ResMed's PLD, the reports, the 24 L/min large-leak line). The
            // vent's share is the mask's and the pressure's, which the card
            // does not say, so no unintentional leak is derived from it.
            summary.total_leak = s.total_leak;
            summary.snore_index = s.snore;
            summary.epr_pressure = s.epap;
        }
    }

    if (warnings.total() > 0) {
        std::cerr << "PhilipsParser: session " << selected->session_id
                  << " parse warnings: crc=" << warnings.crc_failures
                  << " truncated=" << warnings.truncated_chunks
                  << " unknown_events=" << warnings.unknown_event_codes
                  << " unknown_records=" << warnings.unknown_record_codes
                  << " unsupported=" << warnings.unsupported_chunks << std::endl;
    }
    session->parse_warnings = warnings.total();

    session->status = ParsedSession::Status::COMPLETED;
    session->file_complete = true;
    // A Philips session's breathing figures come from its breaths, not its
    // minute means (calculateMetrics, applyBreathMetrics). Events and the AHI
    // stay the .002/.B02 file's alone: machine_reported (the summary's own
    // tally) is a cross-check and never becomes an event.
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
