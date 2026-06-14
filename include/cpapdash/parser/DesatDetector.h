#pragma once

#include "cpapdash/parser/Models.h"
#include <vector>

namespace cpapdash::parser {

/**
 * Parameters for SpO2 desaturation detection.
 *
 * Defaults follow the AASM 3% rule, matching the existing O2Ring/VLD path
 * (VLDParser::calculateMetrics). A drop opens an event; recovery toward the
 * rolling baseline closes it; events shorter than min_seconds are ignored.
 */
struct DesatParams {
    double drop_pct      = 3.0;    // % below rolling baseline to open an event
    double recover_pct   = 1.0;    // % below baseline to close an event
    double baseline_secs = 120.0;  // rolling baseline = max SpO2 in this window
    double min_seconds   = 8.0;    // minimum sustained duration to record
};

/**
 * Detect SpO2 desaturation events from a time-ordered vitals series.
 *
 * Uses absolute timestamps (not a fixed sample interval), so it works for
 * the SAD/machine-SpO2 path (1 Hz) as well as irregularly sampled sources.
 * Samples without a valid SpO2 are skipped. Returns events in onset order.
 */
std::vector<DesatEvent> detectDesaturations(
    const std::vector<VitalSample>& samples,
    const DesatParams& params = {});

}  // namespace cpapdash::parser
