#ifdef CPAPDASH_WITH_SEFAM

#include "cpapdash/parser/SefamParser.h"

#include <cmath>
#include <set>
#include <string>

namespace cpapdash::parser {

// Sefam records detections as a sampled channel rather than as an annotation
// list: one code per sample, at the rate the INI declares for that channel. An
// event is therefore a run of the same non-zero code.
//
// Every one of them comes out as EventType::OTHER.
//
// We do not know what a Sefam detection code means. The vendor's own software
// distinguishes obstructive from central apneas and hypopneas, reports snoring
// and flow-limitation runs, and there is no published mapping from those to the
// numbers in this channel. Assigning one by guesswork would put invented numbers
// into somebody's AHI, which is worse than reporting none: under SDD-004 an
// OTHER is inside total_events and inside no clinical index, so the codes are
// visible in the data without pretending to a classification.
//
// The map gets filled in when a donor card can be checked against a SEFAM
// Analyze report for the same nights, and not before.
std::vector<SleepEvent> SefamParser::decodeEvents(
    const std::vector<double>& codes,
    int freq,
    std::chrono::system_clock::time_point session_start,
    int* unknown_code_count)
{
    std::vector<SleepEvent> events;
    std::set<int> seen;

    if (freq <= 0 || codes.empty()) {
        if (unknown_code_count) *unknown_code_count = 0;
        return events;
    }

    // A run shorter than a second is not a respiratory event. At 25 Hz a single
    // sample is 40 ms, and emitting one event per sample would turn a noisy or
    // mis-descrambled channel into tens of thousands of events that all look
    // real. Nothing the device detects -- apnea, hypopnea, snore, flow
    // limitation -- is over inside a second.
    const size_t min_run = static_cast<size_t>(freq);

    auto codeAt = [&](size_t i) { return static_cast<int>(std::lround(codes[i])); };

    size_t i = 0;
    while (i < codes.size()) {
        const int code = codeAt(i);
        size_t j = i + 1;
        while (j < codes.size() && codeAt(j) == code) ++j;

        const size_t run = j - i;
        if (code != 0 && run >= min_run) {
            seen.insert(code);

            SleepEvent ev;
            ev.event_type = EventType::OTHER;
            ev.timestamp = session_start + std::chrono::milliseconds(
                static_cast<long long>(std::llround(1000.0 * static_cast<double>(i) / freq)));
            ev.duration_seconds = static_cast<double>(run) / freq;
            ev.details = "Sefam detection code " + std::to_string(code);
            events.push_back(std::move(ev));
        }

        i = j;
    }

    if (unknown_code_count) *unknown_code_count = static_cast<int>(seen.size());
    return events;
}

} // namespace cpapdash::parser

#endif // CPAPDASH_WITH_SEFAM
