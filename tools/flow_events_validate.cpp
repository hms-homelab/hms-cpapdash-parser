// flow_events_validate -- does the flow-derived apnea detector agree with a
// machine that reports its own apneas?
//
// SDD-006 section 6. Point it at a ResMed card. Every DATALOG date folder that
// has both _EVE.edf (the machine's own annotations) and _BRP.edf (the flow it
// saw when it made them) is one labelled night: run the detector on the flow,
// compare what it found against what the machine reported.
//
// ResMed's detector is a commercial black box, so this is a REFERENCE and not a
// truth. What it establishes is whether our numbers are in the same clinical
// world as a device people are actually treated on.
//
// A tool and not a test, deliberately: tests never run on a real person's
// therapy data. The unit tests use synthetic flow with events written into it.
//
// SDD-014 D6 adds the per-event record (--events), matching by onset within a
// tolerance (--tolerance, 5 s), and a Luna card against its own .evt (--bmc,
// in a build with CPAPDASH_PARSER_WITH_BMC). A ResMed stamps an event at its
// end, so its onset is the stamp less the duration.
//
// Build:
//   cmake -S . -B build -DCPAPDASH_PARSER_BUILD_TOOLS=ON
//   ./build/flow_events_validate <card root or DATALOG folder> [--events]
//   ./build/flow_events_validate --bmc <Luna card folder>

#include "cpapdash/parser/EDFParser.h"
#include "cpapdash/parser/FlowEvents.h"
#include "cpapdash/parser/ISessionParser.h"

#ifdef CPAPDASH_WITH_BMC
#include "cpapdash/parser/BmcParser.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

using namespace cpapdash::parser;
namespace fs = std::filesystem;

namespace {

bool isApnea(EventType t) {
    return t == EventType::APNEA || t == EventType::OBSTRUCTIVE ||
           t == EventType::CENTRAL || t == EventType::CLEAR_AIRWAY;
}

struct Night {
    std::string date;
    double hours = 0;
    int reported = 0, detected = 0, matched = 0, within = 0;
    double ai_reported = 0, ai_detected = 0;
};

// Two events match when they overlap in time at all. Onset alone is too strict:
// the machine stamps an event where its own algorithm decided, which need not be
// the same instant airflow stopped.
bool overlaps(const SleepEvent& a, const SleepEvent& b) {
    const auto a0 = a.timestamp;
    const auto a1 = a0 + std::chrono::milliseconds(
        static_cast<long long>(a.duration_seconds * 1000));
    const auto b0 = b.timestamp;
    const auto b1 = b0 + std::chrono::milliseconds(
        static_cast<long long>(b.duration_seconds * 1000));
    return a0 < b1 && b0 < a1;
}

// The stricter test SDD-014 D6 asks for: onsets within `tol` seconds.
double onsetGap(const SleepEvent& a, const SleepEvent& b) {
    return std::chrono::duration<double>(a.timestamp - b.timestamp).count();
}

// A ResMed stamps a respiratory event at its END (docs of EDFWriteAnnotation);
// the onset is the stamp less the duration.
SleepEvent onsetOf(SleepEvent e) {
    e.timestamp -= std::chrono::milliseconds(static_cast<long long>(e.duration_seconds * 1000));
    return e;
}

std::string wall(std::chrono::system_clock::time_point tp) {
    const std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm lt{};
    localtime_r(&t, &lt);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &lt);
    return buf;
}

// Every reported event with the nearest detected onset, and every detection no
// reported event is near: the per-event record SDD-014 D6 keeps.
void listEvents(const std::vector<SleepEvent>& reported, const std::vector<SleepEvent>& detected,
                double tol) {
    for (const auto& r : reported) {
        const SleepEvent* best = nullptr;
        for (const auto& d : detected)
            if (!best || std::fabs(onsetGap(d, r)) < std::fabs(onsetGap(*best, r))) best = &d;
        if (best)
            std::printf("    machine %s %5.1f s  nearest ours %+8.1f s (%5.1f s)%s\n",
                        wall(r.timestamp).c_str(), r.duration_seconds, onsetGap(*best, r),
                        best->duration_seconds,
                        std::fabs(onsetGap(*best, r)) <= tol ? "  MATCH" : "");
        else
            std::printf("    machine %s %5.1f s  nothing of ours\n", wall(r.timestamp).c_str(),
                        r.duration_seconds);
    }
    for (const auto& d : detected) {
        bool near = false;
        for (const auto& r : reported)
            if (std::fabs(onsetGap(d, r)) <= tol) near = true;
        if (!near)
            std::printf("    ours    %s %5.1f s  no machine event within %.0f s\n",
                        wall(d.timestamp).c_str(), d.duration_seconds, tol);
    }
}

std::vector<fs::path> datalogFolders(const fs::path& root) {
    std::vector<fs::path> out;
    std::error_code ec;

    fs::path datalog = root;
    if (fs::is_directory(root / "DATALOG", ec)) datalog = root / "DATALOG";

    for (const auto& e : fs::directory_iterator(datalog, ec)) {
        if (!e.is_directory(ec)) continue;
        bool eve = false, brp = false;
        for (const auto& f : fs::directory_iterator(e.path(), ec)) {
            const std::string n = f.path().filename().string();
            if (n.size() > 8 && n.rfind("_EVE.edf") == n.size() - 8) eve = true;
            if (n.size() > 8 && n.rfind("_BRP.edf") == n.size() - 8) brp = true;
        }
        if (eve && brp) out.push_back(e.path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace

// Diagnosis, for when the detector disagrees with the machine and the question
// is why. For each apnea the machine reported, measure what the flow ACTUALLY
// does there: peak-to-peak in the reported window, against the trailing
// baseline. That ratio is the drop the detector has to be looking for, measured
// rather than assumed.
static void diagnose(const fs::path& dir, int limit) {
    auto session = EDFParser::parseSession(dir.string(), "diagnose", "");
    if (!session) return;

    std::vector<SleepEvent> reported;
    for (const auto& e : session->events)
        if (isApnea(e.event_type)) reported.push_back(e);
    if (reported.empty()) return;

    std::vector<fs::path> brps;
    for (const auto& f : fs::directory_iterator(dir)) {
        const std::string nm = f.path().filename().string();
        if (nm.size() > 8 && nm.rfind("_BRP.edf") == nm.size() - 8)
            brps.push_back(f.path());
    }
    std::sort(brps.begin(), brps.end());

    std::printf("\n=== %s: what the flow does where the machine reported an apnea ===\n",
                dir.filename().string().c_str());
    std::printf("  ratio_at = flow peak-to-peak over [t, t+dur] / baseline\n"
                "  ratio_before = over [t-dur, t] / baseline\n"
                "  A collapse sits wherever the ratio is near zero.\n");
    std::printf("%8s %8s %10s %10s %10s\n",
                "at_s", "dur_s", "baseline", "ratio_at", "ratio_before");

    int shown = 0;
    for (const auto& brp : brps) {
        EDFFile edf;
        if (!edf.open(brp.string())) continue;
        const int fi = edf.findSignal("Flow");
        if (fi < 0) continue;

        std::vector<double> flow;
        edf.readSignal(fi, flow);
        if (flow.empty()) continue;
        for (double& v : flow) v *= 60.0;

        const double rate = edf.signals[fi].samples_per_record / edf.record_duration;
        if (rate <= 0) continue;
        const auto file_start = edf.getStartTime();
        const auto breaths = EDFParser::detectBreaths(flow, rate);

        auto p2p = [&](double from_s, double to_s) {
            const size_t a = static_cast<size_t>(std::max(0.0, from_s * rate));
            const size_t b = std::min(flow.size(), static_cast<size_t>(to_s * rate));
            if (a >= b) return 0.0;
            double lo = flow[a], hi = flow[a];
            for (size_t k = a; k < b; ++k) { lo = std::min(lo, flow[k]); hi = std::max(hi, flow[k]); }
            return hi - lo;
        };

        for (const auto& r : reported) {
            if (shown >= limit) return;
            const double at_s = std::chrono::duration<double>(
                r.timestamp - file_start).count();
            if (at_s < 0 || at_s * rate >= static_cast<double>(flow.size())) continue;

            const double dur = r.duration_seconds > 0 ? r.duration_seconds : 10.0;
            const double ev_amp = p2p(at_s, at_s + dur);

            // Baseline: median breath amplitude over the two minutes before.
            std::vector<double> window;
            for (const auto& b : breaths) {
                const double bs = b.start_idx / rate;
                if (bs >= at_s) break;
                if (bs >= at_s - 120.0) window.push_back(breathAmplitude(flow, b));
            }
            double base = 0;
            if (!window.empty()) {
                std::sort(window.begin(), window.end());
                base = window[window.size() / 2];
            }

            const double before_amp = p2p(std::max(0.0, at_s - dur), at_s);

            std::printf("%8.1f %8.1f %10.2f %10.3f %10.3f\n", at_s, dur, base,
                        base > 0 ? ev_amp / base : -1.0,
                        base > 0 ? before_amp / base : -1.0);
            ++shown;
        }
    }
}

#ifdef CPAPDASH_WITH_BMC
// Peak-to-peak of the samples with a reading, NaN when none has one.
static double p2pOf(const std::vector<double>& v, size_t a, size_t b) {
    double lo = NAN, hi = NAN;
    for (size_t i = a; i < b && i < v.size(); ++i) {
        if (std::isnan(v[i])) continue;
        lo = std::isnan(lo) ? v[i] : std::min(lo, v[i]);
        hi = std::isnan(hi) ? v[i] : std::max(hi, v[i]);
    }
    return hi - lo;
}

// SDD-014 D6.2: a Luna card's flow apneas against its own .evt, session by
// session, and for each machine apnea what the flow does over [t, t+d] and over
// [t-d, t] against the two minutes before, which says whether the .evt stamps
// an apnea at its start or at its end.
static int validateBmc(const std::string& dir, double tol) {
    const auto card = BmcParser::readCard(dir);
    if (!card) {
        std::cerr << "no BMC card in " << dir << "\n";
        return 1;
    }
    int reported_total = 0, detected_total = 0, within_total = 0;
    for (const auto& span : card->sessions) {
        auto s = BmcParser::sessionFromCard(*card, span, "validate", "");
        FlowEventStats st;
        const auto ours = BmcParser::flowApneas(*card, span, &st);
        if (s->events.empty() && ours.empty()) continue;

        int within = 0;
        for (const auto& r : s->events)
            for (const auto& d : ours)
                if (std::fabs(onsetGap(d, r)) <= tol) { ++within; break; }
        std::printf("session %s  %6d s  machine %d  ours %d  within %.0f s %d  "
                    "(breaths %zu, candidates %zu, leak %zu, mask-off %zu)\n",
                    wall(*s->session_start).c_str(), span.seconds,
                    static_cast<int>(s->events.size()), static_cast<int>(ours.size()), tol,
                    within, st.breaths, st.candidates, st.rejected_leak, st.rejected_mask_off);
        listEvents(s->events, ours, tol);

        for (const auto& r : s->events) {
            const auto t0 = *s->session_start;
            const long at = static_cast<long>(std::chrono::duration<double>(r.timestamp - t0).count());
            const long d = static_cast<long>(std::lround(r.duration_seconds));
            const BmcCivilSeconds base = span.start + std::max(0L, at - d - 120);
            const auto w = BmcParser::waveform(*card, base, span.start + at + d + 1);
            const size_t hz = kBmcWaveHz;
            const size_t t_i = static_cast<size_t>(span.start + at - base) * hz;
            const size_t pre = t_i > static_cast<size_t>(d) * hz ? t_i - d * hz : 0;
            std::vector<double> windows;
            for (size_t k = 0; k + 4 * hz <= pre; k += 4 * hz) {
                const double v = p2pOf(w.flow, k, k + 4 * hz);
                if (!std::isnan(v)) windows.push_back(v);
            }
            std::sort(windows.begin(), windows.end());
            const double b = windows.empty() ? NAN : windows[windows.size() / 2];
            std::printf("    flow at machine %s: [t,t+d] %.2f  [t-d,t] %.2f of baseline %.1f L/min\n",
                        wall(r.timestamp).c_str(), p2pOf(w.flow, t_i, t_i + d * hz) / b,
                        p2pOf(w.flow, pre, t_i) / b, b);
        }
        reported_total += static_cast<int>(s->events.size());
        detected_total += static_cast<int>(ours.size());
        within_total += within;
    }
    std::printf("\nmachine apneas %d, ours %d, machine apneas with one of ours within %.0f s: %d\n",
                reported_total, detected_total, tol, within_total);
    return 0;
}
#endif

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: flow_events_validate <card root or DATALOG folder> [--diagnose]\n"
                     "         [--events] [--tolerance S] [--drop F]\n"
                     "       flow_events_validate --bmc <Luna card folder> [--tolerance S]\n";
        return 2;
    }

    bool diag = false, list = false;
    double tol = 5.0;
    FlowEventParams params;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--diagnose") diag = true;
        else if (a == "--events") list = true;
        else if (a == "--tolerance" && i + 1 < argc) tol = std::atof(argv[++i]);
        else if (a == "--drop" && i + 1 < argc) params.drop_fraction = std::atof(argv[++i]);
    }

    if (std::string(argv[1]) == "--bmc") {
#ifdef CPAPDASH_WITH_BMC
        if (argc < 3) return 2;
        return validateBmc(argv[2], tol);
#else
        std::cerr << "built without CPAPDASH_PARSER_WITH_BMC\n";
        return 2;
#endif
    }

    if (diag) {
        const auto folders = datalogFolders(argv[1]);
        for (size_t i = 0; i < folders.size() && i < 40; ++i) diagnose(folders[i], 40);
        return 0;
    }

    const auto nights_paths = datalogFolders(argv[1]);
    if (nights_paths.empty()) {
        std::cerr << "no date folders with both _EVE.edf and _BRP.edf under "
                  << argv[1] << "\n";
        return 1;
    }

    std::printf("%-10s %6s %9s %9s %8s %8s %8s %8s   (within: onsets %.0f s apart)\n",
                "night", "hours", "reported", "detected", "matched", "within", "AI_rep", "AI_det",
                tol);

    std::vector<Night> nights;
    int total_reported = 0, total_detected = 0, total_matched = 0;

    for (const auto& dir : nights_paths) {
        auto session = EDFParser::parseSession(dir.string(), "validate", "");
        if (!session || !session->session_start) continue;

        // The machine's own apneas, moved from the stamp at their end to their
        // onset so both sides are compared by the same instant.
        std::vector<SleepEvent> reported;
        for (const auto& e : session->events)
            if (isApnea(e.event_type)) reported.push_back(onsetOf(e));

        // Ours, from the same night's flow. A night is several BRP checkpoints,
        // so each is read, detected in, and its events appended -- the same
        // shape parseSession() builds its per-minute rows from.
        std::vector<SleepEvent> detected;
        double analysed_hours = 0;

        std::vector<fs::path> brps;
        for (const auto& f : fs::directory_iterator(dir)) {
            const std::string nm = f.path().filename().string();
            if (nm.size() > 8 && nm.rfind("_BRP.edf") == nm.size() - 8)
                brps.push_back(f.path());
        }
        std::sort(brps.begin(), brps.end());

        for (const auto& brp : brps) {
            EDFFile edf;
            if (!edf.open(brp.string())) continue;

            const int flow_idx = edf.findSignal("Flow");
            if (flow_idx < 0) continue;

            std::vector<double> flow;
            edf.readSignal(flow_idx, flow);
            if (flow.empty()) continue;

            // ResMed stores flow in L/sec; the rest of the parser works in
            // L/min. The detector is scale-free -- it compares against its own
            // baseline -- but keeping units consistent keeps the leak threshold
            // meaningful.
            for (double& v : flow) v *= 60.0;

            const double rate =
                edf.signals[flow_idx].samples_per_record / edf.record_duration;
            if (rate <= 0) continue;

            const auto breaths = EDFParser::detectBreaths(flow, rate);
            const auto found = detectFlowApneas(breaths, flow, rate,
                                                edf.getStartTime(), {}, 0, params);
            detected.insert(detected.end(), found.begin(), found.end());
            analysed_hours += flow.size() / rate / 3600.0;
        }

        int matched = 0;
        for (const auto& d : detected)
            for (const auto& r : reported)
                if (overlaps(d, r)) { ++matched; break; }

        Night n;
        n.date = dir.filename().string();
        n.hours = analysed_hours > 0 ? analysed_hours
                : (session->duration_seconds ? *session->duration_seconds / 3600.0 : 0);
        n.reported = static_cast<int>(reported.size());
        n.detected = static_cast<int>(detected.size());
        n.matched = matched;
        n.ai_reported = n.hours > 0 ? n.reported / n.hours : 0;
        n.ai_detected = n.hours > 0 ? n.detected / n.hours : 0;

        nights.push_back(n);
        total_reported += n.reported;
        total_detected += n.detected;
        total_matched += matched;

        for (const auto& r : reported)
            for (const auto& d : detected)
                if (std::fabs(onsetGap(d, r)) <= tol) { ++n.within; break; }

        std::printf("%-10s %6.2f %9d %9d %8d %8d %8.2f %8.2f\n",
                    n.date.c_str(), n.hours, n.reported, n.detected, n.matched, n.within,
                    n.ai_reported, n.ai_detected);
        if (list) listEvents(reported, detected, tol);
    }

    std::printf("\n%zu nights, %d reported apneas, %d detected, %d matched\n",
                nights.size(), total_reported, total_detected, total_matched);

    if (total_detected) {
        const double precision = static_cast<double>(total_matched) / total_detected;
        const double recall = total_reported
            ? static_cast<double>(total_matched) / total_reported : 0;
        std::printf("precision %.3f  recall %.3f  F1 %.3f\n", precision, recall,
                    precision + recall > 0 ? 2 * precision * recall / (precision + recall) : 0);
    }
    return 0;
}
