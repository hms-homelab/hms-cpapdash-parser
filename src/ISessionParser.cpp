#include "sleeplink/parser/ISessionParser.h"
#include "sleeplink/parser/EDFParser.h"

#include <algorithm>
#include <cctype>

// Minimal filesystem detection without <filesystem> (C++17 compat with older toolchains)
#include <sys/stat.h>
#include <dirent.h>

namespace sleeplink::parser {

namespace {

bool directoryExists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool hasSubdirCaseInsensitive(const std::string& parent, const std::string& target) {
    DIR* dir = opendir(parent.c_str());
    if (!dir) return false;

    std::string target_lower = target;
    std::transform(target_lower.begin(), target_lower.end(), target_lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    bool found = false;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name(entry->d_name);
        std::string name_lower = name;
        std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (name_lower == target_lower) {
            std::string full = parent + "/" + name;
            if (directoryExists(full)) {
                found = true;
                break;
            }
        }
    }
    closedir(dir);
    return found;
}

bool hasFileWithExtension(const std::string& dir_path, const std::string& ext) {
    DIR* dir = opendir(dir_path.c_str());
    if (!dir) return false;

    bool found = false;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name(entry->d_name);
        if (name.size() > ext.size() &&
            name.compare(name.size() - ext.size(), ext.size(), ext) == 0) {
            found = true;
            break;
        }
    }
    closedir(dir);
    return found;
}

} // anonymous namespace

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
    // Philips: look for P-SERIES/ directory (case-insensitive)
    if (hasSubdirCaseInsensitive(data_dir, "P-SERIES")) {
#ifdef SLEEPLINK_WITH_PHILIPS
        return createParser(DeviceManufacturer::PHILIPS);
#else
        return nullptr;  // Philips support not compiled in
#endif
    }

    // ResMed: look for .edf files or DATALOG/ directory
    if (hasSubdirCaseInsensitive(data_dir, "DATALOG") ||
        hasFileWithExtension(data_dir, ".edf")) {
        return std::make_unique<ResMedParser>();
    }

    return nullptr;
}

std::unique_ptr<ISessionParser> createParser(DeviceManufacturer manufacturer) {
    switch (manufacturer) {
        case DeviceManufacturer::RESMED:
            return std::make_unique<ResMedParser>();

        case DeviceManufacturer::PHILIPS:
#ifdef SLEEPLINK_WITH_PHILIPS
            // Will be implemented in PhilipsParser.cpp
            // Forward declaration resolved at link time
            extern std::unique_ptr<ISessionParser> createPhilipsParser();
            return createPhilipsParser();
#else
            return nullptr;
#endif

        default:
            return nullptr;
    }
}

} // namespace sleeplink::parser
