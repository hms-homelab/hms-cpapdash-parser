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
// Build:
//   cmake -S . -B build -DCPAPDASH_PARSER_BUILD_TOOLS=ON
//   ./build/flow_events_validate <card root or DATALOG folder>

#include "cpapdash/parser/EDFParser.h"
#include "cpapdash/parser/FlowEvents.h"
#include "cpapdash/parser/ISessionParser.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
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
    int reported = 0, detected = 0, matched = 0;
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

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: flow_events_validate <card root or DATALOG folder> [--diagnose]\n";
        return 2;
    }

    bool diag = false;
    for (int i = 2; i < argc; ++i)
        if (std::string(argv[i]) == "--diagnose") diag = true;

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

    std::printf("%-10s %6s %9s %9s %8s %8s %8s\n",
                "night", "hours", "reported", "detected", "matched", "AI_rep", "AI_det");

    std::vector<Night> nights;
    int total_reported = 0, total_detected = 0, total_matched = 0;

    for (const auto& dir : nights_paths) {
        auto session = EDFParser::parseSession(dir.string(), "validate", "");
        if (!session || !session->session_start) continue;

        // The machine's own apneas.
        std::vector<SleepEvent> reported;
        for (const auto& e : session->events)
            if (isApnea(e.event_type)) reported.push_back(e);

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
                                                edf.getStartTime());
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

        std::printf("%-10s %6.2f %9d %9d %8d %8.2f %8.2f\n",
                    n.date.c_str(), n.hours, n.reported, n.detected, n.matched,
                    n.ai_reported, n.ai_detected);
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
