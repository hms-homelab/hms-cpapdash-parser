#pragma once

#ifdef CPAPDASH_WITH_PHILIPS

// PhilipsParser - Philips Respironics PRS1 (System One / DreamStation 1)
//
// PROVENANCE (clean-room, SDD-002): implemented exclusively from
// docs/PHILIPS_FORMAT.md, our cited transcription of
//   [A] Apnea Board wiki "PR System One Data Format"
//   [B] cpap-lib (MIT, StagPoint) @ f760b21205e0d3f49342eacf2eed11912881aa01
// OSCAR / SleepyHead source was never consulted and must never be.

#include "cpapdash/parser/ISessionParser.h"
#include "cpapdash/parser/Models.h"
#include <string>
#include <memory>
#include <optional>
#include <chrono>
#include <map>
#include <vector>
#include <cstdint>

namespace cpapdash::parser {

/** Properties.txt identity manifest (doc section 2). */
struct PhilipsDeviceInfo {
    std::string serial_number;
    std::string model_number;                 // e.g. "560P", "500X110"
    std::optional<int> family;
    std::optional<int> family_version;
    std::optional<std::string> data_format_version;
    std::optional<int> patient_folder_num;
};

/** Never-fatal parse problem counters (doc: skip corrupt, keep session). */
struct PhilipsParseWarnings {
    int crc_failures = 0;        // data-CRC mismatch, chunk skipped
    int truncated_chunks = 0;    // header/body ran past EOF, stop clean
    int unknown_event_codes = 0; // unknown .002 code, rest of block abandoned
    int unsupported_chunks = 0;  // format v!=2, sub-V4 schema, nsig>1, ...

    int total() const {
        return crc_failures + truncated_chunks + unknown_event_codes + unsupported_chunks;
    }
};

/** One framed, checksum-validated chunk (doc section 3). */
struct PhilipsChunk {
    // Standard 15-byte header (doc 3.1)
    uint8_t data_format_version = 0;
    uint16_t block_length = 0;
    uint8_t file_type = 0;         // 0 standard, 1 signal
    uint8_t family = 0;
    uint8_t family_version = 0;
    uint8_t file_extension = 0;
    uint32_t session_number = 0;
    uint32_t timestamp_raw = 0;    // unix epoch seconds (doc 8 Q1: treated as UTC)
    std::chrono::system_clock::time_point timestamp;

    // Signal extra header (doc 3.2), FileType 1 only
    struct WaveformInfo {
        uint8_t sample_format = 0;
        uint16_t interleave = 0;
    };
    uint16_t interval_records = 0;
    uint8_t seconds_per_interval = 0;
    std::vector<WaveformInfo> waveforms;

    std::vector<uint8_t> data;     // payload (CRC already verified)

    /** Block duration in seconds (signal chunks; 0 for standard chunks). */
    int durationSeconds() const {
        return file_type == 1 ? interval_records * seconds_per_interval : 0;
    }
};

/** One session id's file set as discovered on disk. */
struct PhilipsSessionFiles {
    std::string session_id;    // 10-digit stem
    std::string summary_path;  // .001, "" if absent
    std::string events_path;   // .002, "" if absent
    std::string waveform_path; // .005, "" if absent
};

class PhilipsParser : public ISessionParser {
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
        return DeviceManufacturer::PHILIPS;
    }

    // ── Phase 1 modules ──────────────────────────────────────────────
    /** Parse Properties.txt (key=value, stops at first empty line). */
    static std::optional<PhilipsDeviceInfo> parseProperties(const std::string& filepath);

    /** ModelNumber -> product name (doc section 2 table; MIT cpap-lib derived). */
    static std::optional<std::string> modelName(const std::string& model_number);

    /**
     * Frame + validate all chunks in a buffer (doc section 3).
     * Chunks failing the data CRC are counted and omitted; a truncated tail
     * stops cleanly. Non-v2 chunks are counted as unsupported and skipped.
     */
    static std::vector<PhilipsChunk> readChunks(const uint8_t* data, size_t len,
                                                PhilipsParseWarnings& warnings);

    /** Group <10-digit>.001/.002/.005 files in dir by session id (sorted ascending). */
    static std::vector<PhilipsSessionFiles> discoverSessions(const std::string& dir);

    // ── Phase 2 modules ──────────────────────────────────────────────
    /**
     * .001 summary (doc section 5, FamilyVersion >= 4 only; older schemas are
     * counted as unsupported and skipped pending real-card confirmation, doc Q3).
     * Fills session start/end/duration/settings.
     */
    static bool parseSummary(const std::vector<PhilipsChunk>& chunks,
                             ParsedSession& session,
                             PhilipsParseWarnings& warnings);

    /** Machine-graphed stat sample from .002 code 0x11 (doc 4.2). */
    struct EventStat {
        std::chrono::system_clock::time_point timestamp;
        double total_leak = 0;   // L/min
        double snore = 0;        // count/index
        double epap = 0;         // cmH2O
    };

    /**
     * .002 events (doc section 4; FamilyVersion >= 4 table only, doc Q2).
     * Emits onset-stamped SleepEvents (end->onset conversion done here).
     */
    static bool parseEvents(const std::vector<PhilipsChunk>& chunks,
                            ParsedSession& session,
                            std::vector<EventStat>& stats,
                            PhilipsParseWarnings& warnings);

    /** Family 0 V4 event code -> unified EventType (nullopt = not an event / unmapped). */
    static std::optional<EventType> mapFamily0EventCode(uint8_t code);

    /** PRS1 operating-mode byte -> DeviceSettings.therapy_mode (0/1/2). */
    static std::optional<int> mapTherapyMode(uint8_t prs1_mode);

    // ── Phase 3 module ───────────────────────────────────────────────
    /**
     * .005 flow waveform (doc section 6): flow only, nsig==1 chunks only
     * (nsig>1 counted as unsupported, doc Q4). Builds per-minute
     * BreathingSummary and breath-by-breath detail from one pass of the
     * shared EDFParser::detectBreaths, as the ResMed BRP path does.
     */
    static bool parseWaveform(const std::vector<PhilipsChunk>& chunks,
                              ParsedSession& session,
                              PhilipsParseWarnings& warnings);
};

std::unique_ptr<ISessionParser> createPhilipsParser();

} // namespace cpapdash::parser

#endif // CPAPDASH_WITH_PHILIPS
