#pragma once

// The AirSense 11's settings history, read over its Bluetooth.
//
// An AirSense 11 (and AirCurve 11) answers a "SettingProfilesCollection" spool
// over its own Bluetooth radio: a protobuf stream with one record per settings
// change, each the whole settings set as it stood after that change, stamped
// with the time it was applied. It is a change log, not a snapshot: asked from
// a date after the last change it answers nothing at all. Asked from
// 2000-01-01 it answers every change the machine still keeps.
//
// No schema is published; the field numbers below are the decoded ones. Each
// was matched on a real machine, record by record, against the settings
// columns of that machine's own STR.edf for the same days, and against its
// "Get SettingProfiles" answer (the JSON a card keeps as CurrentSettings.json).
//
//   top level   field 3, repeated: one settings record
//   record      1  attributes {1: applied, ms since the epoch}
//               2  active {1: the active therapy profile, 2 repeated: active feature ids}
//               3  therapy profiles, each a message whose field 1 is its mode:
//                    1 CPAP, 2 AutoSet, 4 Spont, 5 S/T, 6 Timed, 7 VAuto,
//                    8 ASV, 9 ASVAuto, 10 iVAPS, 11 PAC, 12 AutoSet for Her
//                  and whose later fields are its pressures (cmH2O x 100),
//                  times (ms) and switches, in the order of the STR labels
//               4  features: comfort, EPR, ramp, SmartStart, circuit, climate, ...
//
// A switch is written as the STR writes it: Off/No 1, On/Yes 2 (Auto 3 where it
// exists), and a sensitivity Medium 3. These codes are the STR's own.
//
// THE MODE. The therapy mode's code in these profiles is not the STR's Mode
// code (AutoSet is 2 here, 1 in the STR). Mode is written only for a profile
// whose STR code was seen on a card; any other leaves Mode out.
//
// WHICH RECORD A DAY TAKES. A card's STR day carries the settings as they
// stood when the day ended: a change made in the evening is already in that
// day's record. So the day ending at `day_end` takes the last record applied
// before it (inForce).

#include "cpapdash/parser/EDFWriter.h"   // STRDayValues

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace cpapdash::parser {

struct AirSense11SettingsRecord {
    std::chrono::system_clock::time_point applied;   // when the machine applied it
    int active_profile = 0;                           // record field 2.1, as sent
    int active_mode    = 0;                           // the active profile's mode code, 0 unknown
    // The record as the STR's settings columns: each value under its STR.edf
    // label, in that signal's unit. Only labels matched on a card are named;
    // Mode is named only when the active mode's STR code is known.
    STRDayValues values;
};

class AirSense11SettingsParser {
public:
    // The bytes are a settings history spool: a protobuf stream whose records
    // carry an applied time and therapy profiles. Decided from the content.
    static bool looksLike(const uint8_t* data, size_t len);

    // Every record in the spool, in the order stored (oldest first). A stream
    // cut short yields the records that are whole; nothing throws.
    static std::vector<AirSense11SettingsRecord> parse(const uint8_t* data, size_t len);

    // The record in force for the STR day ending at `day_end` (the next day's
    // noon on the machine's clock): the last one applied before it. nullptr
    // when every record is later, i.e. the history does not reach that day.
    static const AirSense11SettingsRecord* inForce(const std::vector<AirSense11SettingsRecord>& records,
                                                    std::chrono::system_clock::time_point day_end);

    // The stored spool's extension, so a consumer can tell one apart on disk.
    static constexpr const char* kExtension = ".as11set";
};

} // namespace cpapdash::parser
