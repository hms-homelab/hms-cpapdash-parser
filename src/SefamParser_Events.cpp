#ifdef CPAPDASH_WITH_SEFAM

#include "cpapdash/parser/SefamParser.h"

#include <map>
#include <vector>

namespace cpapdash::parser {

// Why this file produces no events.
//
// The first cut of this parser read DET as an enumeration -- one code per
// sample, a run of equal codes being one event -- and emitted each run as
// EventType::OTHER. The donor card says that is the wrong shape entirely. Over a
// single 6h24m night DET takes 85 distinct values, and they decompose into
// eight independent bits:
//
//     bit 0 (0x01)  14.60% of the night      bit 4 (0x10)   4.11%
//     bit 1 (0x02)  66.48%                   bit 5 (0x20)  37.34%
//     bit 2 (0x04)   2.97%                   bit 6 (0x40)  13.39%
//     bit 3 (0x08)   0.01%                   bit 7 (0x80)  25.13%
//
// It is a bitfield of concurrent flags. A bit that is set for two thirds of the
// night is not an apnea; it is far more likely a breath phase or a device state,
// while the bit set for 0.01% of it might well be an event. But "might well be"
// is not something to put in a person's therapy record.
//
// Reading runs of the whole byte would have produced tens of thousands of
// "events" per night out of flags toggling against each other. Reading runs of
// each bit would produce the same for the prevalent bits. Either way the count
// would be an artefact of the encoding, not a finding about the patient.
//
// So v1 emits nothing, and reports each bit's share of the recording in
// SefamSessionNotes::det_bit_share instead. That is the starting point for
// decoding the bits against a SEFAM Analyze report, which is the only thing that
// can settle what they mean. Until then a Sefam session carries no events and no
// AHI, which is the honest reading rather than a convenient one.

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

} // namespace cpapdash::parser

#endif // CPAPDASH_WITH_SEFAM
