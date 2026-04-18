#pragma once

#include <string>
#include <vector>
#include <chrono>
#include <cstdint>
#include <optional>

namespace cpapdash::parser {

struct OximetrySample {
    std::chrono::system_clock::time_point timestamp;
    uint8_t spo2;          // 0-100, 0xFF = off-wrist
    uint8_t heart_rate;    // bpm, 0xFF = invalid
    uint8_t invalid_flag;
    uint8_t motion;
    uint8_t vibration;
    bool valid() const { return spo2 != 0xFF && heart_rate != 0xFF && invalid_flag == 0; }
};

struct OximetryMetrics {
    double avg_spo2 = 0;
    double min_spo2 = 100;
    double max_spo2 = 0;
    double spo2_baseline = 0;     // 95th percentile of valid readings
    double time_below_90_pct = 0;
    double time_below_88_pct = 0;
    double odi_3pct = 0;          // 3% desaturation events per hour
    int    desat_count_3pct = 0;

    double avg_hr = 0;
    int    min_hr = 300;
    int    max_hr = 0;

    int    valid_samples = 0;
    int    total_samples = 0;
    double recording_hours = 0;
};

struct OximetrySession {
    std::string filename;
    std::chrono::system_clock::time_point start_time;
    std::chrono::system_clock::time_point end_time;
    int duration_seconds = 0;
    double sample_interval = 0;

    std::vector<OximetrySample> samples;
    OximetryMetrics metrics;

    std::string date_str() const;  // "YYYYMMDD"
};

class VLDParser {
public:
    static std::optional<OximetrySession> parse(
        const uint8_t* data, size_t len,
        const std::string& filename = "");

    static std::optional<OximetrySession> parseFile(const std::string& filepath);

    static OximetryMetrics calculateMetrics(
        const std::vector<OximetrySample>& samples,
        double sample_interval);

private:
    static constexpr size_t HEADER_SIZE = 40;
    static constexpr size_t RECORD_SIZE = 5;
    static constexpr uint8_t INVALID = 0xFF;
};

} // namespace cpapdash::parser
