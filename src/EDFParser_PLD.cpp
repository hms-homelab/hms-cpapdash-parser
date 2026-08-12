#include "cpapdash/parser/EDFParser.h"
#include <iostream>
#include <cmath>
#include <algorithm>

namespace cpapdash::parser {

bool EDFParser::parsePLDFile(EDFFile& edf, ParsedSession& session) {
    auto start_time = edf.getStartTime();

    // Find PLD signals by label.
    //
    // A ResMed PLD carries THREE pressure channels, in this order: MaskPress.2s
    // (measured at the mask), Press.2s (the therapy pressure the machine delivers)
    // and EprPress.2s (the expiratory set-point, exactly Press minus the EPR level).
    // Press.2s is the one OSCAR and SleepHQ plot as "Pressure"; without it we were
    // reporting mask pressure under that name and reading roughly 0.8 cmH2O low.
    //
    // It MUST be matched on the prefix. findSignal() is a substring search returning
    // the first hit in signal order, and "MaskPress.2s" contains "Press", so
    // findSignal("Press") silently hands back mask pressure. No substring fallback
    // here for that reason -- on this file a fallback would reintroduce the bug.
    int mask_press_idx = edf.findSignal("MaskPress");
    int press_idx = edf.findSignalPrefix("Press");
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
    std::vector<double> mask_press_data, press_data, epr_press_data, leak_data;
    std::vector<double> rr_data, tv_data, mv_data, snore_data, fl_data, tgt_vent_data;

    if (mask_press_idx >= 0) edf.readSignal(mask_press_idx, mask_press_data);
    if (press_idx >= 0) edf.readSignal(press_idx, press_data);
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

    // Keep the samples at the machine's own 0.5 Hz, alongside the per-minute rows
    // built below. calculateMetrics() takes the published mean/median/95th/max from
    // these, because a percentile of 30-sample means is not a percentile of the
    // signal: it puts leak's 95th at 8.68 L/min where the samples (and the machine's
    // own STR summary) say 8.4, and it flattens a real 87.6 L/min blow-out to 10.8.
    {
        auto& nsamp = session.native_samples;
        auto take = [](const std::vector<double>& src, std::vector<double>& dst, double scale) {
            if (src.empty()) return;
            dst.reserve(dst.size() + src.size());
            for (double x : src) dst.push_back(x * scale);
        };
        take(leak_data, nsamp.leak, 60.0);      // L/s -> L/min
        take(press_data, nsamp.therapy, 1.0);
        take(mask_press_data, nsamp.mask, 1.0);
    }

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

        // The NEAREST BRP minute within 30s, not merely the first one inside it.
        //
        // This took the first row within tolerance and broke out. When more than
        // one BRP row sits inside a 30-second window the first is not necessarily
        // the closest, so the wrong minute could be claimed while the one that
        // should have had these values kept none.
        //
        // Whether that has ever happened in the field is NOT established. It was
        // written while chasing the chart gaps in ticket 67 and does not explain
        // them: on the night investigated the BRP checkpoints were hours apart,
        // so only one row was ever in range and both matchers behave identically.
        // Those gaps line up with checkpoints the machine simply never wrote a
        // PLD for — five BRP, three PLD — which no matcher can invent, and which
        // STR cannot fill either since it carries one row per night rather than
        // per minute. This is a correctness fix on its own terms, not that fix.
        BreathingSummary* target = nullptr;
        long best = 31;   // strictly outside the tolerance, so 30 still wins
        for (auto& bs : session.breathing_summary) {
            const long diff = std::labs(
                static_cast<long>(std::chrono::duration_cast<std::chrono::seconds>(
                    minute_ts - bs.timestamp).count()));
            if (diff < best) {
                best = diff;
                target = &bs;
                if (diff == 0) break;   // nothing can beat an exact hit
            }
        }

        // If no matching BRP minute exists, create a new BreathingSummary
        if (!target) {
            session.breathing_summary.emplace_back(minute_ts);
            target = &session.breathing_summary.back();
        }

        // PLD-exclusive fields (always set from PLD)
        if (!press_data.empty()) {
            target->therapy_pressure = avg_range(press_data, start, end);
        }
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
            // Keep the minute's extremes too. The mean alone hides blow-outs: one
            // 1.46 L/s sample inside a minute of otherwise-zero leak is a real
            // 87.6 L/min event that averages down to 4.8 and disappears.
            size_t lo = std::min(start, leak_data.size());
            size_t hi = std::min(end, leak_data.size());
            if (lo < hi) {
                auto mm = std::minmax_element(leak_data.begin() + lo, leak_data.begin() + hi);
                target->leak_min = *mm.first * 60.0;
                target->leak_max = *mm.second * 60.0;
            }
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
