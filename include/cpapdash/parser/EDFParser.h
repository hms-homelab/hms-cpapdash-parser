#pragma once

#include "cpapdash/parser/EDFFile.h"
#include "cpapdash/parser/Models.h"
#include <string>
#include <memory>
#include <vector>
#include <chrono>
#include <cstdint>
#include <optional>

namespace cpapdash::parser {

/**
 * EDFParser - Parser for ResMed CPAP EDF files
 *
 * Uses raw EDF parsing (not edflib) for ResMed compatibility.
 * Handles incomplete/growing files gracefully.
 *
 * File types:
 *   BRP.edf  Breathing pattern (Flow @ 25 Hz, Pressure @ 25 Hz)
 *   EVE.edf  Events / annotations (Apnea, Hypopnea, Arousal)
 *   SAD.edf  SpO2 @ 1 Hz, Heart Rate @ 1 Hz
 *   PLD.edf  Pressure/Load (9 channels @ 0.5 Hz)
 *   CSL.edf  Clinical summary
 *   STR.edf  Daily therapy summaries (81 signals, 1 record/day)
 */
class EDFParser {
public:
    /**
     * Parse a session from file paths on disk.
     * session_dir should contain BRP/EVE/SAD/PLD/CSL EDF files.
     */
    static std::unique_ptr<ParsedSession> parseSession(
        const std::string& session_dir,
        const std::string& device_id,
        const std::string& device_name,
        std::optional<std::chrono::system_clock::time_point> session_start_from_filename = std::nullopt
    );

    /**
     * Parse a session from in-memory buffers.
     * Any buffer may be nullptr/0 if that file type is unavailable.
     * brp is required; others are optional.
     */
    /// A borrowed byte range. The caller owns the memory.
    struct ByteView {
        const uint8_t* data = nullptr;
        size_t size = 0;
    };

    /**
     * Every file a session is made of, in buffer form.
     *
     * A ResMed night is several mask-on blocks and EVERY type arrives as more
     * than one file: BRP/PLD/SAD checkpoints during the night, and an EVE/CSL
     * pair per block. A caller that reads files itself has to hand over all of
     * them, exactly as the directory form collects all of them.
     *
     * Keeping only one file per type -- the largest, say -- silently shortens
     * the night: the flow series loses every checkpoint but one, and the events
     * lose every block but one.
     *
     * ORDER MATTERS for the signal types. There are no filenames here to sort
     * by, so pass BRP/PLD/SAD in chronological order; the directory form sorts
     * them by name for the same reason. Events carry absolute timestamps and
     * are sorted after parsing regardless.
     */
    struct SessionBuffers {
        std::vector<ByteView> brp;   ///< chronological
        std::vector<ByteView> pld;   ///< chronological
        std::vector<ByteView> sad;   ///< chronological
        std::vector<ByteView> eve;   ///< any order
        std::vector<ByteView> csl;   ///< presence only, sets has_summary
    };

    static std::unique_ptr<ParsedSession> parseSessionFromBuffers(
        const SessionBuffers& buffers,
        const std::string& device_id,
        const std::string& device_name,
        const std::string& session_start_str = ""
    );

    /// Multi-EVE convenience overload, single BRP/PLD/SAD. Delegates.
    static std::unique_ptr<ParsedSession> parseSessionFromBuffers(
        const uint8_t* brp, size_t brp_len,
        const uint8_t* pld, size_t pld_len,
        const uint8_t* sad, size_t sad_len,
        const std::vector<ByteView>& eves,
        const std::string& device_id,
        const std::string& device_name,
        const std::string& session_start_str = ""
    );

    /// Single-EVE convenience overload. Delegates to the vector form.
    static std::unique_ptr<ParsedSession> parseSessionFromBuffers(
        const uint8_t* brp, size_t brp_len,
        const uint8_t* pld, size_t pld_len,
        const uint8_t* sad, size_t sad_len,
        const uint8_t* eve, size_t eve_len,
        const std::string& device_id,
        const std::string& device_name,
        const std::string& session_start_str = ""
    );

    /**
     * Parse STR.edf file containing daily therapy summaries.
     */
    static std::vector<STRDailyRecord> parseSTRFile(
        const std::string& filepath,
        const std::string& device_id);

    /**
     * Parse STR.edf from an in-memory buffer.
     */
    static std::vector<STRDailyRecord> parseSTRFromBuffer(
        const uint8_t* data, size_t len,
        const std::string& device_id);

    // ---- Breath analysis -------------------------------------------------
    // Public so it can be tested as a unit rather than only through a parsed
    // EDF fixture. The Philips fork hoists this block into its own
    // BreathAnalysis.{h,cpp}; this is the seam where the two reconcile.

    struct BreathCycle {
        int start_idx;
        int end_idx;
        double tidal_volume;
        double inspiratory_time;
        double expiratory_time;
        double flow_limitation;
    };

    static std::vector<BreathCycle> detectBreaths(
        const std::vector<double>& flow_data,
        double sample_rate
    );

    // Takes the breaths whose onset falls in this minute. The caller detects
    // breaths ONCE over the whole file and buckets them, so a breath straddling
    // a minute boundary is counted exactly once (SDD-003 D2).
    static void calculateRespiratoryMetrics(
        const std::vector<double>& flow_data,
        const std::vector<double>& pressure_data,
        const std::vector<BreathCycle>& breaths,
        double sample_rate,
        int minute_idx,
        BreathingSummary& summary
    );

private:
    static bool parseDeviceInfo(EDFFile& edf,
                                std::string& serial_number,
                                int& model_id,
                                int& version_id);

    static bool parseBRPFile(EDFFile& edf, ParsedSession& session);
    static bool parseEVEFile(EDFFile& edf, ParsedSession& session);
    static bool parseSADFile(EDFFile& edf, ParsedSession& session);
    static bool parsePLDFile(EDFFile& edf, ParsedSession& session);

    // Flow-based session boundary detection
    static void detectFlowBasedSessionBoundaries(
        const std::vector<double>& flow_data,
        double sample_rate,
        std::chrono::system_clock::time_point file_start,
        std::optional<std::chrono::system_clock::time_point>& actual_start,
        std::optional<std::chrono::system_clock::time_point>& actual_end,
        bool& session_active
    );

    static double calculatePercentile(
        const std::vector<double>& data,
        double percentile
    );

    // Internal STR parser working on an already-opened EDFFile
    static std::vector<STRDailyRecord> parseSTRInternal(
        EDFFile& edf,
        const std::string& device_id);
};

} // namespace cpapdash::parser
