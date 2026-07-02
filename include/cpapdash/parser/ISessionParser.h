#pragma once

#include "cpapdash/parser/Models.h"
#include <string>
#include <memory>
#include <optional>
#include <chrono>
#include <map>
#include <utility>
#include <cstdint>
#include <vector>

namespace cpapdash::parser {

/**
 * ISessionParser - Abstract interface for CPAP session parsing.
 *
 * Implementations:
 *   ResMedParser     - wraps EDFParser (EDF+ files from ResMed AirSense 10/11)
 *   LowensteinParser - Prisma Smart/Line (WMEDF signals + XML events; requires CPAPDASH_WITH_LOWENSTEIN)
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
 * Detect the machine brand from a set of filenames/paths. Filename/path pattern
 * matching ONLY -- no file content is read. Ordered unambiguous-first, first match
 * wins:
 *   1. LOWENSTEIN -- any filename ends ".wmedf"
 *   2. BMC        -- any filename matches the serial-named identity file
 *                    "\d\dC\d{5}.usr" (React Health/3B Luna share this format)
 *   3. PHILIPS    -- any filename is "properties.txt" (the Properties.txt
 *                    identity manifest; gated on this, not on .001/.002, which
 *                    collide with BMC's raw-data numbering)
 *   4. RESMED     -- "STR.edf" present, or a DATALOG subdir, or any .edf file
 *   5. else UNKNOWN
 *
 * Detection is broader than parse support: PHILIPS and BMC are named here even
 * though createParser() has no parser for them (returns nullptr for both).
 */
DeviceManufacturer detectManufacturer(const std::vector<std::string>& filenames);

/** Directory-walk overload: collects filenames under data_dir, then detects. */
DeviceManufacturer detectManufacturer(const std::string& data_dir);

/**
 * Create a parser by auto-detecting the device type from directory contents
 * (via detectManufacturer(data_dir), then createParser(DeviceManufacturer)).
 *
 * @param data_dir  Root of SD card or data directory
 * @return Parser instance, or nullptr if unrecognized or not parse-supported
 */
std::unique_ptr<ISessionParser> createParser(const std::string& data_dir);

/**
 * Create a parser for a known manufacturer.
 *
 * @return Parser instance, or nullptr if the manufacturer is not compiled in
 *         or has no parser (PHILIPS, BMC, UNKNOWN)
 */
std::unique_ptr<ISessionParser> createParser(DeviceManufacturer manufacturer);

} // namespace cpapdash::parser
