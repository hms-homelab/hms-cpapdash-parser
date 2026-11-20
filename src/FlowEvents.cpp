#include "cpapdash/parser/FlowEvents.h"

#include <algorithm>
#include <cmath>

namespace cpapdash::parser {

namespace {

double medianOf(std::vector<double> v) {
    if (v.empty()) return 0;
    const size_t mid = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + static_cast<long>(mid), v.end());
    return v[mid];
}

// Mean leak over a span of the recording, or 0 when no leak signal was supplied.
double leakOver(const std::vector<double>& leak, double leak_rate,
                double from_s, double to_s) {
    if (leak.empty() || leak_rate <= 0 || to_s <= from_s) return 0;

    const size_t a = static_cast<size_t>(std::max(0.0, from_s * leak_rate));
    const size_t b = std::min(leak.size(), static_cast<size_t>(to_s * leak_rate));
    if (a >= b) return 0;

    double sum = 0;
    for (size_t i = a; i < b; ++i) sum += leak[i];
    return sum / static_cast<double>(b - a);
}

struct Run {
    double from_s, to_s;
};

// Runs of flow whose amplitude stays below `fraction` of the trailing baseline
// for at least params.min_duration_s, with the mask-off and large-leak runs
// taken out. The one search both the apnea (fraction 0.10) and the hypopnea
// candidate (0.70) use.
std::vector<Run> suppressedRuns(const std::vector<EDFParser::BreathCycle>& breaths,
                                const std::vector<double>& flow, double sample_rate,
                                const std::vector<double>& leak, double leak_rate,
                                const FlowEventParams& params, double fraction,
                                FlowEventStats& st) {
    std::vector<Run> runs;

    // Amplitude per breath, and the second each breath begins at.
    std::vector<double> amp(breaths.size());
    std::vector<double> at_s(breaths.size());
    for (size_t i = 0; i < breaths.size(); ++i) {
        amp[i] = breathAmplitude(flow, breaths[i]);
        at_s[i] = breaths[i].start_idx / sample_rate;
    }

    // The recording as a series of intervals: every breath, and every gap
    // between two of them.
    //
    // BOTH matter, and the first cut of this got zero recall by only looking at
    // the gaps. During a real apnea the flow does not go flat -- cardiac
    // oscillation and noise keep crossing zero, so the breath detector keeps
    // emitting breaths, just tiny ones. There is no gap to find. The apnea is a
    // RUN OF SUPPRESSED BREATHS, and the gaps matter only for the rarer case
    // where the signal really does stop.
    struct Interval {
        double from_s, to_s, amp;
        size_t breath_before;  // breaths before this interval, for the baseline
    };

    std::vector<Interval> intervals;
    intervals.reserve(breaths.size() * 2);

    auto peakToPeak = [&](double from_s, double to_s) {
        const size_t a = static_cast<size_t>(std::max(0.0, from_s * sample_rate));
        const size_t b = std::min(flow.size(), static_cast<size_t>(to_s * sample_rate));
        if (a >= b) return 0.0;
        double lo = flow[a], hi = flow[a];
        for (size_t k = a; k < b; ++k) { lo = std::min(lo, flow[k]); hi = std::max(hi, flow[k]); }
        return hi - lo;
    };

    for (size_t i = 0; i < breaths.size(); ++i) {
        intervals.push_back({breaths[i].start_idx / sample_rate,
                             breaths[i].end_idx / sample_rate, amp[i], i});

        if (i + 1 < breaths.size()) {
            const double g0 = breaths[i].end_idx / sample_rate;
            const double g1 = breaths[i + 1].start_idx / sample_rate;
            if (g1 > g0) intervals.push_back({g0, g1, peakToPeak(g0, g1), i + 1});
        }
    }

    // Baseline for an interval: the median amplitude of the breaths in the
    // trailing window that ENDED BEFORE it, so nothing from inside a suppression
    // can lower the bar it is judged against.
    auto baselineAt = [&](const Interval& iv) -> double {
        std::vector<double> window;
        for (size_t j = iv.breath_before; j-- > 0;) {
            if (at_s[j] < iv.from_s - params.baseline_window_s) break;
            window.push_back(amp[j]);
        }
        if (window.size() < params.min_baseline_breaths) return -1;
        return medianOf(window);
    };

    std::vector<bool> suppressed(intervals.size(), false);
    for (size_t i = 0; i < intervals.size(); ++i) {
        const double base = baselineAt(intervals[i]);
        if (base < 0) { ++st.rejected_no_baseline; continue; }
        if (base <= 0) continue;
        suppressed[i] = intervals[i].amp < fraction * base;
    }

    // Runs of suppressed intervals, long enough to be an event.
    for (size_t i = 0; i < intervals.size();) {
        if (!suppressed[i]) { ++i; continue; }

        size_t j = i;
        while (j < intervals.size() && suppressed[j]) ++j;

        const double from_s = intervals[i].from_s;
        const double to_s = intervals[j - 1].to_s;
        const double duration = to_s - from_s;
        i = j;

        if (duration < params.min_duration_s) continue;

        // Longer than anyone holds their breath: the mask came off, or the
        // recording has a hole in it. Not an event, and not therapy time.
        if (duration >= params.mask_off_s) { ++st.rejected_mask_off; continue; }
        ++st.candidates;

        // Above the large-leak line the flow stopped describing the patient, so
        // an apparent collapse says nothing about their airway.
        if (!leak.empty() &&
            leakOver(leak, leak_rate, from_s, to_s) > params.large_leak_lpm) {
            ++st.rejected_leak;
            continue;
        }
        runs.push_back({from_s, to_s});
    }
    return runs;
}

std::chrono::system_clock::time_point at(std::chrono::system_clock::time_point start,
                                         double seconds) {
    return start + std::chrono::milliseconds(static_cast<long long>(std::llround(seconds * 1000.0)));
}

} // anonymous namespace

double breathAmplitude(const std::vector<double>& flow,
                       const EDFParser::BreathCycle& breath) {
    const size_t a = static_cast<size_t>(std::max(0, breath.start_idx));
    const size_t b = std::min(flow.size(), static_cast<size_t>(std::max(0, breath.end_idx)));
    if (a >= b) return 0;

    double lo = flow[a], hi = flow[a];
    for (size_t i = a; i < b; ++i) {
        lo = std::min(lo, flow[i]);
        hi = std::max(hi, flow[i]);
    }
    return hi - lo;
}

std::vector<SleepEvent> detectFlowApneas(
    const std::vector<EDFParser::BreathCycle>& breaths,
    const std::vector<double>& flow,
    double sample_rate,
    std::chrono::system_clock::time_point start,
    const std::vector<double>& leak,
    double leak_rate,
    const FlowEventParams& params,
    FlowEventStats* stats)
{
    std::vector<SleepEvent> events;
    FlowEventStats st;
    st.breaths = breaths.size();

    if (breaths.empty() || flow.empty() || sample_rate <= 0) {
        if (stats) *stats = st;
        return events;
    }

    st.analysed_seconds = static_cast<double>(flow.size()) / sample_rate;

    for (const auto& r : suppressedRuns(breaths, flow, sample_rate, leak, leak_rate, params,
                                        params.drop_fraction, st)) {
        SleepEvent ev;
        // Unclassified. Airflow stopped -- that we can measure. Obstructive
        // versus central needs effort belts, and is not guessed at.
        ev.event_type = EventType::APNEA;
        ev.timestamp = at(start, r.from_s);
        ev.duration_seconds = r.to_s - r.from_s;
        ev.details = "Flow-derived apnea";
        ev.source = EventSource::Computed;  // SDD-014
        events.push_back(std::move(ev));
    }

    if (stats) *stats = st;
    return events;
}

std::vector<SleepEvent> detectFlowHypopneas(
    const std::vector<EDFParser::BreathCycle>& breaths,
    const std::vector<double>& flow,
    double sample_rate,
    std::chrono::system_clock::time_point start,
    const std::vector<DesatEvent>& desaturations,
    const std::vector<double>& leak,
    double leak_rate,
    const FlowEventParams& params)
{
    std::vector<SleepEvent> events;
    if (breaths.empty() || flow.empty() || sample_rate <= 0 || desaturations.empty())
        return events;

    FlowEventStats ignored;
    const auto apneas = suppressedRuns(breaths, flow, sample_rate, leak, leak_rate, params,
                                       params.drop_fraction, ignored);
    const auto drops = suppressedRuns(breaths, flow, sample_rate, leak, leak_rate, params,
                                      params.hypopnea_fraction, ignored);

    for (const auto& d : drops) {
        // A drop that holds an apnea is that apnea, scored once.
        const bool is_apnea = std::any_of(apneas.begin(), apneas.end(), [&](const Run& a) {
            return a.from_s < d.to_s && d.from_s < a.to_s;
        });
        if (is_apnea) continue;

        // The desaturation that goes with it begins during the drop or within
        // the window after its end.
        const auto from = at(start, d.from_s);
        const auto until = at(start, d.to_s + params.desat_window_s);
        const bool desat = std::any_of(
            desaturations.begin(), desaturations.end(), [&](const DesatEvent& e) {
                return e.depth >= params.desat_min_depth && e.onset >= from && e.onset <= until;
            });
        if (!desat) continue;

        SleepEvent ev;
        ev.event_type = EventType::HYPOPNEA;
        ev.timestamp = from;
        ev.duration_seconds = d.to_s - d.from_s;
        ev.details = "Flow-derived hypopnea, with a desaturation";
        ev.source = EventSource::Computed;
        events.push_back(std::move(ev));
    }
    return events;
}

} // namespace cpapdash::parser
