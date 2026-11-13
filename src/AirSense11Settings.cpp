#include "cpapdash/parser/AirSense11Settings.h"

#include "ProtoWire.h"

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace cpapdash::parser {

namespace {

using Clock = std::chrono::system_clock;
using protowire::Field;
using protowire::Reader;

// How a field's number becomes the STR signal's value.
enum class Unit {
    Hundredths,   // pressures, rates, EPR level, ramp minutes, temperature: x 100
    Millis,       // inspiratory times: ms, the STR keeps seconds
    AsIs,         // switches, sensitivities, levels, and the rise time (ms in both)
};

struct Slot {
    uint32_t    field;
    const char* label;
    Unit        unit;
};

// Each therapy profile by its mode code (its field 1), with its later fields
// under the STR labels they were matched to. The order of the fields is the
// order of the STR's labels for that profile.
const std::map<uint32_t, std::vector<Slot>>& profileSlots() {
    static const std::map<uint32_t, std::vector<Slot>> m = {
        {1, {   // CPAP
            {2, "S.C.Press", Unit::Hundredths},
            {3, "S.C.StartPress", Unit::Hundredths},
        }},
        {2, {   // AutoSet
            {2, "S.A.StartPress", Unit::Hundredths},
            {3, "S.A.MinPress", Unit::Hundredths},
            {4, "S.A.MaxPress", Unit::Hundredths},
        }},
        {12, {  // AutoSet for Her
            {2, "S.AFH.StartPress", Unit::Hundredths},
            {3, "S.AFH.MinPress", Unit::Hundredths},
            {4, "S.AFH.MaxPress", Unit::Hundredths},
        }},
        {4, {   // Spont
            {2, "S.S.StartPress", Unit::Hundredths},
            {3, "S.S.IPAP", Unit::Hundredths},
            {4, "S.S.EPAP", Unit::Hundredths},
            {5, "S.S.EasyBreathe", Unit::AsIs},
            {6, "S.S.RespRateEn", Unit::AsIs},
            {8, "S.S.TiMax", Unit::Millis},
            {9, "S.S.TiMin", Unit::Millis},
            {10, "S.S.RiseEnable", Unit::AsIs},
            {11, "S.S.RiseTime", Unit::AsIs},
            {12, "S.S.Trigger", Unit::AsIs},
            {13, "S.S.Cycle", Unit::AsIs},
        }},
        {5, {   // S/T
            {2, "S.ST.StartPress", Unit::Hundredths},
            {3, "S.ST.IPAP", Unit::Hundredths},
            {4, "S.ST.EPAP", Unit::Hundredths},
            {6, "S.ST.RespRate", Unit::Hundredths},
            {7, "S.ST.TiMax", Unit::Millis},
            {8, "S.ST.TiMin", Unit::Millis},
            {9, "S.ST.RiseEnable", Unit::AsIs},
            {10, "S.ST.RiseTime", Unit::AsIs},
            {11, "S.ST.Trigger", Unit::AsIs},
            {12, "S.ST.Cycle", Unit::AsIs},
        }},
        {6, {   // Timed
            {2, "S.T.StartPress", Unit::Hundredths},
            {3, "S.T.IPAP", Unit::Hundredths},
            {4, "S.T.EPAP", Unit::Hundredths},
            {6, "S.T.RespRate", Unit::Hundredths},
            {7, "S.T.Ti", Unit::Millis},
            {8, "S.T.RiseEnable", Unit::AsIs},
            {9, "S.T.RiseTime", Unit::AsIs},
        }},
        {7, {   // VAuto
            {2, "S.VA.StartPress", Unit::Hundredths},
            {3, "S.VA.MaxIPAP", Unit::Hundredths},
            {4, "S.VA.MinEPAP", Unit::Hundredths},
            {5, "S.VA.PS", Unit::Hundredths},
            {6, "S.VA.TiMax", Unit::Millis},
            {7, "S.VA.TiMin", Unit::Millis},
            {8, "S.VA.Trigger", Unit::AsIs},
            {9, "S.VA.Cycle", Unit::AsIs},
        }},
        {8, {   // ASV
            {2, "S.AV.StartPress", Unit::Hundredths},
            {3, "S.AV.EPAP", Unit::Hundredths},
            {4, "S.AV.MaxPS", Unit::Hundredths},
            {5, "S.AV.MinPS", Unit::Hundredths},
        }},
        {9, {   // ASVAuto
            {2, "S.AA.StartPress", Unit::Hundredths},
            {3, "S.AA.MaxEPAP", Unit::Hundredths},
            {4, "S.AA.MinEPAP", Unit::Hundredths},
            {5, "S.AA.MaxPS", Unit::Hundredths},
            {6, "S.AA.MinPS", Unit::Hundredths},
        }},
    };
    return m;
}

// The features (record field 4), each by its field number there.
const std::map<uint32_t, std::vector<Slot>>& featureSlots() {
    static const std::map<uint32_t, std::vector<Slot>> m = {
        {1, {{1, "S.AS.Comfort", Unit::AsIs}}},
        {2, {
            {1, "S.EPR.ClinEnable", Unit::AsIs},
            {2, "S.EPR.EPREnable", Unit::AsIs},
            {3, "S.EPR.EPRType", Unit::AsIs},
            {4, "S.EPR.Level", Unit::Hundredths},
        }},
        {3, {
            {1, "S.RampEnable", Unit::AsIs},
            {2, "S.RampTime", Unit::Hundredths},
        }},
        {4, {{1, "S.SmartStart", Unit::AsIs}}},
        {5, {
            {1, "S.Mask", Unit::AsIs},
            {2, "S.Tube", Unit::AsIs},
            {3, "S.ABFilter", Unit::AsIs},
        }},
        {6, {
            {1, "S.ClimateControl", Unit::AsIs},
            {2, "S.HumEnable", Unit::AsIs},
            {3, "S.HumLevel", Unit::AsIs},
            {4, "S.TempEnable", Unit::AsIs},
            {5, "S.Temp", Unit::Hundredths},
        }},
    };
    return m;
}

// The STR's Mode code of a profile's mode, for the modes whose code was seen
// on a card. 0: not known, and Mode is left out.
int strModeOf(uint32_t mode) {
    switch (mode) {
    case 2: return 1;   // AutoSet
    case 7: return 8;   // VAuto
    default: return 0;
    }
}

double valueOf(uint64_t raw, Unit unit) {
    switch (unit) {
    case Unit::Hundredths: return static_cast<double>(raw) * 0.01;
    case Unit::Millis:     return static_cast<double>(raw) * 0.001;
    case Unit::AsIs:       break;
    }
    return static_cast<double>(raw);
}

// A message's varint fields, by number (the last one wins, as protobuf reads it).
std::map<uint32_t, uint64_t> varints(const uint8_t* b, size_t n) {
    std::map<uint32_t, uint64_t> out;
    Reader r(b, n);
    Field f;
    while (r.next(f))
        if (f.wire == 0) out[f.number] = f.varint;
    return out;
}

void put(STRDayValues& to, const std::vector<Slot>& slots, const std::map<uint32_t, uint64_t>& from) {
    for (const auto& s : slots) {
        const auto it = from.find(s.field);
        if (it != from.end()) to.values.emplace_back(s.label, valueOf(it->second, s.unit));
    }
}

// One record. False when it carries no applied time: not a settings record.
bool readRecord(const uint8_t* b, size_t n, AirSense11SettingsRecord& out) {
    bool have_time = false;
    std::vector<std::pair<uint32_t, std::map<uint32_t, uint64_t>>> profiles;   // slot, fields
    std::vector<std::pair<uint32_t, std::map<uint32_t, uint64_t>>> features;   // number, fields

    Reader r(b, n);
    Field f;
    while (r.next(f)) {
        if (f.wire != 2) continue;
        if (f.number == 1) {
            const auto a = varints(f.bytes, f.len);
            if (const auto it = a.find(1); it != a.end() && it->second > 0) {
                out.applied = Clock::time_point(std::chrono::duration_cast<Clock::duration>(
                    std::chrono::milliseconds(static_cast<int64_t>(it->second))));
                have_time = true;
            }
        } else if (f.number == 2) {
            const auto a = varints(f.bytes, f.len);
            if (const auto it = a.find(1); it != a.end()) out.active_profile = static_cast<int>(it->second);
        } else if (f.number == 3 || f.number == 4) {
            Reader inner(f.bytes, f.len);
            Field g;
            while (inner.next(g)) {
                if (g.wire != 2) continue;
                (f.number == 3 ? profiles : features).emplace_back(g.number, varints(g.bytes, g.len));
            }
        }
    }
    if (!have_time) return false;

    for (const auto& [slot, fields] : profiles) {
        const auto mode = fields.find(1);
        if (mode == fields.end()) continue;
        // The active profile is named by its slot in field 3 (AutoSet in
        // slot 1, active 1, on a card that says AutoSet).
        if (out.active_profile > 0 && slot == static_cast<uint32_t>(out.active_profile))
            out.active_mode = static_cast<int>(mode->second);
        const auto known = profileSlots().find(static_cast<uint32_t>(mode->second));
        if (known != profileSlots().end()) put(out.values, known->second, fields);
    }
    for (const auto& [number, fields] : features) {
        const auto known = featureSlots().find(number);
        if (known != featureSlots().end()) put(out.values, known->second, fields);
    }
    if (const int code = strModeOf(static_cast<uint32_t>(out.active_mode)); code > 0)
        out.values.values.emplace(out.values.values.begin(), "Mode", static_cast<double>(code));
    return true;
}

}  // namespace

bool AirSense11SettingsParser::looksLike(const uint8_t* data, size_t len) {
    if (!data || len == 0) return false;
    Reader r(data, len);
    Field f;
    if (!r.next(f) || f.number != 3 || f.wire != 2) return false;
    AirSense11SettingsRecord rec;
    return readRecord(f.bytes, f.len, rec);
}

std::vector<AirSense11SettingsRecord> AirSense11SettingsParser::parse(const uint8_t* data, size_t len) {
    std::vector<AirSense11SettingsRecord> out;
    if (!data) return out;
    Reader r(data, len);
    Field f;
    while (r.next(f)) {
        if (f.number != 3 || f.wire != 2) continue;
        AirSense11SettingsRecord rec;
        if (readRecord(f.bytes, f.len, rec)) out.push_back(std::move(rec));
    }
    return out;
}

const AirSense11SettingsRecord* AirSense11SettingsParser::inForce(
    const std::vector<AirSense11SettingsRecord>& records, Clock::time_point day_end) {
    const AirSense11SettingsRecord* best = nullptr;
    for (const auto& r : records)
        if (r.applied < day_end && (!best || r.applied >= best->applied)) best = &r;
    return best;
}

} // namespace cpapdash::parser
