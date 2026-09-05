#pragma once

#include "cpapdash/parser/Models.h"
#include "cpapdash/parser/EDFParser.h"
#include <chrono>
#include <vector>

// Apneas detected from the flow waveform -- see sdd/006-flow-derived-events.md.
//
// For machines that do not report their own events, or whose event encoding we
// cannot read. A Sefam S.Box writes its detections as a bitfield whose bits mean
// nothing to us yet (SDD-005 section 7a), so its users get no AHI at all; this
// is how they get one.
//
// Nothing here is device-specific. It takes a breath series and a flow series
// and returns events, so any machine whose flow we can read can use it.
//
// The thresholds are the AASM scoring manual's, which is a published clinical
// standard: an apnea is a drop of at least 90% in peak airflow excursion from
// the pre-event baseline, lasting at least ten seconds.

namespace cpapdash::parser {

/** Knobs, all of them, so nothing is buried as a literal in the algorithm. */
struct FlowEventParams {
    // The AASM apnea criteria.
    double drop_fraction = 0.10;      // amplitude below 10% of baseline is a >=90% drop
    double min_duration_s = 10.0;

    // Trailing window the baseline is taken over, with the candidate event
    // excluded so an apnea cannot lower the bar it has to clear.
    double baseline_window_s = 120.0;

    // A baseline needs breaths behind it. Below this many, no call is made --
    // which is what excludes the opening of a recording.
    size_t min_baseline_breaths = 8;

    // Above this leak the flow no longer describes the patient's breathing.
    // ResMed's own large-leak line. Ignored when no leak signal is supplied.
    double large_leak_lpm = 24.0;

    // A stretch of flow this long with no breath in it is the mask being off,
    // not an apnea of remarkable duration.
    double mask_off_s = 120.0;
};

/** What a detection pass found, alongside the events themselves. */
struct FlowEventStats {
    size_t breaths = 0;
    size_t candidates = 0;         // runs that met the amplitude and duration test
    size_t rejected_leak = 0;
    size_t rejected_mask_off = 0;
    size_t rejected_no_baseline = 0;
    double analysed_seconds = 0;   // therapy time the index should be taken over
};

/**
 * Detect apneas in a flow waveform.
 *
 * Every event comes back as EventType::APNEA -- the unclassified apnea, which
 * counts toward AHI under SDD-004. Obstructive and central cannot be told apart
 * without effort belts, so they are not guessed at.
 *
 * Hypopneas are deliberately NOT detected. The AASM rule needs a desaturation or
 * an arousal alongside the airflow drop, and a CPAP gives us neither; emitting a
 * flow-only hypopnea would put it in the AHI numerator on half a rule.
 *
 * @param breaths     from EDFParser::detectBreaths(), over the whole recording
 * @param flow        the same flow those breaths were detected in
 * @param sample_rate Hz
 * @param start       absolute time of flow[0]
 * @param leak        optional leak signal; empty to skip the leak exclusion
 * @param leak_rate   Hz of `leak`
 */
std::vector<SleepEvent> detectFlowApneas(
    const std::vector<EDFParser::BreathCycle>& breaths,
    const std::vector<double>& flow,
    double sample_rate,
    std::chrono::system_clock::time_point start,
    const std::vector<double>& leak = {},
    double leak_rate = 0,
    const FlowEventParams& params = {},
    FlowEventStats* stats = nullptr);

/** Peak-to-peak flow excursion over one breath. Scale-free once compared to a
 *  baseline, which is what lets this run on a machine whose flow units are not
 *  pinned down. */
double breathAmplitude(const std::vector<double>& flow,
                       const EDFParser::BreathCycle& breath);

} // namespace cpapdash::parser
