#pragma once

#ifdef CPAPDASH_WITH_PHILIPS

#include "cpapdash/parser/ISessionParser.h"
#include "cpapdash/parser/PhilipsCrypto.h"
#include "cpapdash/parser/PhilipsChunk.h"
#include <memory>
#include <string>

namespace cpapdash::parser {

/**
 * PhilipsParser - Parser for Philips DreamStation 2 (PRS1 F0V6) CPAP data.
 *
 * Handles:
 *   .B01 / .001 — Session summary (settings, mask on/off, duration)
 *   .B02 / .002 — Events (OA, CA, HY, FL, RERA, PB, snore, leak, stats)
 *   .B05 / .005 — Waveform (flow, mask pressure) [deferred]
 *
 * DS2 files (.B##) are AES-256-GCM encrypted; plain (.0##) files also supported.
 */
class PhilipsParser : public ISessionParser {
public:
    PhilipsParser();

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
        return DeviceManufacturer::PHILIPS;
    }

    // ── Individual file parsers (exposed for testing) ────────────────────────

    /** Parse .001 summary chunks into session metadata + settings. */
    static bool parseSummary(const std::vector<std::pair<PRS1ChunkHeader, std::vector<uint8_t>>>& chunks,
                             ParsedSession& session);

    /** Parse .002 event chunks into events + breathing summaries. */
    static bool parseEvents(const std::vector<std::pair<PRS1ChunkHeader, std::vector<uint8_t>>>& chunks,
                            ParsedSession& session);

private:
    PhilipsCrypto crypto_;

    // Get decrypted data from a file (handles both .B## encrypted and .0## plain)
    std::vector<uint8_t> getDecryptedData(const std::string& filepath);
    std::vector<uint8_t> getDecryptedData(const uint8_t* data, size_t len,
                                          const std::string& extension);
};

/** Factory function called by createParser(DeviceManufacturer::PHILIPS). */
std::unique_ptr<ISessionParser> createPhilipsParser();

} // namespace cpapdash::parser

#endif // CPAPDASH_WITH_PHILIPS
