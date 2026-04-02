#pragma once

#include "sleeplink/parser/EDFFile.h"
#include "sleeplink/parser/Models.h"
#include <string>
#include <memory>
#include <vector>
#include <chrono>
#include <cstdint>

namespace sleeplink::parser {

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

    // Breath analysis and calculated metrics (OSCAR-style)
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

    static void calculateRespiratoryMetrics(
        const std::vector<double>& flow_data,
        const std::vector<double>& pressure_data,
        double sample_rate,
        int minute_idx,
        BreathingSummary& summary
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

} // namespace sleeplink::parser
