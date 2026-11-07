#pragma once

#ifdef CPAPDASH_WITH_PHILIPS

// PhilipsParser - Philips Respironics PRS1 (System One, DreamStation 1, and
// decrypted DreamStation 2)
//
// PROVENANCE: implemented exclusively from docs/PHILIPS_FORMAT.md, our cited
// transcription of
//   [A] Apnea Board wiki "PR System One Data Format"
//   [B] cpap-lib (MIT, StagPoint) @ f760b21205e0d3f49342eacf2eed11912881aa01
//   [C] mattjoyce/dreamstation docs/FORMAT.md (Apache-2.0) @ 268d786, format v3
//       only, every fact checked on a real card [S] (doc section 9, SDD-009)
// OSCAR / SleepyHead source was never consulted and must never be. This parser
// never decrypts: an encrypted DreamStation 2 card is reported by
// isEncryptedCard() and left to a separate decryption step.

#include "cpapdash/parser/ISessionParser.h"
#include "cpapdash/parser/Models.h"
#include <string>
#include <memory>
#include <optional>
#include <chrono>
#include <map>
#include <vector>
#include <array>
#include <cstdint>

namespace cpapdash::parser {

/**
 * Identity manifest: Properties.txt (v2, doc section 2) or PROP.TXT / PROP.BIN
 * (v3, doc 9.1, short keys).
 */
struct PhilipsDeviceInfo {
    std::string serial_number;
    std::string model_number;                 // e.g. "560P", "500X110", "520X110C"
    std::optional<int> family;
    std::optional<int> family_version;
    std::optional<std::string> data_format_version;
    std::optional<int> patient_folder_num;    // PatientFolderNum (v2) / FN (v3)
    std::string software_version;             // SV (v3), "" when absent

    // v3 PROP.BIN keys of unknown meaning that look like key material
    // (SDD-009 D2): kept verbatim so a SEPARATE decryption step can use them.
    // Never copied into ParsedSession, never logged.
    std::string sid, bk, sk, ek, dk;
};

/** Never-fatal parse problem counters (doc: skip corrupt, keep session). */
struct PhilipsParseWarnings {
    int crc_failures = 0;         // data-CRC mismatch, chunk skipped
    int truncated_chunks = 0;     // header/body ran past EOF, stop clean
    int unknown_event_codes = 0;  // v2: unknown .002 code, rest of block abandoned
    int unknown_record_codes = 0; // v3: unknown record code, skipped by its size
    int unsupported_chunks = 0;   // unknown format version, sub-V4 schema, nsig>1, ...

    int total() const {
        return crc_failures + truncated_chunks + unknown_event_codes +
               unknown_record_codes + unsupported_chunks;
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

    // Signal extra header (doc 3.2; v3 doc 9.2), FileType 1 only
    struct WaveformInfo {
        uint8_t sample_format = 0;
        uint16_t interleave = 0;
        uint8_t extra = 0;         // v3 only: 4th byte of the entry (8 on [S]; meaning OPEN)
    };
    uint16_t interval_records = 0;
    uint8_t seconds_per_interval = 0;
    std::vector<WaveformInfo> waveforms;

    // v3 FileType 0 header block (doc 9.2): record size per code, -1 = code absent.
    // All -1 on a v2 chunk, whose record sizes are implied by the code tables.
    std::array<int16_t, 256> record_size = makeAbsentSizes();

    std::vector<uint8_t> data;     // payload (CRC already verified)

    static std::array<int16_t, 256> makeAbsentSizes() {
        std::array<int16_t, 256> a{};
        a.fill(-1);
        return a;
    }

    /** Block duration in seconds (signal chunks; 0 for standard chunks). */
    int durationSeconds() const {
        return file_type == 1 ? interval_records * seconds_per_interval : 0;
    }
};

/**
 * One session's file set as discovered on disk, grouped by the session number in
 * the chunk headers (doc 9.3), not by file name: a file can hold chunks of
 * several sessions, and a session's chunks can sit in several files.
 */
struct PhilipsSessionFiles {
    std::string session_id;     // file stem: 10 decimal digits (v2) or 8 hex digits (v3)
    uint32_t session_number = 0;
    std::string summary_path;   // first .001 / .B01 holding this session, "" if absent
    std::string events_path;    // first .002 / .B02, "" if absent
    std::string waveform_path;  // first .005 / .B05, "" if absent
    // Every file holding a chunk of this session, per kind (the *_path above is
    // the first of each).
    std::vector<std::string> summary_files, events_files, waveform_files;
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
    /**
     * Parse the identity manifest: Properties.txt (long keys) or PROP.TXT /
     * decrypted PROP.BIN (short keys, doc 9.1). key=value, stops at the first
     * empty line. nullopt when the file is missing or holds no key=value line.
     */
    static std::optional<PhilipsDeviceInfo> parseProperties(const std::string& filepath);

    /** ModelNumber -> product name (doc section 2 table; MIT cpap-lib derived). */
    static std::optional<std::string> modelName(const std::string& model_number);

    /**
     * Frame + validate all chunks in a buffer (doc section 3; v3 doc 9.2).
     * DataFormatVersion 2 and 3 are framed; any other version is counted as
     * unsupported and skipped. Chunks failing the data CRC are counted and
     * omitted; a truncated tail stops cleanly.
     */
    static std::vector<PhilipsChunk> readChunks(const uint8_t* data, size_t len,
                                                PhilipsParseWarnings& warnings);

    /**
     * The v3 data checksum (doc 9.2): CRC-32, MSB first, polynomial 0x04C11DB7,
     * initial value 0xFFFFFFFF, no final inversion, each byte fed as the 32-bit
     * word 00 00 00 bb. The machine uses the same CRC for LOG.SEQ and the VC key
     * of PROP.BIN (doc 9.10).
     */
    static uint32_t crc32v3(const uint8_t* data, size_t len);

    /**
     * Assemble one session from its chunks, already filtered to that session:
     * identity (when info is given), summary, events, waveform, metrics. The
     * core of parseSession, shared with the Bluetooth record path
     * (PhilipsBleCard.h) so both build a night the same way. nullptr when the
     * chunks yield no session start.
     */
    static std::unique_ptr<ParsedSession> sessionFromChunks(
        const std::vector<PhilipsChunk>& summary_chunks,
        const std::vector<PhilipsChunk>& events_chunks,
        const std::vector<PhilipsChunk>& waveform_chunks,
        const PhilipsDeviceInfo* info,
        const std::string& device_id,
        const std::string& device_name,
        PhilipsParseWarnings& warnings);

    /**
     * Discover the sessions in dir: .001/.002/.005 with 10-decimal-digit stems
     * (v2), and .B01/.B02/.B05 or .001/.002/.005 with 8-hex-digit stems (v3).
     * Grouped by the session number in the chunk headers, sorted ascending.
     */
    static std::vector<PhilipsSessionFiles> discoverSessions(const std::string& dir);

    /**
     * True when dir (or its parent) holds a DreamStation 2 card that is still
     * encrypted: a PROP.BIN that is not plain key=value text, or session files
     * that frame as v3 but fail every data CRC. This parser never decrypts; the
     * caller reports "needs decrypting" instead of a generic failure (SDD-009 D1).
     */
    static bool isEncryptedCard(const std::string& dir);

    // ── Phase 2 modules ──────────────────────────────────────────────
    /**
     * .001 / .B01 summary. Dispatches per chunk: v2 FamilyVersion >= 4 (doc
     * section 5; older schemas counted unsupported, doc Q3) or v3 FamilyVersion 6
     * (doc 9.5, parseSummaryV3). Fills session start/end, settings, and
     * duration_seconds as the SUM of mask-on periods (SDD-009 D12).
     */
    static bool parseSummary(const std::vector<PhilipsChunk>& chunks,
                             ParsedSession& session,
                             PhilipsParseWarnings& warnings);

    /**
     * One v3 FamilyVersion 6 summary chunk (doc 9.5-9.7): time records, settings,
     * statistics summed into session.machine_reported. Appends this chunk's
     * mask-on spans to mask_on_spans. Unknown codes are skipped by size and
     * counted. false when the chunk has no usable mask-on period.
     */
    static bool parseSummaryV3(const PhilipsChunk& chunk,
                               ParsedSession& session,
                               std::vector<std::pair<std::chrono::system_clock::time_point,
                                                     std::chrono::system_clock::time_point>>&
                                   mask_on_spans,
                               PhilipsParseWarnings& warnings);

    /** Machine-graphed stat sample from .002 code 0x11 (doc 4.2). */
    struct EventStat {
        std::chrono::system_clock::time_point timestamp;
        double total_leak = 0;   // L/min, TOTAL (vent flow included): BreathingSummary::total_leak
        double snore = 0;        // count/index
        double epap = 0;         // cmH2O
    };

    /**
     * .002 / .B02 events. Dispatches per chunk: v2 FamilyVersion >= 4 (doc 4.2;
     * table only, doc Q2) or v3 FamilyVersion 6 (doc 9.8, parseEventsV3).
     * Emits onset-stamped SleepEvents (end->onset conversion done here).
     */
    static bool parseEvents(const std::vector<PhilipsChunk>& chunks,
                            ParsedSession& session,
                            std::vector<EventStat>& stats,
                            PhilipsParseWarnings& warnings);

    /**
     * One v3 FamilyVersion 6 events chunk (doc 9.8). Records framed by the
     * chunk's record_size table; 0x12 carries no time field; the 0x0b hypopnea
     * variant is emitted as EventType::OTHER (kept out of AHI, SDD-009 D10);
     * unknown codes skipped by size and counted.
     */
    static bool parseEventsV3(const PhilipsChunk& chunk,
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

    /**
     * A session's flow as one series (doc 6, 9.9): every usable single-signal
     * waveform chunk laid at its header time from `start`, gaps NaN (no zero
     * fill). `recorded_seconds` is the time the chunks cover, i.e. mask-on
     * time: a gap between chunks is the mask off (doc 9.9).
     */
    struct FlowSeries {
        std::chrono::system_clock::time_point start;
        double sample_rate = 0;            // Hz, from the first chunk (about 5)
        std::vector<double> flow;          // L/min, NaN where no chunk covers
        int recorded_seconds = 0;
        std::chrono::system_clock::time_point end() const {
            return start + std::chrono::milliseconds(static_cast<long long>(
                       sample_rate > 0 ? flow.size() * 1000.0 / sample_rate : 0));
        }
    };

    /**
     * The chunks' flow as one series anchored at `base` (the first chunk's time
     * when unset; a chunk before `base` is counted unsupported and left out).
     * nullopt when no chunk is usable flow.
     */
    static std::optional<FlowSeries> flowSeries(
        const std::vector<PhilipsChunk>& chunks, PhilipsParseWarnings& warnings,
        std::optional<std::chrono::system_clock::time_point> base = std::nullopt);

    /**
     * The flow of ONE session of the card in `session_dir`, by the session
     * number in its chunk headers, from every patient folder that holds a
     * waveform of it (doc 9.3). It needs no summary, so it reads a session the
     * machine is still recording: a DreamStation appends the open session's
     * waveform as it goes and writes the summary and events at mask-off. A
     * truncated last chunk (the file caught mid-write) is left out cleanly.
     * nullopt when the session has no usable flow.
     */
    static std::optional<FlowSeries> sessionFlow(const std::string& session_dir,
                                                 uint32_t session_number,
                                                 PhilipsParseWarnings& warnings);
};

std::unique_ptr<ISessionParser> createPhilipsParser();

} // namespace cpapdash::parser

#endif // CPAPDASH_WITH_PHILIPS
