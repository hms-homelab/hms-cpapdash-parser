#include "cpapdash/parser/EDFParser.h"
#include <iostream>
#include <cmath>
#include <algorithm>
#include <numeric>

namespace cpapdash::parser {

bool EDFParser::parseBRPFile(EDFFile& edf, ParsedSession& session) {
    // Get EDF header timestamp
    auto file_start = edf.getStartTime();

    // Set timestamps from EDF header only if not already set (from filename)
    if (!session.session_start.has_value()) {
        session.session_start = file_start;
    }

    // Track this BRP's end time (EDF start + data duration).
    // For multi-BRP sessions, the last BRP's end is the session end.
    auto brp_end = file_start + std::chrono::seconds(
        static_cast<int>(edf.actual_records * edf.record_duration));
    session.session_end = brp_end;
    session.duration_seconds = static_cast<int>(
        std::chrono::duration_cast<std::chrono::seconds>(
            brp_end - session.session_start.value()).count());
    session.data_records = edf.actual_records;
    session.file_complete = edf.complete;
    session.extra_records = edf.extra_records;
    session.growing = edf.growing;

    // Find Flow and Pressure signals
    int flow_idx = edf.findSignal("Flow");
    int press_idx = edf.findSignal("Press");

    if (flow_idx < 0) {
        std::cerr << "Parser: Flow signal not found in BRP" << std::endl;
        return false;
    }

    std::vector<double> flow_data, press_data;
    edf.readSignal(flow_idx, flow_data);
    if (press_idx >= 0) {
        edf.readSignal(press_idx, press_data);
    }

    if (flow_data.empty()) {
        std::cerr << "Parser: No flow data read from BRP" << std::endl;
        return false;
    }

    // ResMed stores flow in L/sec - convert to L/min (multiply by 60)
    // Reference: OSCAR resmed_loader.cpp line 3121
    for (double& val : flow_data) {
        val *= 60.0;
    }

    // Compute per-minute breathing summaries
    // Flow is 25 Hz -> 1500 samples per minute
    double sample_rate = edf.signals[flow_idx].samples_per_record / edf.record_duration;
    int samples_per_minute = static_cast<int>(sample_rate * 60);
    int n_minutes = static_cast<int>(flow_data.size()) / samples_per_minute;

    bool have_pressure = !press_data.empty();

    for (int min = 0; min < n_minutes; ++min) {
        int start = min * samples_per_minute;
        int end   = start + samples_per_minute;

        auto flow_begin = flow_data.begin() + start;
        auto flow_end   = flow_data.begin() + end;

        BreathingSummary summary(session.session_start.value() + std::chrono::minutes(min));

        // Basic flow statistics
        double sum = std::accumulate(flow_begin, flow_end, 0.0);
        summary.avg_flow_rate = sum / samples_per_minute;
        summary.max_flow_rate = *std::max_element(flow_begin, flow_end);
        summary.min_flow_rate = *std::min_element(flow_begin, flow_end);

        // Basic pressure statistics
        if (have_pressure && end <= static_cast<int>(press_data.size())) {
            auto p_begin = press_data.begin() + start;
            auto p_end   = press_data.begin() + end;
            double psum = std::accumulate(p_begin, p_end, 0.0);
            summary.avg_pressure = psum / samples_per_minute;
            summary.max_pressure = *std::max_element(p_begin, p_end);
            summary.min_pressure = *std::min_element(p_begin, p_end);
        }

        // Calculate OSCAR-style respiratory metrics (RR, TV, MV, Ti/Te, I:E, FL, percentiles)
        calculateRespiratoryMetrics(flow_data, press_data, sample_rate, min, summary);

        session.breathing_summary.push_back(summary);
    }

    // Detect flow-based session boundaries (actual mask on/off times)
    std::optional<std::chrono::system_clock::time_point> actual_start, actual_end;
    bool session_active = false;
    detectFlowBasedSessionBoundaries(flow_data, sample_rate, file_start,
                                     actual_start, actual_end, session_active);

    return true;
}

void EDFParser::detectFlowBasedSessionBoundaries(
    const std::vector<double>& flow_data,
    double sample_rate,
    std::chrono::system_clock::time_point file_start,
    std::optional<std::chrono::system_clock::time_point>& actual_start,
    std::optional<std::chrono::system_clock::time_point>& actual_end,
    bool& session_active
) {
    if (flow_data.empty()) return;

    const double FLOW_THRESHOLD = 0.1;  // L/min - below this is "no flow"
    const int ZERO_FLOW_DURATION = 5 * 60; // 5 minutes of zero flow = session ended
    const int samples_for_end = static_cast<int>(ZERO_FLOW_DURATION * sample_rate);

    // Find first non-zero flow (actual session start)
    for (size_t i = 0; i < flow_data.size(); ++i) {
        if (std::abs(flow_data[i]) > FLOW_THRESHOLD) {
            int seconds_offset = static_cast<int>(i / sample_rate);
            actual_start = file_start + std::chrono::seconds(seconds_offset);
            break;
        }
    }

    // Find last sustained non-zero flow (actual session end)
    int last_flow_idx = -1;
    for (int i = flow_data.size() - 1; i >= 0; --i) {
        if (std::abs(flow_data[i]) > FLOW_THRESHOLD) {
            last_flow_idx = i;
            break;
        }
    }

    if (last_flow_idx < 0) {
        // No flow detected at all
        session_active = false;
        return;
    }

    // Check if there's sustained zero flow after last_flow_idx
    int zero_count = flow_data.size() - last_flow_idx - 1;
    if (zero_count >= samples_for_end) {
        int seconds_offset = static_cast<int>(last_flow_idx / sample_rate);
        actual_end = file_start + std::chrono::seconds(seconds_offset);
        session_active = false;
    } else {
        session_active = true;
    }
}

// ============================================================================
//  Respiratory Metrics Calculation (OSCAR-style)
// ============================================================================

double EDFParser::calculatePercentile(
    const std::vector<double>& data,
    double percentile
) {
    if (data.empty()) return 0.0;

    // Filter out invalid values (-1, NaN, Inf)
    std::vector<double> valid_data;
    valid_data.reserve(data.size());

    for (double val : data) {
        if (!std::isnan(val) && !std::isinf(val) && val != -1.0) {
            valid_data.push_back(val);
        }
    }

    if (valid_data.empty()) return 0.0;

    std::vector<double> sorted = valid_data;
    std::sort(sorted.begin(), sorted.end());

    double idx = (percentile / 100.0) * (sorted.size() - 1);
    int lower = static_cast<int>(std::floor(idx));
    int upper = static_cast<int>(std::ceil(idx));

    if (lower == upper) {
        return sorted[lower];
    }

    // Linear interpolation
    double weight = idx - lower;
    return sorted[lower] * (1 - weight) + sorted[upper] * weight;
}

std::vector<EDFParser::BreathCycle> EDFParser::detectBreaths(
    const std::vector<double>& flow_data,
    double sample_rate
) {
    std::vector<BreathCycle> breaths;

    if (flow_data.empty()) return breaths;

    const double FLOW_THRESHOLD = 0.05;  // L/min - noise threshold
    const int MIN_BREATH_SAMPLES = static_cast<int>(sample_rate * 1.0);  // 1 second minimum
    const int MAX_BREATH_SAMPLES = static_cast<int>(sample_rate * 10.0); // 10 seconds maximum
    const double MAX_VALID_FLOW = 200.0;  // L/min - sanity check
    const double MIN_VALID_FLOW = -200.0;

    // Validate and filter flow data first
    std::vector<double> clean_flow;
    clean_flow.reserve(flow_data.size());

    for (double val : flow_data) {
        if (std::isnan(val) || std::isinf(val) ||
            val == -1.0 || val < MIN_VALID_FLOW || val > MAX_VALID_FLOW) {
            clean_flow.push_back(0.0);
        } else {
            clean_flow.push_back(val);
        }
    }

    // Detect zero-crossings (breath boundaries)
    // Positive flow = inspiration, negative flow = expiration
    std::vector<int> zero_crossings;
    zero_crossings.push_back(0);  // Start of data

    bool was_positive = (clean_flow[0] > 0);

    for (size_t i = 1; i < clean_flow.size(); ++i) {
        bool is_positive = (clean_flow[i] > FLOW_THRESHOLD);
        bool is_negative = (clean_flow[i] < -FLOW_THRESHOLD);

        // Detect crossing from negative to positive (start of inspiration)
        if (!was_positive && is_positive) {
            zero_crossings.push_back(i);
            was_positive = true;
        }
        // Detect crossing from positive to negative (start of expiration)
        else if (was_positive && is_negative) {
            zero_crossings.push_back(i);
            was_positive = false;
        }
    }

    zero_crossings.push_back(clean_flow.size() - 1);  // End of data

    // Process each breath cycle (inspiration + expiration)
    for (size_t i = 0; i < zero_crossings.size() - 2; i += 2) {
        int start = zero_crossings[i];
        int mid = zero_crossings[i + 1];  // Inspiration -> Expiration transition
        int end = zero_crossings[i + 2];

        int breath_duration = end - start;

        // Filter out breaths that are too short or too long
        if (breath_duration < MIN_BREATH_SAMPLES || breath_duration > MAX_BREATH_SAMPLES) {
            continue;
        }

        BreathCycle breath;
        breath.start_idx = start;
        breath.end_idx = end;

        // Calculate inspiratory time (start -> mid)
        breath.inspiratory_time = static_cast<double>(mid - start) / sample_rate;

        // Calculate expiratory time (mid -> end)
        breath.expiratory_time = static_cast<double>(end - mid) / sample_rate;

        // Calculate tidal volume (integrate flow over time)
        double inspiratory_volume = 0.0;
        for (int j = start; j < mid; ++j) {
            if (clean_flow[j] > 0) {
                inspiratory_volume += clean_flow[j];
            }
        }
        // Convert: (L/min) * (samples) / (samples/sec) / (sec/min) = L -> mL
        breath.tidal_volume = (inspiratory_volume / sample_rate / 60.0) * 1000.0;

        // Validate tidal volume (normal: 300-800 mL, extreme: 100-2000 mL)
        if (breath.tidal_volume < 50.0 || breath.tidal_volume > 3000.0) {
            continue;  // Skip this invalid breath
        }

        // Calculate flow limitation score (0-1)
        double insp_max = 0.0;
        double insp_sum = 0.0;
        int insp_count = 0;

        for (int j = start; j < mid; ++j) {
            if (clean_flow[j] > 0) {
                insp_max = std::max(insp_max, clean_flow[j]);
                insp_sum += clean_flow[j];
                insp_count++;
            }
        }

        if (insp_count > 0 && insp_max > 0) {
            double insp_mean = insp_sum / insp_count;
            double peak_to_mean_ratio = insp_max / insp_mean;
            breath.flow_limitation = std::max(0.0, std::min(1.0, 1.0 - (peak_to_mean_ratio - 1.0) / 2.0));
        } else {
            breath.flow_limitation = 0.0;
        }

        breaths.push_back(breath);
    }

    return breaths;
}

void EDFParser::calculateRespiratoryMetrics(
    const std::vector<double>& flow_data,
    const std::vector<double>& pressure_data,
    double sample_rate,
    int minute_idx,
    BreathingSummary& summary
) {
    int samples_per_minute = static_cast<int>(sample_rate * 60);
    int start = minute_idx * samples_per_minute;
    int end = start + samples_per_minute;

    if (end > static_cast<int>(flow_data.size())) {
        end = flow_data.size();
    }

    // Extract this minute's flow data
    std::vector<double> minute_flow(flow_data.begin() + start, flow_data.begin() + end);

    // Detect breaths in this minute
    std::vector<BreathCycle> breaths = detectBreaths(minute_flow, sample_rate);

    if (breaths.empty()) {
        return;
    }

    // Calculate respiratory rate (breaths per minute)
    summary.respiratory_rate = static_cast<double>(breaths.size());

    // Calculate average tidal volume
    double total_tv = 0.0;
    for (const auto& breath : breaths) {
        total_tv += breath.tidal_volume;
    }
    summary.tidal_volume = total_tv / breaths.size();

    // Calculate minute ventilation (L/min)
    if (summary.respiratory_rate.has_value() && summary.tidal_volume.has_value()) {
        summary.minute_ventilation = (summary.respiratory_rate.value() *
                                     summary.tidal_volume.value()) / 1000.0;
    }

    // Calculate average inspiratory and expiratory times
    double total_ti = 0.0;
    double total_te = 0.0;
    for (const auto& breath : breaths) {
        total_ti += breath.inspiratory_time;
        total_te += breath.expiratory_time;
    }
    summary.inspiratory_time = total_ti / breaths.size();
    summary.expiratory_time = total_te / breaths.size();

    // Calculate I:E ratio
    if (summary.inspiratory_time.has_value() &&
        summary.expiratory_time.has_value() &&
        summary.expiratory_time.value() > 0) {
        summary.ie_ratio = summary.inspiratory_time.value() /
                          summary.expiratory_time.value();
    }

    // Calculate average flow limitation score
    double total_fl = 0.0;
    for (const auto& breath : breaths) {
        total_fl += breath.flow_limitation;
    }
    summary.flow_limitation = total_fl / breaths.size();

    // Calculate percentile statistics for flow
    summary.flow_p95 = calculatePercentile(minute_flow, 95.0);

    // Calculate percentile statistics for pressure
    if (!pressure_data.empty() && end <= static_cast<int>(pressure_data.size())) {
        std::vector<double> minute_pressure(pressure_data.begin() + start,
                                           pressure_data.begin() + end);
        summary.pressure_p95 = calculatePercentile(minute_pressure, 95.0);
    }

    // Calculate leak rate
    if (!breaths.empty()) {
        double total_leak = 0.0;
        int valid_breaths = 0;

        for (const auto& breath : breaths) {
            double expiratory_volume = 0.0;
            int mid_idx = breath.start_idx + (breath.end_idx - breath.start_idx) / 2;

            for (int j = mid_idx; j < breath.end_idx; ++j) {
                if (j >= 0 && j < static_cast<int>(minute_flow.size())) {
                    if (minute_flow[j] < 0) {
                        expiratory_volume += std::abs(minute_flow[j]);
                    }
                }
            }
            expiratory_volume = (expiratory_volume / sample_rate / 60.0) * 1000.0;

            double breath_leak = breath.tidal_volume - expiratory_volume;

            if (breath_leak >= 0 && breath_leak < 100000.0) {
                total_leak += breath_leak;
                valid_breaths++;
            }
        }

        if (valid_breaths > 0 && summary.respiratory_rate.has_value()) {
            double avg_leak_ml_per_breath = total_leak / valid_breaths;
            summary.leak_rate = (avg_leak_ml_per_breath * summary.respiratory_rate.value()) / 1000.0;

            if (summary.leak_rate.value() < 0 || summary.leak_rate.value() > 100.0) {
                summary.leak_rate = std::nullopt;
            }
        } else {
            summary.leak_rate = std::nullopt;
        }
    }
}

} // namespace cpapdash::parser
