#include "cpapdash/parser/EDFParser.h"
#include <iostream>
#include <cmath>
#include <algorithm>

namespace cpapdash::parser {

bool EDFParser::parsePLDFile(EDFFile& edf, ParsedSession& session) {
    auto start_time = edf.getStartTime();

    // Find PLD signals by label
    int mask_press_idx = edf.findSignal("MaskPress");
    int epr_press_idx = edf.findSignal("EprPress");
    int leak_idx = edf.findSignal("Leak");
    int rr_idx = edf.findSignal("RespRate");
    int tv_idx = edf.findSignal("TidVol");
    int mv_idx = edf.findSignal("MinVent");
    int snore_idx = edf.findSignal("Snore");
    int fl_idx = edf.findSignal("FlowLim");
    int tgt_vent_idx = edf.findSignal("TgtVent");  // ASV only, -1 on CPAP/APAP

    if (mask_press_idx < 0 && leak_idx < 0 && rr_idx < 0) {
        std::cerr << "Parser: PLD file has no recognizable signals" << std::endl;
        return false;
    }

    // Read all available signals
    std::vector<double> mask_press_data, epr_press_data, leak_data;
    std::vector<double> rr_data, tv_data, mv_data, snore_data, fl_data, tgt_vent_data;

    if (mask_press_idx >= 0) edf.readSignal(mask_press_idx, mask_press_data);
    if (epr_press_idx >= 0) edf.readSignal(epr_press_idx, epr_press_data);
    if (leak_idx >= 0) edf.readSignal(leak_idx, leak_data);
    if (rr_idx >= 0) edf.readSignal(rr_idx, rr_data);
    if (tv_idx >= 0) edf.readSignal(tv_idx, tv_data);
    if (mv_idx >= 0) edf.readSignal(mv_idx, mv_data);
    if (snore_idx >= 0) edf.readSignal(snore_idx, snore_data);
    if (fl_idx >= 0) edf.readSignal(fl_idx, fl_data);
    if (tgt_vent_idx >= 0) edf.readSignal(tgt_vent_idx, tgt_vent_data);

    // Determine sample count (use largest signal)
    size_t n_samples = 0;
    if (!leak_data.empty()) n_samples = leak_data.size();
    else if (!rr_data.empty()) n_samples = rr_data.size();
    else if (!mask_press_data.empty()) n_samples = mask_press_data.size();

    if (n_samples == 0) {
        return true;
    }

    // PLD is 0.5 Hz = 30 samples per 60-second record
    // Group into per-minute summaries to match BRP's BreathingSummary cadence
    const size_t SAMPLES_PER_MINUTE = 30;
    size_t n_minutes = (n_samples + SAMPLES_PER_MINUTE - 1) / SAMPLES_PER_MINUTE;

    // Helper to compute average of a range
    auto avg_range = [](const std::vector<double>& data, size_t start, size_t end) -> double {
        if (data.empty() || start >= data.size()) return 0.0;
        end = std::min(end, data.size());
        double sum = 0.0;
        int count = 0;
        for (size_t i = start; i < end; ++i) {
            sum += data[i];
            ++count;
        }
        return count > 0 ? sum / count : 0.0;
    };

    // Build per-minute PLD summaries and merge with existing BRP BreathingSummaries
    for (size_t min_idx = 0; min_idx < n_minutes; ++min_idx) {
        size_t start = min_idx * SAMPLES_PER_MINUTE;
        size_t end = std::min(start + SAMPLES_PER_MINUTE, n_samples);

        // Timestamp for this minute
        auto minute_ts = start_time + std::chrono::seconds(min_idx * 60);

        // Find matching BRP BreathingSummary by timestamp (within 30s tolerance)
        BreathingSummary* target = nullptr;
        for (auto& bs : session.breathing_summary) {
            auto diff = std::chrono::duration_cast<std::chrono::seconds>(
                minute_ts - bs.timestamp).count();
            if (std::abs(diff) <= 30) {
                target = &bs;
                break;
            }
        }

        // If no matching BRP minute exists, create a new BreathingSummary
        if (!target) {
            session.breathing_summary.emplace_back(minute_ts);
            target = &session.breathing_summary.back();
        }

        // PLD-exclusive fields (always set from PLD)
        if (!mask_press_data.empty()) {
            target->mask_pressure = avg_range(mask_press_data, start, end);
        }
        if (!epr_press_data.empty()) {
            target->epr_pressure = avg_range(epr_press_data, start, end);
        }
        if (!snore_data.empty()) {
            target->snore_index = avg_range(snore_data, start, end);
        }
        if (!tgt_vent_data.empty()) {
            target->target_ventilation = avg_range(tgt_vent_data, start, end);
        }

        // PLD overwrites BRP-derived values (machine's calculations are authoritative)
        if (!leak_data.empty()) {
            target->leak_rate = avg_range(leak_data, start, end) * 60.0;  // L/s -> L/min
        }
        if (!rr_data.empty()) {
            target->respiratory_rate = avg_range(rr_data, start, end);
        }
        if (!tv_data.empty()) {
            target->tidal_volume = avg_range(tv_data, start, end) * 1000.0;  // L -> mL
        }
        if (!mv_data.empty()) {
            target->minute_ventilation = avg_range(mv_data, start, end);
        }
        if (!fl_data.empty()) {
            target->flow_limitation = avg_range(fl_data, start, end);
        }
    }

    return true;
}

} // namespace cpapdash::parser
