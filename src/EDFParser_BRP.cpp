#include "cpapdash/parser/EDFParser.h"
#include <iostream>
#include <cmath>
#include <algorithm>
#include <numeric>
#include <optional>
#include <string>
#include <ctime>

namespace cpapdash::parser {

// Start time carried by a "YYYYMMDD_HHMMSS_TYPE.edf" name, if it has one.
//
// The name beats the header. An AirSense 10 was observed writing header date
// 29.06.26 into 20260706_195339_BRP.edf, a week behind, and anchoring to that
// would drop a night's data a week away from the session it belongs to.
static std::optional<std::chrono::system_clock::time_point>
startFromFilename(const std::string& path) {
    if (path.empty()) return std::nullopt;
    auto slash = path.find_last_of("/\\");
    std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);
    if (name.size() < 15 || name[8] != '_') return std::nullopt;

    auto num = [&](std::size_t off, std::size_t len, int lo, int hi) -> int {
        int v = 0;
        for (std::size_t i = 0; i < len; ++i) {
            char c = name[off + i];
            if (c < '0' || c > '9') return -1;
            v = v * 10 + (c - '0');
        }
        return (v >= lo && v <= hi) ? v : -1;
    };
    const int Y = num(0, 4, 1970, 2999), Mo = num(4, 2, 1, 12), D = num(6, 2, 1, 31);
    const int h = num(9, 2, 0, 23), mi = num(11, 2, 0, 59), s = num(13, 2, 0, 60);
    if (Y < 0 || Mo < 0 || D < 0 || h < 0 || mi < 0 || s < 0) return std::nullopt;

    // mktime, matching EDFFile::getStartTime() exactly. A card writes wall-clock
    // local time in both the name and the header, and the two must land on the
    // same instant: interpreting the name as UTC while the header stays local
    // would shift every path-opened file by the machine's UTC offset — invisible
    // in a UTC test environment and hours wrong everywhere else.
    std::tm t{};
    t.tm_year = Y - 1900; t.tm_mon = Mo - 1; t.tm_mday = D;
    t.tm_hour = h; t.tm_min = mi; t.tm_sec = s;
    t.tm_isdst = -1;
    const std::time_t e = std::mktime(&t);
    if (e == static_cast<std::time_t>(-1)) return std::nullopt;
    return std::chrono::system_clock::from_time_t(e);
}

bool EDFParser::parseBRPFile(EDFFile& edf, ParsedSession& session) {
    // Where THIS checkpoint sits on the clock. Every sample it holds is placed
    // relative to this, never to the session's start.
    //
    // A night is not one file. ResMed writes a checkpoint every few minutes and
    // opens a fresh one after any mask-off break, so a session routinely arrives
    // as several BRPs separated by real gaps. Anchoring each of them to
    // session_start stacked them all from the same instant: the break was erased
    // because every segment restarted at the beginning, and the series ended
    // early by the total of the gaps it had swallowed. That is issue #21 — a
    // 2:20am break that never appeared and a flow chart that stopped at 4am
    // while the summary metrics, which come from STR, covered the whole night.
    const auto header_start = edf.getStartTime();
    const auto file_start = startFromFilename(edf.filepath()).value_or(header_start);

    // Session start still comes from the first file, and only if nobody set it
    // from the folder name already.
    if (!session.session_start.has_value()) {
        session.session_start = file_start;
    }

    const auto file_seconds =
        static_cast<int>(edf.actual_records * edf.record_duration);

    // MAX, not last-one-wins. Checkpoints are parsed in name order, but a merge
    // that re-parsed an earlier one would otherwise pull the session end
    // backwards over a later checkpoint that had already extended it.
    const auto brp_end = file_start + std::chrono::seconds(file_seconds);
    if (!session.session_end.has_value() || brp_end > session.session_end.value())
        session.session_end = brp_end;

    // Duration is the SUM of what the checkpoints hold, not the distance between
    // the first and the last. The gap between two checkpoints is mask-off time --
    // the very break this function's header comment describes ResMed opening a
    // fresh file after -- so measuring the envelope reports it as therapy. That
    // is support ticket 87: 1 + 114 + 107 minutes of real data spread across a
    // 7h evening break came back as 11h22m instead of 3h42m, and AHI fell from
    // 0.54 to 0.18 with it. OSCAR, which sums the sessions, agrees with 3h42m.
    //
    // Assign by file_start rather than adding: this same checkpoint is re-parsed
    // on a merge and grows during a live session, and both would otherwise
    // inflate a running total.
    session.brp_spans[file_start] = file_seconds;
    int therapy_seconds = 0;
    for (const auto& [start, secs] : session.brp_spans) {
        (void)start;
        therapy_seconds += secs;
    }
    session.duration_seconds = therapy_seconds;
    session.data_records = edf.actual_records;
    session.file_complete = edf.complete;
    session.extra_records = edf.extra_records;
    session.growing = edf.growing;

    // Find Flow and Pressure signals.
    //
    // Pressure is matched on the label PREFIX first: findSignal() is a substring
    // match, so on any file that carries both a mask and a therapy channel it
    // returns whichever comes first, and "MaskPress..." contains "Press". A
    // ResMed BRP only holds Press.40ms today so the two agree, but the fallback
    // keeps a device that labels its BRP pressure differently working exactly as
    // before rather than losing the channel.
    int flow_idx = edf.findSignal("Flow");
    int press_idx = edf.findSignalPrefix("Press");
    if (press_idx < 0) press_idx = edf.findSignal("Press");

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
    for (double& val : flow_data) {
        val *= 60.0;
    }

    // Compute per-minute breathing summaries
    // Flow is 25 Hz -> 1500 samples per minute
    double sample_rate = edf.signals[flow_idx].samples_per_record / edf.record_duration;
    int samples_per_minute = static_cast<int>(sample_rate * 60);
    int n_minutes = static_cast<int>(flow_data.size()) / samples_per_minute;

    bool have_pressure = !press_data.empty();

    // Detect breaths ONCE over the full flow series, then bucket them by the
    // minute their onset falls in. This feeds both the per-minute summaries
    // below and session.breaths further down, so the two can no longer disagree
    // about a breath that straddles a minute boundary, and an eight-hour night
    // is scanned once instead of once plus once per minute. (SDD-003 D2)
    const auto all_breaths = detectBreaths(flow_data, sample_rate);

    std::vector<std::vector<BreathCycle>> breaths_by_minute(n_minutes);
    for (const auto& b : all_breaths) {
        const int min_idx = b.start_idx / samples_per_minute;
        if (min_idx >= 0 && min_idx < n_minutes) {
            breaths_by_minute[min_idx].push_back(b);
        }
    }

    for (int min = 0; min < n_minutes; ++min) {
        int start = min * samples_per_minute;
        int end   = start + samples_per_minute;

        auto flow_begin = flow_data.begin() + start;
        auto flow_end   = flow_data.begin() + end;

        BreathingSummary summary(file_start + std::chrono::minutes(min));

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

        // Calculate calculated respiratory metrics (RR, TV, MV, Ti/Te, I:E, FL, percentiles)
        calculateRespiratoryMetrics(flow_data, press_data, breaths_by_minute[min],
                                    sample_rate, min, summary);

        session.breathing_summary.push_back(summary);
    }

    // Persist breath-by-breath detail from the same single detection pass the
    // per-minute summaries were built from, mapping sample indices to absolute
    // time. Base on THIS file's start, the same anchor the per-minute summaries
    // use, so breaths stay lined up with them across a multi-checkpoint night.
    {
        auto breath_base = file_start;
        session.breaths.reserve(all_breaths.size());
        for (const auto& b : all_breaths) {
            Breath out;
            out.onset = breath_base + std::chrono::microseconds(
                static_cast<long long>(b.start_idx / sample_rate * 1e6));
            out.tidal_volume = b.tidal_volume;
            out.inspiratory_time = b.inspiratory_time;
            out.expiratory_time = b.expiratory_time;
            out.flow_limitation = b.flow_limitation;
            session.breaths.push_back(out);
        }
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
//  Respiratory Metrics Calculation (calculated)
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
    //
    // The list holds ONLY real crossings, so it strictly alternates. It used to
    // be seeded with index 0 and walked in triples from there, which is correct
    // only when the file opens in inspiration. A file opening mid-expiration got
    // every triple shifted by half a breath: the segment read as inspiration was
    // the tail of an expiration, Ti and Te came out swapped, and the tidal-volume
    // sanity filter below then discarded most of the mispaired cycles, so the
    // whole file quietly yielded almost no breaths instead of visibly wrong ones.
    // A sine starting exactly at 0.0 yielded none at all. (SDD-003 D1)

    // Opening phase comes from the first sample that clears the noise threshold.
    // clean_flow[0] is 0.0 for any file that starts at rest, and 0.0 > 0 is
    // false, which is what silently declared those files to be in expiration.
    size_t first_signal = 0;
    while (first_signal < clean_flow.size() &&
           std::abs(clean_flow[first_signal]) <= FLOW_THRESHOLD) {
        ++first_signal;
    }
    if (first_signal == clean_flow.size()) return breaths;  // flat line

    const bool opened_in_inspiration = clean_flow[first_signal] > 0;

    std::vector<int> zero_crossings;
    bool was_positive = opened_in_inspiration;

    for (size_t i = first_signal + 1; i < clean_flow.size(); ++i) {
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

    // Start the walk on the first crossing INTO inspiration, so every triple is
    // (inspiration start, expiration start, next inspiration start). If the file
    // opened in inspiration, crossing 0 is into expiration and we skip it. The
    // partial breath at each end of the file is dropped rather than guessed:
    // under 0.1% of an eight-hour night at roughly one breath per four seconds.
    const size_t first_triple = opened_in_inspiration ? 1 : 0;

    // Process each breath cycle (inspiration + expiration)
    for (size_t i = first_triple; i + 2 < zero_crossings.size(); i += 2) {
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

// A shape-based flattening index was written here and then REMOVED, because it
// did not survive validation. Measured against ResMed's own FlowLim channel over
// 175 sessions and 49,256 paired minutes of real card data: Spearman -0.003, and
// the machine's highest flow-limitation minutes scored no higher than its
// lowest. ResMed computes flow limitation by blending a flatness index with a
// breath shape index, ventilation change and duty cycle, so a flatness scalar on
// its own measures almost none of it. Do not re-add one without an oracle.
// See sdd/003-the-breath-detector.md.

void EDFParser::calculateRespiratoryMetrics(
    const std::vector<double>& flow_data,
    const std::vector<double>& pressure_data,
    const std::vector<BreathCycle>& breaths,
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

    // Extract this minute's flow data (still needed for the percentiles below,
    // which are sample statistics rather than breath statistics)
    std::vector<double> minute_flow(flow_data.begin() + start, flow_data.begin() + end);

    // Breaths arrive already detected over the whole file and bucketed by the
    // minute their onset falls in. Re-detecting them from a one-minute slice, as
    // this used to do, meant a second full pass over the night AND truncating
    // every breath that straddled a boundary, which the tidal-volume filter then
    // dropped from both minutes. All breath indices below are file-relative.
    // (SDD-003 D2)
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

            // This is the only place breath indices escape the detector. They
            // are file-relative now that detection runs once over the whole
            // file, so index flow_data, not the minute slice (SDD-003 D2).
            for (int j = mid_idx; j < breath.end_idx; ++j) {
                if (j >= 0 && j < static_cast<int>(flow_data.size())) {
                    if (flow_data[j] < 0) {
                        expiratory_volume += std::abs(flow_data[j]);
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
