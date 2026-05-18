#pragma once

#ifdef CPAPDASH_WITH_LOWENSTEIN

#include "cpapdash/parser/ISessionParser.h"
#include "cpapdash/parser/Models.h"
#include <string>
#include <memory>
#include <optional>
#include <chrono>
#include <map>

namespace cpapdash::parser {

struct PrismaDeviceInfo {
    std::string serial_number;
    int device_type = 0;
    std::string fw_version;
    std::string fw_build;
    int hw_version = 0;
};

class PrismaParser : public ISessionParser {
public:
    std::unique_ptr<ParsedSession> parseSession(
        const std::string& session_dir,
        const std::string& device_id,
        const std::string& device_name,
        std::optional<std::chrono::system_clock::time_point> session_start = std::nullopt
    ) override;

    std::unique_ptr<ParsedSession> parseSessionFromBuffers(
        const std::map<std::string, std::pair<const uint8_t*, size_t>>& buffers,
        const std::string& device_id,
        const std::string& device_name,
        const std::string& session_start_str = ""
    ) override;

    DeviceManufacturer manufacturer() const override {
        return DeviceManufacturer::LOWENSTEIN;
    }

    static PrismaDeviceInfo parseDeviceXml(const std::string& filepath);
    static bool parseEventXml(const std::string& filepath, ParsedSession& session,
                              std::chrono::system_clock::time_point session_start);
    static bool parseSignalWmedf(const std::string& filepath, ParsedSession& session);
    static std::optional<EventType> mapRespEventId(int id);
    static std::optional<int> mapTherapyMode(int prisma_mode);
};

std::unique_ptr<ISessionParser> createLowensteinParser();

} // namespace cpapdash::parser

#endif // CPAPDASH_WITH_LOWENSTEIN
