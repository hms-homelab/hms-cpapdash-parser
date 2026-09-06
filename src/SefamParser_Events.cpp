#ifdef CPAPDASH_WITH_SEFAM

#include "cpapdash/parser/SefamParser.h"

#include <cmath>
#include <map>
#include <string>
#include <vector>

namespace cpapdash::parser {

// Sefam records detections as a sampled bitfield rather than an annotation list:
// one byte per sample at the DET channel's rate, several flags set at once.
//
// WHICH BIT IS WHICH was established from the card itself, without the vendor's
// software, using two tests that need no external truth because the clinical
// definitions constrain what each flag can look like.
//
// 1. RUN-LENGTH SHAPE, over 40 nights and 131 hours. An apnea is by definition
//    at least ten seconds, so its flag's runs cluster there; a breath phase
//    lasts one breath.
// 2. FLOW COLLAPSE, over 12 nights. During a real apnea the airflow stops, so
//    the peak-to-peak flow inside a run, over the peak-to-peak in the two
//    minutes before it, must be near zero. A flag that rides normal breathing
//    shows no drop at all.
//
//   bit  rate     median run   flow ratio   reading
//   ---  -------  -----------  -----------  --------------------------------
//    0    76/h        4.5 s       0.74      borderline; possible hypopnea
//    1   127/h        8.7 s       0.83      not an event
//    2     5/h       10.4 s       0.06      APNEA -- a 94% collapse
//    3     9/h        0.4 s        --       too rare and short to judge
//    4     5/h       13.6 s       0.89      event-SHAPED but no airflow drop
//    5   841/h        1.6 s        --       breath phase
//    6   839/h        0.6 s        --       breath phase
//    7    25/h       16.4 s       0.86      flow limitation or snoring
//
// Bit 2 is the only bit whose long runs coincide with airflow actually
// stopping; the next lowest ratio that occurs often is 0.74. It is also the only
// bit that correlated with apneas derived independently from the flow waveform,
// found before any of this and by an unrelated method.
//
// Two things in that table are worth keeping in mind. Bits 5 and 6 fire about
// 841 times an hour, which is 14 a minute -- the respiratory rate our own breath
// detector finds independently, so they corroborate each other. And bit 4 is the
// trap: event-shaped in duration, no airflow drop. Judging by shape alone would
// have called it an apnea.

/** The apnea flag. See the table above for how it was identified. */
constexpr int kSefamApneaBit = 2;

/**
 * The shortest run that counts.
 *
 * The AASM criterion, and it is doing real work here rather than being
 * decorative: bit 2 also fires in sub-second bursts that are not events -- its
 * median run overall is under a second, and only the long ones show the airflow
 * collapse.
 */
constexpr double kSefamApneaMinSeconds = 10.0;

std::map<int, double> sefamDetBitShare(const std::vector<double>& codes) {
    std::map<int, double> share;
    if (codes.empty()) return share;

    std::vector<size_t> counts(8, 0);
    for (double d : codes) {
        const unsigned v = static_cast<unsigned>(d) & 0xFFu;
        for (int b = 0; b < 8; ++b)
            if (v & (1u << b)) ++counts[static_cast<size_t>(b)];
    }

    for (int b = 0; b < 8; ++b)
        if (counts[static_cast<size_t>(b)])
            share[b] = static_cast<double>(counts[static_cast<size_t>(b)]) /
                       static_cast<double>(codes.size());

    return share;
}

std::vector<SleepEvent> sefamApneasFromDet(
    const std::vector<double>& codes,
    int freq,
    std::chrono::system_clock::time_point start)
{
    std::vector<SleepEvent> events;
    if (freq <= 0 || codes.empty()) return events;

    const unsigned mask = 1u << kSefamApneaBit;
    const size_t min_samples =
        static_cast<size_t>(std::llround(kSefamApneaMinSeconds * freq));

    size_t run_start = 0;
    size_t run = 0;

    auto flush = [&](size_t end_index) {
        if (run < min_samples) return;

        SleepEvent ev;
        // APNEA, not OBSTRUCTIVE and not CENTRAL. That airflow stopped is
        // measured; telling obstructive from central needs effort belts, and the
        // device may well distinguish them on another bit we have not
        // identified. Under SDD-004 a bare APNEA counts toward the index without
        // claiming a mechanism.
        ev.event_type = EventType::APNEA;
        ev.timestamp = start + std::chrono::milliseconds(
            static_cast<long long>(std::llround(1000.0 * run_start / freq)));
        ev.duration_seconds = static_cast<double>(run) / freq;
        ev.details = "Sefam apnea (DET bit 2)";
        events.push_back(std::move(ev));
        (void)end_index;
    };

    for (size_t i = 0; i < codes.size(); ++i) {
        const unsigned v = static_cast<unsigned>(codes[i]) & 0xFFu;
        if (v & mask) {
            if (run == 0) run_start = i;
            ++run;
        } else if (run) {
            flush(i);
            run = 0;
        }
    }
    if (run) flush(codes.size());

    return events;
}

// WHAT THIS DELIBERATELY DOES NOT DO.
//
// It does not produce hypopneas, so what comes out is an APNEA INDEX and NOT an
// AHI. Bit 0 is the only hypopnea candidate and its flow ratio of 0.74 does not
// meet the <= 0.70 the definition requires, so calling it one would be a guess
// that lands directly in a clinical index.
//
// That matters downstream more than it looks. ParsedSession::calculateMetrics()
// puts these in `ahi`, because that field is the only one there is, and the web
// front end grades `ahi` against the AASM severity thresholds of 5, 15 and 30.
// A Sefam user therefore gets an AHI-shaped badge computed from half the
// measurement. Until a consumer can tell the two apart -- an index-kind flag
// alongside the number -- a Sefam index must not be presented as an AHI.

} // namespace cpapdash::parser

#endif // CPAPDASH_WITH_SEFAM
