#pragma once

// The AirSense 11's stored pages, read over its own Bluetooth (SDD-012).
//
// Besides the live stream, an AirSense 11 keeps the night's signals and
// events in "spools" it answers page by page (StartSpool / PullSpoolFragments).
// A night the stream missed is still on those pages. This reads them.
//
// A signal page is protobuf. The top-level field number names the signal:
//
//   18 RespiratoryFlow6p25Hz      160 ms   0.02 L/s per count
//   19 MaskPressure6p25Hz         160 ms   0.2 cmH2O per count
//   20 Leak0p5Hz                 2000 ms   0.02 L/s per count
//   21 InspiratoryPressure0p5Hz  2000 ms   0.2 cmH2O per count
//
// and each record is {1: 1, 2: {1: interval ms, 2: start ms, 3: end ms,
// 4: RC03 blob}}. A record holds N = (end - start) / interval + 1 samples;
// sample i is at start + i * interval, so the last one sits at end.
//
// The RC03 blob: byte 0 is H, the number of header bytes after it; then
// "RC03", two bytes, two varints, a byte D and a byte E. The Rice parameter is
// k = log2(D) - 1. The payload opens with the first two samples as int16
// little-endian; every further sample is a Rice code read MSB first (q 1-bits,
// a 0, k bits), its zigzag residual added to a second-order prediction,
// x[n] = 2x[n-1] - x[n-2] + residual. The stream is padded with 1-bits.
//
// The events page is {4: {1: repeated {1: type, 2: onset ms, 3: ms, 4:
// duration ms}}}. Type 3 is a central apnea, 4 an obstructive apnea; no other
// type has been seen yet.
//
// Every rule above was matched against the same machine's card for one night
// (r = 0.999 on all four signals, 7 of 7 events); the details and what is not
// yet explained are in the SDD's FORMAT.md. Times are the machine's own clock.
// Mapping the samples into a night's EDF files is the consumer's (hms-cpap
// SDD-070); this only decodes.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace cpapdash::parser {

enum class AirSense11PageSignal {
    Unknown = 0,
    Flow = 18,                 // RespiratoryFlow6p25Hz
    MaskPressure = 19,         // MaskPressure6p25Hz
    Leak = 20,                 // Leak0p5Hz
    InspiratoryPressure = 21,  // InspiratoryPressure0p5Hz
};

// One record: a run of samples on a fixed grid.
struct AirSense11PageChunk {
    AirSense11PageSignal signal = AirSense11PageSignal::Unknown;
    int64_t start_ms    = 0;    // epoch ms, the first sample
    int64_t end_ms      = 0;    // epoch ms, the last sample
    int64_t interval_ms = 0;
    std::vector<int32_t> raw;   // counts; value() turns one into its unit

    int64_t timeOf(size_t i) const { return start_ms + static_cast<int64_t>(i) * interval_ms; }
    double  value(size_t i) const;
};

struct AirSense11SignalPage {
    std::vector<AirSense11PageChunk> chunks;   // in the order stored
    int skipped = 0;                           // records that could not be decoded
    std::string first_error;                   // why the first of them failed
};

struct AirSense11PageEvent {
    int     type        = 0;   // 3 central apnea, 4 obstructive apnea
    int64_t onset_ms    = 0;   // field 2
    int64_t field3_ms   = 0;   // field 3: equal to the onset in every event seen
    int64_t duration_ms = 0;   // field 4
};

class AirSense11PagesParser {
public:
    // A signal page: every record that decodes, in order. A record that does
    // not decode is counted in skipped and the rest are still read, since each
    // record carries its own length. A page cut inside a record yields the
    // whole records before it. Nothing throws.
    static AirSense11SignalPage parseSignalPage(const uint8_t* data, size_t len);

    // An events page: every event, in order. A page cut short yields the
    // events that are whole.
    static std::vector<AirSense11PageEvent> parseEventsPage(const uint8_t* data, size_t len);

    // One RC03 blob into n samples, in counts. Empty with err set when the
    // header is not RC03, the bitstream runs out before n samples, or anything
    // but 1-padding follows the last one.
    static std::optional<std::vector<int32_t>> decodeRc03(const uint8_t* blob, size_t len,
                                                          size_t n, std::string* err = nullptr);

    // The unit per count, and its name: 0.02 "L/s" or 0.2 "cmH2O"; 1 "" for
    // an unknown signal.
    static double      scale(AirSense11PageSignal s);
    static const char* unit(AirSense11PageSignal s);

    // "Central Apnea", "Obstructive Apnea", or "" for a type not yet seen.
    static const char* eventName(int type);
};

} // namespace cpapdash::parser
