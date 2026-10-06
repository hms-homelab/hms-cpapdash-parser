#pragma once

// The AirSense 11's own daily summary, read over its Bluetooth.
//
// An AirSense 11 (and AirCurve 11) answers a "Summary" spool over its own
// Bluetooth radio: a protobuf stream with one record per therapy day, noon to
// noon, carrying the same figures the machine writes to STR.edf on its card.
// Every field was matched against STR.edf over 70+ nights by the protocol's
// documenters (github.com/maroliar/hass-resmed-airsense, MIT, docs/PROTOCOL.md
// §7); the values are the STR.edf signal in hundredths. No schema is published;
// the field numbers below are the decoded ones.
//
//   top level    field 2 (length-delimited), repeated: one day
//   day          2 / 3   day start / end, ms since the epoch
//                5       Duration, minutes (not scaled)
//                6       sessions: repeated message {1: start ms, 2: minutes}
//                7..13   AHI, AI, HI, OAI, CAI, UAI, RIN            x 0.01
//                14      Leak {2: 50, 3: 70, 4: 95, 5: Max}, L/s    x 0.01
//                15/20/21 TgtIPAP / TgtEPAP / MaskPress {2: 50, 3: 95, 4: Max}, cmH2O
//                22/23/25 TidVol (L) / MinVent (L/min) / RespRate {2, 3, 4}
//                16      CSR, minutes                               x 0.01
//                29..33  AmbHumidity, HumTemp, HTubeTemp, HumPow, HTubePow {2: 50}
//                36 / 37 BlowPress / Flow {1: 5, 3: 95}
//                38      BlowFlow {2: 50}
//                39      MaskEvents (not scaled)
//                40      updated, ms
//
// The bytes carry every date, so the stored file's name is informational: a
// consumer stores the spool as it arrived and reads it back by content.
//
// A summary is what a night is when the card cannot be read: the machine's own
// indices and sessions, no waveforms and no event list. toStrRecord() turns a day into the STRDailyRecord EDFParser::parseSTR*
// returns, in the same units, so a consumer stores a Bluetooth night with the
// code that stores an STR day.

#include "cpapdash/parser/Models.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace cpapdash::parser {

// A percentile set as the summary carries it. Which members are set depends on
// the signal: leak has 50/70/95/max, pressures 50/95/max, blower 5/95.
struct AirSense11Stats {
    std::optional<double> p5, p50, p70, p95, max;
};

struct AirSense11Session {
    std::chrono::system_clock::time_point start;
    int minutes = 0;
};

struct AirSense11Day {
    std::chrono::system_clock::time_point day_start;   // the machine's noon, as an instant
    std::chrono::system_clock::time_point day_end;
    int usage_minutes = 0;                              // [Duration]
    std::vector<AirSense11Session> sessions;            // [MaskOn/MaskOff]
    int mask_events = 0;                                // [MaskEvents]

    // Indices, events per hour; set only on a day with usage, as the machine
    // reports them. [AHI AI HI OAI CAI UAI RIN]
    std::optional<double> ahi, ai, hi, oai, cai, uai, rin;
    std::optional<double> csr_minutes;                  // [CSR]

    AirSense11Stats leak_lps;          // [Leak.50/70/95/Max] L/s
    AirSense11Stats target_ipap;       // [TgtIPAP.50/95/Max] cmH2O
    AirSense11Stats target_epap;       // [TgtEPAP.50/95/Max] cmH2O
    AirSense11Stats mask_pressure;     // [MaskPress.50/95/Max] cmH2O
    AirSense11Stats tidal_volume_l;    // [TidVol.50/95/Max] L
    AirSense11Stats minute_vent;       // [MinVent.50/95/Max] L/min
    AirSense11Stats resp_rate;         // [RespRate.50/95/Max] breaths/min
    AirSense11Stats blower_pressure;   // [BlowPress.5/95] cmH2O
    AirSense11Stats flow_lps;          // [Flow.5/95] L/s
    std::optional<double> blower_flow_lps;    // [BlowFlow.50]
    std::optional<double> ambient_humidity;   // [AmbHumidity.50]
    std::optional<double> humidifier_temp;    // [HumTemp.50]
    std::optional<double> heated_tube_temp;   // [HTubeTemp.50]
    std::optional<double> humidifier_power;   // [HumPow.50]
    std::optional<double> heated_tube_power;  // [HTubePow.50]
    std::optional<std::chrono::system_clock::time_point> updated;

    bool used() const { return usage_minutes > 0; }
};

class AirSense11SummaryParser {
public:
    // The bytes are a Summary spool: a protobuf stream whose first field is a
    // day record carrying a day start. Decided from the content, never a name.
    static bool looksLike(const uint8_t* data, size_t len);

    // Every day record in the spool, in the order stored (oldest first). A
    // stream cut short yields the days that are whole; nothing throws.
    static std::vector<AirSense11Day> parse(const uint8_t* data, size_t len);

    // One day as the STR reader would return it: record_date is the day's
    // start instant, leak in L/min, pressures in cmH2O, indices per hour, the
    // sessions as mask on/off pairs. Settings the spool does not carry (mode,
    // the pressure settings, the family) are left at their defaults.
    static STRDailyRecord toStrRecord(const AirSense11Day& day, const std::string& device_id);

    // The days WITH usage, as STR records, in order. A day without usage is
    // skipped exactly as EDFParser::parseSTR* skips an STR record without
    // therapy: a night off is not a night of zeros.
    static std::vector<STRDailyRecord> toStrRecords(const std::vector<AirSense11Day>& days,
                                                    const std::string& device_id);

    // The stored spool's extension, so a consumer can tell one apart on disk.
    static constexpr const char* kExtension = ".as11";
};

} // namespace cpapdash::parser
