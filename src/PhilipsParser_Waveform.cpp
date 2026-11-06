#ifdef CPAPDASH_WITH_PHILIPS

// .005 waveform: flow signal only (SDD-002 decision 2).
// PROVENANCE: docs/PHILIPS_FORMAT.md section 6 ([A] wiki waveform header +
// interleave, [B] cpap-lib ReadSignals/BuildFlowSignal @f760b21: signed
// 8-bit L/min at 0.2 s/sample). nsig>1 chunks are counted + skipped: the
// flow position within a multiplex is undocumented (doc Q4). No OSCAR source.

#include "cpapdash/parser/PhilipsParser.h"
#include "cpapdash/parser/EDFParser.h"
#include <cmath>
#include <limits>
#include <numeric>
#include <algorithm>
#include <iostream>

namespace cpapdash::parser {

std::optional<PhilipsParser::FlowSeries> PhilipsParser::flowSeries(
    const std::vector<PhilipsChunk>& chunks, PhilipsParseWarnings& warnings,
    std::optional<std::chrono::system_clock::time_point> base_hint) {
    // Collect usable flow chunks (single signal)
    struct FlowChunk {
        std::chrono::system_clock::time_point start;
        const std::vector<uint8_t>* samples;
        double sample_rate;
    };
    std::vector<FlowChunk> flow_chunks;

    for (const auto& chunk : chunks) {
        if (chunk.file_type != 1 || chunk.data.empty()) {
            warnings.unsupported_chunks++;
            continue;
        }
        if (chunk.waveforms.size() != 1) {
            // Multiplexed signals: flow position undocumented (doc Q4)
            warnings.unsupported_chunks++;
            continue;
        }
        int duration = chunk.durationSeconds();
        if (duration <= 0) {
            warnings.unsupported_chunks++;
            continue;
        }
        // Derive the sample rate from the block itself (doc 6: verify ~5 Hz,
        // never hardcode).
        double rate = static_cast<double>(chunk.data.size()) / duration;
        if (rate < 1.0 || rate > 100.0) {
            warnings.unsupported_chunks++;
            continue;
        }
        flow_chunks.push_back({chunk.timestamp, &chunk.data, rate});
    }

    if (flow_chunks.empty()) return std::nullopt;

    std::sort(flow_chunks.begin(), flow_chunks.end(),
              [](const FlowChunk& a, const FlowChunk& b) { return a.start < b.start; });

    const auto base = base_hint.value_or(flow_chunks.front().start);
    const double rate = flow_chunks.front().sample_rate;

    auto last_end = flow_chunks.back().start +
                    std::chrono::seconds(static_cast<int>(
                        flow_chunks.back().samples->size() / rate));

    // Assemble one series from base to the last chunk end. Gaps between
    // chunks stay NaN: no zero-fill, gap minutes produce no metrics
    // (deliberate deviation from [B], doc discrepancy 6).
    auto total_secs = std::chrono::duration_cast<std::chrono::seconds>(
        last_end - base).count();
    if (total_secs <= 0) return std::nullopt;

    FlowSeries out;
    out.start = base;
    out.sample_rate = rate;
    out.flow.assign(static_cast<size_t>(total_secs * rate),
                    std::numeric_limits<double>::quiet_NaN());
    auto& flow = out.flow;

    double covered = 0;
    for (const auto& fc : flow_chunks) {
        auto offset_secs = std::chrono::duration_cast<std::chrono::seconds>(
            fc.start - base).count();
        if (offset_secs < 0) { warnings.unsupported_chunks++; continue; }
        size_t offset = static_cast<size_t>(offset_secs * rate);
        for (size_t i = 0; i < fc.samples->size() && offset + i < flow.size(); ++i) {
            // Signed 8-bit sample, value is flow in L/min (doc section 6)
            flow[offset + i] = static_cast<double>(
                static_cast<int8_t>((*fc.samples)[i]));
        }
        covered += static_cast<double>(fc.samples->size()) / fc.sample_rate;
    }
    out.recorded_seconds = static_cast<int>(covered + 0.5);
    return out;
}

bool PhilipsParser::parseWaveform(const std::vector<PhilipsChunk>& chunks,
                                  ParsedSession& session,
                                  PhilipsParseWarnings& warnings) {
    // Anchor the series at the summary-provided session start when available,
    // else at the first chunk.
    auto series = flowSeries(chunks, warnings, session.session_start);
    if (!series) return false;
    const auto base = series->start;
    const double rate = series->sample_rate;
    const std::vector<double>& flow = series->flow;
    if (!session.session_start.has_value()) session.session_start = base;
    const auto last_end = series->end();
    if (!session.session_end.has_value() || last_end > session.session_end.value()) {
        session.session_end = last_end;
    }
    // A session with no summary yet (the machine still recording it, doc 9.9)
    // has no mask-on periods to sum: its therapy so far is the waveform's time.
    if (!session.duration_seconds.has_value() && series->recorded_seconds > 0) {
        session.duration_seconds = series->recorded_seconds;
    }

    // Per-minute summaries; a minute needs >= 50% valid samples to count.
    int samples_per_minute = static_cast<int>(rate * 60);
    if (samples_per_minute <= 0) return false;
    int n_minutes = static_cast<int>(flow.size()) / samples_per_minute;

    // Detect breaths ONCE over the full series (NaN -> zero flow inside the
    // detector) and bucket them by the minute their onset falls in, the same
    // shared detector and the same single pass as the ResMed BRP path. The
    // per-minute summaries and session.breaths below then agree on every
    // breath, including one that straddles a minute boundary (SDD-003 D2).
    const auto breaths = EDFParser::detectBreaths(flow, rate);

    std::vector<std::vector<EDFParser::BreathCycle>> breaths_by_minute(
        static_cast<size_t>(n_minutes));
    for (const auto& b : breaths) {
        const int min_idx = b.start_idx / samples_per_minute;
        if (min_idx >= 0 && min_idx < n_minutes) {
            breaths_by_minute[min_idx].push_back(b);
        }
    }

    for (int min = 0; min < n_minutes; ++min) {
        int start = min * samples_per_minute;
        int end = start + samples_per_minute;

        int valid = 0;
        double sum = 0, maxv = -1e9, minv = 1e9;
        for (int i = start; i < end; ++i) {
            if (std::isnan(flow[i])) continue;
            valid++;
            sum += flow[i];
            maxv = std::max(maxv, flow[i]);
            minv = std::min(minv, flow[i]);
        }
        if (valid < samples_per_minute / 2) continue;

        BreathingSummary summary(base + std::chrono::minutes(min));
        summary.pressure_measured = false;   // the waveform is flow only (doc 6, 9.9)
        summary.avg_flow_rate = sum / valid;
        summary.max_flow_rate = maxv;
        summary.min_flow_rate = minv;

        // Shared pipeline, flow-only (empty pressure), 5 Hz-safe (PR-1)
        EDFParser::calculateRespiratoryMetrics(flow, /*pressure_data=*/{},
                                               breaths_by_minute[min],
                                               rate, min, summary);

        session.breathing_summary.push_back(summary);
    }

    // Breath-by-breath detail from the same single detection pass.
    session.breaths.reserve(session.breaths.size() + breaths.size());
    for (const auto& b : breaths) {
        Breath out;
        out.onset = base + std::chrono::microseconds(
            static_cast<long long>(b.start_idx / rate * 1e6));
        out.tidal_volume = b.tidal_volume;
        out.inspiratory_time = b.inspiratory_time;
        out.expiratory_time = b.expiratory_time;
        out.flow_limitation = b.flow_limitation;
        session.breaths.push_back(out);
    }

    return true;
}

} // namespace cpapdash::parser

#endif // CPAPDASH_WITH_PHILIPS
