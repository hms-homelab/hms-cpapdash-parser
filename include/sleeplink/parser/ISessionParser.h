#pragma once

#include "sleeplink/parser/Models.h"
#include <string>
#include <memory>
#include <optional>
#include <chrono>
#include <map>
#include <utility>
#include <cstdint>

namespace sleeplink::parser {

/**
 * ISessionParser - Abstract interface for CPAP session parsing.
 *
 * Implementations:
 *   ResMedParser  - wraps EDFParser (EDF+ files from ResMed AirSense 10/11)
 *   PhilipsParser - PRS1 binary files from Philips DreamStation 2 (requires SLEEPLINK_WITH_PHILIPS)
 */
class ISessionParser {
public:
    virtual ~ISessionParser() = default;

    /**
     * Parse a session from files on disk.
     *
     * @param session_dir   Directory containing session data files
     * @param device_id     Device identifier for MQTT/DB
     * @param device_name   Human-readable device name
     * @param session_start Optional timestamp hint from filename
     * @return Parsed session, or nullptr on failure
     */
    virtual std::unique_ptr<ParsedSession> parseSession(
        const std::string& session_dir,
        const std::string& device_id,
        const std::string& device_name,
        std::optional<std::chrono::system_clock::time_point> session_start = std::nullopt
    ) = 0;

    /**
     * Parse a session from in-memory buffers.
     *
     * @param buffers       Map of file extension/name -> (data pointer, length)
     * @param device_id     Device identifier
     * @param device_name   Human-readable device name
     * @param session_start Optional session start as string (e.g., "20260206_021037")
     * @return Parsed session, or nullptr on failure
     */
    virtual std::unique_ptr<ParsedSession> parseSessionFromBuffers(
        const std::map<std::string, std::pair<const uint8_t*, size_t>>& buffers,
        const std::string& device_id,
        const std::string& device_name,
        const std::string& session_start_str = ""
    ) = 0;

    /** Which manufacturer this parser handles. */
    virtual DeviceManufacturer manufacturer() const = 0;
};

/**
 * Create a parser by auto-detecting the device type from directory contents.
 *
 * Detection:
 *   - P-SERIES/ folder -> Philips (returns PhilipsParser if SLEEPLINK_WITH_PHILIPS, else nullptr)
 *   - .edf files or DATALOG/ -> ResMed
 *
 * @param data_dir  Root of SD card or data directory
 * @return Parser instance, or nullptr if unrecognized
 */
std::unique_ptr<ISessionParser> createParser(const std::string& data_dir);

/**
 * Create a parser for a known manufacturer.
 *
 * @return Parser instance, or nullptr if the manufacturer is not compiled in
 */
std::unique_ptr<ISessionParser> createParser(DeviceManufacturer manufacturer);

} // namespace sleeplink::parser
