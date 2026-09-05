#include "cpapdash/parser/ISessionParser.h"
#include "cpapdash/parser/EDFParser.h"

#include <algorithm>
#include <cctype>

// Cross-platform directory scanning via std::filesystem (C++17)
#include <filesystem>

namespace cpapdash::parser {

namespace {

namespace fs = std::filesystem;

bool hasSubdirCaseInsensitive(const std::string& parent, const std::string& target) {
    std::error_code ec;
    if (!fs::is_directory(parent, ec)) return false;

    std::string target_lower = target;
    std::transform(target_lower.begin(), target_lower.end(), target_lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    for (const auto& entry : fs::directory_iterator(parent, ec)) {
        if (!entry.is_directory(ec)) continue;
        std::string name = entry.path().filename().string();
        std::string name_lower = name;
        std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (name_lower == target_lower) return true;
    }
    return false;
}

bool hasFileWithExtension(const std::string& dir_path, const std::string& ext) {
    std::error_code ec;
    if (!fs::is_directory(dir_path, ec)) return false;

    for (const auto& entry : fs::directory_iterator(dir_path, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        std::string name = entry.path().filename().string();
        if (name.size() > ext.size() &&
            name.compare(name.size() - ext.size(), ext.size(), ext) == 0) {
            return true;
        }
    }
    return false;
}

std::string toLower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return out;
}

std::string basenameOf(const std::string& path) {
    auto slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool endsWithCI(const std::string& s, const std::string& suffix_lower) {
    if (s.size() < suffix_lower.size()) return false;
    return toLower(s.substr(s.size() - suffix_lower.size())) == suffix_lower;
}

// BMC / React Health-3B Luna identity file: a serial-named basename matching
// "\d\dC\d{5}.usr" (e.g. "16C01034.usr").
bool isBmcUsrFilename(const std::string& filename) {
    std::string base = basenameOf(filename);
    if (base.size() != 12) return false;  // NNCNNNNN.usr
    if (!std::isdigit(static_cast<unsigned char>(base[0])) ||
        !std::isdigit(static_cast<unsigned char>(base[1]))) return false;
    if (std::tolower(static_cast<unsigned char>(base[2])) != 'c') return false;
    for (int i = 3; i <= 7; ++i)
        if (!std::isdigit(static_cast<unsigned char>(base[i]))) return false;
    if (base[8] != '.') return false;
    return toLower(base.substr(9)) == "usr";
}

// Philips Respironics identity manifest.
bool isPropertiesTxtFilename(const std::string& filename) {
    return toLower(basenameOf(filename)) == "properties.txt";
}

// "DATA_7" and the like -- a Sefam session folder.
bool isSefamSessionFolderName(const std::string& name) {
    const std::string n = toLower(name);
    if (n.rfind("data_", 0) != 0 || n.size() <= 5) return false;
    for (size_t i = 5; i < n.size(); ++i)
        if (!std::isdigit(static_cast<unsigned char>(n[i]))) return false;
    return true;
}

// Sefam S.Box session manifest: "DATA_<n>/DATA_<n>.INI".
//
// A bare ".ini" is far too common a name to gate a brand on. What makes this one
// safe is that the file is named after the folder holding it, so the pair is the
// signature rather than the extension. Where the caller passed a bare filename
// with no directory component there is nothing to cross-check against, and the
// "DATA_<digits>" stem alone carries it.
bool isSefamSessionIniPath(const std::string& path) {
    const std::string base = basenameOf(path);
    if (!endsWithCI(base, ".ini")) return false;

    const std::string stem = base.substr(0, base.size() - 4);
    if (!isSefamSessionFolderName(stem)) return false;

    auto slash = path.find_last_of("/\\");
    if (slash == std::string::npos) return true;  // no parent to check

    std::string parent = path.substr(0, slash);
    auto up = parent.find_last_of("/\\");
    if (up != std::string::npos) parent = parent.substr(up + 1);

    return toLower(parent) == toLower(stem);
}

// Directory-scoped version of a per-filename predicate (shallow, non-recursive --
// matches the existing hasFileWithExtension/hasSubdirCaseInsensitive scope).
bool hasMatchingFilename(const std::string& dir_path, bool (*pred)(const std::string&)) {
    std::error_code ec;
    if (!fs::is_directory(dir_path, ec)) return false;
    for (const auto& entry : fs::directory_iterator(dir_path, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        if (pred(entry.path().filename().string())) return true;
    }
    return false;
}

// A Sefam session folder sits two levels down from the card root
// (<model>/<serial>/DATA_<n>/), so unlike every other brand here it cannot be
// found by looking in one directory. Three levels reaches it from the card root,
// from the serial folder, and from the session folder itself, which are the three
// places a caller plausibly points at.
constexpr int kSefamSearchDepth = 3;

bool hasSefamSessionFolder(const std::string& dir_path, int depth) {
    std::error_code ec;
    if (depth < 0 || !fs::is_directory(dir_path, ec)) return false;

    // The folder we were handed may be the session folder.
    const std::string self = fs::path(dir_path).filename().string();
    if (isSefamSessionFolderName(self) &&
        fs::is_regular_file(fs::path(dir_path) / (self + ".INI"), ec)) return true;
    if (isSefamSessionFolderName(self) &&
        fs::is_regular_file(fs::path(dir_path) / (self + ".ini"), ec)) return true;

    for (const auto& entry : fs::directory_iterator(dir_path, ec)) {
        if (!entry.is_directory(ec)) continue;
        if (hasSefamSessionFolder(entry.path().string(), depth - 1)) return true;
    }
    return false;
}

} // anonymous namespace

DeviceManufacturer detectManufacturer(const std::vector<std::string>& filenames) {
    bool has_wmedf = false, has_sefam_ini = false, has_bmc_usr = false;
    bool has_properties = false, has_edf = false;
    for (const auto& f : filenames) {
        if (!has_wmedf && endsWithCI(f, ".wmedf")) has_wmedf = true;
        if (!has_sefam_ini && isSefamSessionIniPath(f)) has_sefam_ini = true;
        if (!has_bmc_usr && isBmcUsrFilename(f)) has_bmc_usr = true;
        if (!has_properties && isPropertiesTxtFilename(f)) has_properties = true;
        if (!has_edf && endsWithCI(basenameOf(f), ".edf")) has_edf = true;
    }
    if (has_wmedf) return DeviceManufacturer::LOWENSTEIN;
    if (has_sefam_ini) return DeviceManufacturer::SEFAM;
    if (has_bmc_usr) return DeviceManufacturer::BMC;
    if (has_properties) return DeviceManufacturer::PHILIPS;
    if (has_edf) return DeviceManufacturer::RESMED;
    return DeviceManufacturer::UNKNOWN;
}

DeviceManufacturer detectManufacturer(const std::string& data_dir) {
    if (hasFileWithExtension(data_dir, ".wmedf")) return DeviceManufacturer::LOWENSTEIN;
    if (hasSefamSessionFolder(data_dir, kSefamSearchDepth)) return DeviceManufacturer::SEFAM;
    if (hasMatchingFilename(data_dir, isBmcUsrFilename)) return DeviceManufacturer::BMC;
    if (hasMatchingFilename(data_dir, isPropertiesTxtFilename)) return DeviceManufacturer::PHILIPS;
    if (hasSubdirCaseInsensitive(data_dir, "DATALOG") || hasFileWithExtension(data_dir, ".edf"))
        return DeviceManufacturer::RESMED;
    return DeviceManufacturer::UNKNOWN;
}

/**
 * ResMedParser - Wraps the static EDFParser API behind ISessionParser.
 */
class ResMedParser : public ISessionParser {
public:
    std::unique_ptr<ParsedSession> parseSession(
        const std::string& session_dir,
        const std::string& device_id,
        const std::string& device_name,
        std::optional<std::chrono::system_clock::time_point> session_start) override
    {
        auto session = EDFParser::parseSession(session_dir, device_id, device_name, session_start);
        if (session) {
            session->manufacturer = DeviceManufacturer::RESMED;
        }
        return session;
    }

    std::unique_ptr<ParsedSession> parseSessionFromBuffers(
        const std::map<std::string, std::pair<const uint8_t*, size_t>>& buffers,
        const std::string& device_id,
        const std::string& device_name,
        const std::string& session_start_str) override
    {
        // Map from generic buffer map to EDFParser's positional buffer API
        const uint8_t* brp = nullptr; size_t brp_len = 0;
        const uint8_t* pld = nullptr; size_t pld_len = 0;
        const uint8_t* sad = nullptr; size_t sad_len = 0;
        const uint8_t* eve = nullptr; size_t eve_len = 0;

        for (const auto& [key, buf] : buffers) {
            std::string k = key;
            std::transform(k.begin(), k.end(), k.begin(),
                           [](unsigned char c) { return std::toupper(c); });
            if (k.find("BRP") != std::string::npos) { brp = buf.first; brp_len = buf.second; }
            else if (k.find("PLD") != std::string::npos) { pld = buf.first; pld_len = buf.second; }
            else if (k.find("SAD") != std::string::npos) { sad = buf.first; sad_len = buf.second; }
            else if (k.find("EVE") != std::string::npos) { eve = buf.first; eve_len = buf.second; }
        }

        auto session = EDFParser::parseSessionFromBuffers(
            brp, brp_len, pld, pld_len, sad, sad_len, eve, eve_len,
            device_id, device_name, session_start_str);

        if (session) {
            session->manufacturer = DeviceManufacturer::RESMED;
        }
        return session;
    }

    DeviceManufacturer manufacturer() const override {
        return DeviceManufacturer::RESMED;
    }
};

// ── Factory functions ────────────────────────────────────────────────────────

std::unique_ptr<ISessionParser> createParser(const std::string& data_dir) {
    return createParser(detectManufacturer(data_dir));
}

std::unique_ptr<ISessionParser> createParser(DeviceManufacturer manufacturer) {
    switch (manufacturer) {
        case DeviceManufacturer::RESMED:
            return std::make_unique<ResMedParser>();

        case DeviceManufacturer::LOWENSTEIN:
#ifdef CPAPDASH_WITH_LOWENSTEIN
        {
            extern std::unique_ptr<ISessionParser> createLowensteinParser();
            return createLowensteinParser();
        }
#else
            return nullptr;
#endif

        case DeviceManufacturer::SEFAM:
#ifdef CPAPDASH_WITH_SEFAM
        {
            extern std::unique_ptr<ISessionParser> createSefamParser();
            return createSefamParser();
        }
#else
            return nullptr;
#endif

        default:
            return nullptr;
    }
}

} // namespace cpapdash::parser
