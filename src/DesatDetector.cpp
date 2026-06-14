#include "cpapdash/parser/DesatDetector.h"
#include <algorithm>

namespace cpapdash::parser {

namespace {
inline double secondsBetween(std::chrono::system_clock::time_point a,
                             std::chrono::system_clock::time_point b) {
    return std::chrono::duration<double>(b - a).count();
}
}  // namespace

std::vector<DesatEvent> detectDesaturations(
    const std::vector<VitalSample>& samples,
    const DesatParams& params) {
    std::vector<DesatEvent> events;
    if (samples.empty()) return events;

    bool in_event = false;
    DesatEvent cur;
    double baseline_at_open = 0.0;

    for (size_t i = 0; i < samples.size(); ++i) {
        if (!samples[i].spo2.has_value()) continue;
        const double spo2 = samples[i].spo2.value();
        const auto t = samples[i].timestamp;

        // Rolling baseline: max valid SpO2 within the preceding window.
        double baseline = spo2;
        for (size_t j = i; j-- > 0;) {
            if (secondsBetween(samples[j].timestamp, t) > params.baseline_secs) break;
            if (samples[j].spo2.has_value())
                baseline = std::max(baseline, samples[j].spo2.value());
        }

        const double drop = baseline - spo2;

        if (!in_event) {
            if (drop >= params.drop_pct) {
                in_event = true;
                cur = DesatEvent{};
                cur.onset = t;
                cur.nadir = spo2;
                baseline_at_open = baseline;
            }
        } else {
            cur.nadir = std::min(cur.nadir, spo2);
            if (drop < params.recover_pct) {  // recovered
                const double dur = secondsBetween(cur.onset, t);
                if (dur >= params.min_seconds) {
                    cur.duration_seconds = dur;
                    cur.depth = baseline_at_open - cur.nadir;
                    events.push_back(cur);
                }
                in_event = false;
            }
        }
    }

    // Close a desaturation still open at end-of-record (e.g. recording cut off).
    if (in_event) {
        const double dur = secondsBetween(cur.onset, samples.back().timestamp);
        if (dur >= params.min_seconds) {
            cur.duration_seconds = dur;
            cur.depth = baseline_at_open - cur.nadir;
            events.push_back(cur);
        }
    }

    return events;
}

}  // namespace cpapdash::parser
