// Reads a BMC / React Health Luna card folder and reports what BmcParser made of
// it: every session, its duration, its apneas and the index, and the card-level
// notes (refused packets, duplicate seconds, clock order, span types). Then, for
// each session of ten minutes or more, the checks that the flow and pressure
// read plausibly (docs/BMC_FORMAT.md, "Flow" and "Pressure").
//
// A tool and not a test: it runs on a real person's card, which never enters this
// repository. Its output on a real card is the accuracy check the synthetic
// fixtures in tests/ cannot be.
//
//   bmc_probe <card folder>

#include "cpapdash/parser/BmcParser.h"
#include "cpapdash/parser/EDFParser.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <map>
#include <vector>

using namespace cpapdash::parser;

namespace {

std::string fmt(std::chrono::system_clock::time_point tp) {
    const std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm lt{};
    localtime_r(&t, &lt);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &lt);
    return buf;
}

double median(std::vector<double> v) {
    if (v.empty()) return NAN;
    std::sort(v.begin(), v.end());
    const size_t h = v.size() / 2;
    return v.size() % 2 ? v[h] : (v[h - 1] + v[h]) / 2;
}

double pct(std::vector<double> v, double p) {
    if (v.empty()) return NAN;
    std::sort(v.begin(), v.end());
    return v[static_cast<size_t>(p / 100.0 * (v.size() - 1))];
}

double corr(const std::vector<double>& a, const std::vector<double>& b) {
    const size_t n = std::min(a.size(), b.size());
    if (n < 3) return NAN;
    double ma = 0, mb = 0;
    for (size_t i = 0; i < n; ++i) ma += a[i], mb += b[i];
    ma /= n, mb /= n;
    double sab = 0, saa = 0, sbb = 0;
    for (size_t i = 0; i < n; ++i) {
        sab += (a[i] - ma) * (b[i] - mb);
        saa += (a[i] - ma) * (a[i] - ma);
        sbb += (b[i] - mb) * (b[i] - mb);
    }
    return sab / std::sqrt(saa * sbb);
}

// Flow and pressure plausibility for one session.
void checkWaves(const BmcCard& card, const BmcSessionSpan& span, const ParsedSession& s) {
    const auto w = BmcParser::waveform(card, span.start, span.end + 1);

    // Per-second words of the session, by second.
    const size_t secs = static_cast<size_t>(span.end - span.start + 1);
    std::vector<int> epap(secs, -1), ipap(secs, -1), leak(secs, -1), w105(secs, -1);
    for (const auto& p : card.packets) {
        if (p.stamp < span.start || p.stamp > span.end) continue;
        const size_t k = static_cast<size_t>(p.stamp - span.start);
        if (p.words[bmc_word::kEpap] != kBmcInvalid) epap[k] = p.words[bmc_word::kEpap];
        if (p.words[bmc_word::kIpap] != kBmcInvalid) ipap[k] = p.words[bmc_word::kIpap];
        if (p.words[bmc_word::kLeak] != kBmcInvalid) leak[k] = p.words[bmc_word::kLeak];
        if (p.words[bmc_word::kIeRatio] != kBmcInvalid && p.words[bmc_word::kIeRatio] != 0)
            w105[k] = p.words[bmc_word::kIeRatio];
    }

    // Minutes.
    int flow_min = 0, press_min = 0;
    std::vector<double> avg_flow, total_leak, vt, mv, p_minus_set, pmin_minus_epap;
    for (const auto& m : s.breathing_summary) {
        if (m.flow_measured) {
            ++flow_min;
            avg_flow.push_back(m.avg_flow_rate);
            if (m.total_leak) total_leak.push_back(*m.total_leak);
        }
        if (m.pressure_measured) {
            ++press_min;
            if (m.therapy_pressure && m.epr_pressure) {
                // The minute's mean pressure against the set pressure (EPAP on
                // a CPAP, where IPAP = EPAP; the mean of the two on a bi-level).
                p_minus_set.push_back(m.avg_pressure - (*m.therapy_pressure + *m.epr_pressure) / 2);
            }
        }
        if (m.tidal_volume) vt.push_back(*m.tidal_volume);
        if (m.minute_ventilation) mv.push_back(*m.minute_ventilation);
    }
    std::printf("    minutes %zu: flow measured %d, pressure measured %d\n",
                s.breathing_summary.size(), flow_min, press_min);
    std::printf("    patient flow, minute mean: median %.2f L/min (p5 %.2f, p95 %.2f)\n",
                median(avg_flow), pct(avg_flow, 5), pct(avg_flow, 95));
    std::printf("    total flow (vent + leak), minute mean: median %.1f L/min (p5 %.1f, p95 %.1f)\n",
                median(total_leak), pct(total_leak, 5), pct(total_leak, 95));
    std::printf("    Vt (word 99) median %.0f mL (p5 %.0f, p95 %.0f); MV (word 101) median %.1f L/min\n",
                median(vt), pct(vt, 5), pct(vt, 95), median(mv));
    if (!w.pressure.empty())
        std::printf("    pressure, minute mean - set: median %+.2f cmH2O (p5 %+.2f, p95 %+.2f)\n",
                    median(p_minus_set), pct(p_minus_set, 5), pct(p_minus_set, 95));

    // End-expiratory pauses, where the machine sits at EPAP (the set pressure
    // on a CPAP). In each 30 s window with EPAP and IPAP unchanged and the leak
    // under 20 L/min: samples where the flow is flat (a step of 0.4 L/min or
    // less) and near the window's median (within 15% of its p5..p95 span), in
    // runs of 0.4 s or more, the first 3 samples of each run dropped. The
    // window's median pressure over those samples is one point.
    if (!w.pressure.empty()) {
        std::vector<double> miss;
        for (size_t s0 = 0; s0 + 30 <= secs; s0 += 30) {
            bool steady = true;
            for (size_t k = s0; k < s0 + 30 && steady; ++k)
                steady = epap[k] >= 0 && epap[k] == epap[s0] && ipap[k] == ipap[s0] &&
                         leak[k] >= 0 && leak[k] < 200;
            if (!steady) continue;
            const size_t a = s0 * kBmcWaveHz, b = (s0 + 30) * kBmcWaveHz;
            std::vector<double> f(w.flow.begin() + a, w.flow.begin() + b);
            if (std::any_of(f.begin(), f.end(), [](double x) { return std::isnan(x); })) continue;
            const double med = median(f), span90 = pct(f, 95) - pct(f, 5);
            std::vector<double> at;
            size_t run = 0;
            for (size_t i = 0; i <= f.size(); ++i) {
                const bool flat = i < f.size() && std::fabs(f[i] - f[i ? i - 1 : 0]) <= 0.4 &&
                                  std::fabs(f[i] - med) <= 0.15 * span90;
                if (flat) {
                    ++run;
                    continue;
                }
                if (run >= 10)
                    for (size_t j = i - run + 3; j < i; ++j)
                        if (!std::isnan(w.pressure[a + j])) at.push_back(w.pressure[a + j]);
                run = 0;
            }
            if (at.size() >= 10) miss.push_back(median(at) - epap[s0] / 2.0);
        }
        double ss = 0;
        for (double d : miss) ss += d * d;
        std::printf("    pause pressure - EPAP: %zu windows, median %+.3f, rms %.3f cmH2O\n",
                    miss.size(), median(miss), miss.empty() ? NAN : std::sqrt(ss / miss.size()));
    }

    // Breaths found per minute against the machine's rate (word 104), minutes
    // with the leak under 90 L/min; and our Te/Ti against word 105.
    std::map<int64_t, int> count;
    const auto start = bmcToTimePoint(span.start);
    for (const auto& b : s.breaths)
        ++count[std::chrono::duration_cast<std::chrono::seconds>(b.onset - start).count() / 60];
    int within2 = 0, minutes = 0;
    std::vector<double> ie_diff;
    for (size_t k = 0; k < s.breathing_summary.size(); ++k) {
        const auto& m = s.breathing_summary[k];
        if (!m.respiratory_rate || !m.leak_rate || *m.leak_rate >= 90 || !m.flow_measured) continue;
        ++minutes;
        if (std::fabs(count[static_cast<int64_t>(k)] - *m.respiratory_rate) <= 2) ++within2;
        double sum = 0;
        int n = 0;
        for (size_t sec = k * 60; sec < std::min(secs, (k + 1) * 60); ++sec)
            if (w105[sec] > 0) sum += w105[sec] / 10.0, ++n;
        if (n && m.ie_ratio && *m.ie_ratio > 0) ie_diff.push_back(std::fabs(1.0 / *m.ie_ratio - sum / n));
    }
    std::printf("    breaths/min within 2 of word 104: %d of %d minutes (%.1f%%)\n", within2, minutes,
                minutes ? 100.0 * within2 / minutes : NAN);
    std::printf("    Te/Ti from breaths vs word 105 / 10: median |diff| %.3f over %zu minutes\n",
                median(ie_diff), ie_diff.size());

    // Our inspiratory integral against word 99, per minute.
    const auto cycles = EDFParser::detectBreaths(w.flow, kBmcWaveHz);
    std::map<int64_t, std::pair<double, int>> integral;
    for (const auto& c : cycles) {
        auto& e = integral[c.start_idx / (60 * kBmcWaveHz)];
        e.first += c.tidal_volume;
        ++e.second;
    }
    std::vector<double> ours, machine;
    for (size_t k = 0; k < s.breathing_summary.size(); ++k) {
        const auto& m = s.breathing_summary[k];
        const auto it = integral.find(static_cast<int64_t>(k));
        if (!m.tidal_volume || it == integral.end() || !m.leak_rate || *m.leak_rate >= 90) continue;
        ours.push_back(it->second.first / it->second.second);
        machine.push_back(*m.tidal_volume);
    }
    std::vector<double> ratio;
    for (size_t i = 0; i < ours.size(); ++i) ratio.push_back(machine[i] / ours[i]);
    std::printf("    Vt word 99 vs our integral (flat leak baseline): r %.3f, word/ours median %.3f over %zu minutes\n",
                corr(ours, machine), median(ratio), ours.size());
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <card folder>\n", argv[0]);
        return 2;
    }
    const auto card = BmcParser::readCard(argv[1]);
    if (!card) {
        std::fprintf(stderr, "no BMC card (serial-named raw files + .evt) in %s\n", argv[1]);
        return 1;
    }
    const auto& n = card->notes;
    const auto scale = bmcPressureScale(card->model);
    std::printf("model \"%s\": %s\n", card->model.c_str(),
                scale ? "pressure scale measured" : "no pressure scale (flow only)");
    std::printf("packets %d, refused %d, duplicate seconds %d, out of order %s\n", n.packets,
                n.refused_packets, n.duplicate_seconds, n.out_of_order ? "yes" : "no");
    std::printf("event records %zu, refused %d, apneas outside every session %d\n",
                card->events.size(), n.refused_event_records, n.events_outside_session);
    std::printf("sessions %d\n", n.sessions);

    int total_events = 0;
    BmcNotes all;
    for (const auto& span : card->sessions) {
        BmcNotes per;
        auto s = BmcParser::sessionFromCard(*card, span, "probe", "BMC", &per);
        const auto& m = *s->metrics;
        std::printf("  %s -> %s  %6d s  (#%d)  OA %d  CA %d  AHI %.2f  IPAP %.2f  EPAP %.2f%s\n",
                    fmt(*s->session_start).c_str(), fmt(*s->session_end).c_str(),
                    *s->duration_seconds, span.session_number, m.obstructive_apneas,
                    m.clear_airway_apneas, m.ahi, m.avg_therapy_pressure.value_or(-1),
                    m.avg_epr_pressure.value_or(-1),
                    m.index_kind == SessionMetrics::IndexKind::Ungraded ? " (ungraded)" : "");
        if (span.seconds >= 600) checkWaves(*card, span, *s);
        total_events += per.events_emitted;
        for (const auto& [type, count] : per.span_types) all.span_types[type] += count;
    }
    std::printf("apneas placed %d\n", total_events);
    std::printf("span types (not events):");
    for (const auto& [type, count] : all.span_types) std::printf(" %d:%d", type, count);
    std::printf("\n");
    return 0;
}
