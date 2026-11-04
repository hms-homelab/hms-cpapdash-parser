#ifdef CPAPDASH_WITH_PHILIPS

// .001 session summary: slot-coded stream + 24-byte settings block.
// PROVENANCE: docs/PHILIPS_FORMAT.md section 5 (layout from [B] cpap-lib
// ReadSummary/ReadSettings @f760b21; wiki leaves .001 undocumented).
// FamilyVersion >= 4 only; older schemas skip-with-warning (doc Q3).
// No OSCAR source.

#include "cpapdash/parser/PhilipsParser.h"
#include <iostream>

namespace cpapdash::parser {

namespace {

constexpr double kPressureTick = 0.1;  // cmH2O per unit (doc: confirmed both sources)

uint16_t readU16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

// Doc 5.1 rows 11-12: humidifier flag pair -> DeviceSettings fields.
void applyHumidifier(uint8_t flags1, uint8_t flags2, DeviceSettings& s) {
    int humidity_level      = flags1 & 0x07;
    int tube_humidity_level = (flags1 >> 4) & 0x07;
    int tube_temp           = (flags1 >> 7) | ((flags2 & 0x03) << 1);
    bool no_humidifier      = (flags2 & 0x10) != 0;
    bool adaptive           = (flags2 & 0x04) != 0;
    bool heated_tube        = (flags2 & 0x08) != 0 && !adaptive;

    if (no_humidifier) {
        s.humidifier_mode = 0;  // Off
        return;
    }
    s.humidifier_mode = adaptive ? 2 : (heated_tube ? 3 : 1);
    s.humidifier_level = heated_tube ? tube_humidity_level : humidity_level;
    if (heated_tube) s.tube_temp = tube_temp;
}

// Doc 5.1 row 10: flex flag byte -> flex_mode/flex_level.
// flex_mode: 0=Off, 1=Flex(CFlex/CFlex+), 2=AFlex, 3=Rise, 4=BiFlex (Models.h).
void applyFlex(uint8_t flags, std::optional<int> therapy_mode, DeviceSettings& s) {
    bool enabled   = (flags & 0x80) != 0;
    bool plainflex = (flags & 0x20) != 0;
    bool risetime  = (flags & 0x10) != 0;
    bool plusmode  = (flags & 0x08) != 0;
    int level      = flags & 0x03;

    if (!enabled) {
        s.flex_mode = 0;
        return;
    }
    if (risetime)        s.flex_mode = 3;
    else if (plainflex)  s.flex_mode = 1;
    else if (plusmode)   s.flex_mode = (therapy_mode == 1) ? 2 : 1;  // AFlex on APAP, CFlex+ on CPAP
    else                 s.flex_mode = (therapy_mode == 2) ? 4 : 1;  // BiFlex on bilevel, CFlex else
    s.flex_level = level;
}

// Doc 5.1: 24-byte settings block (code 0x00 body).
bool applySettings(const uint8_t* p, size_t len, ParsedSession& session) {
    constexpr size_t kSettingsLen = 24;
    if (len < kSettingsLen) return false;

    DeviceSettings s;

    auto mode = PhilipsParser::mapTherapyMode(p[1]);
    s.therapy_mode = mode;

    double min_pressure = p[2] * kPressureTick;
    double max_pressure = p[3] * kPressureTick;
    s.min_pressure = min_pressure;
    s.max_pressure = max_pressure;
    // CPAP single pressure: min if non-zero else max (doc 5.1)
    if (mode == 0) s.set_pressure = (min_pressure > 0) ? min_pressure : max_pressure;
    // p[4], p[5]: min/max pressure support; no unified-model slot in v1.
    // p[6]: startup mode, unused.
    s.ramp_time = p[7];
    s.ramp_pressure = p[8] * kPressureTick;
    applyFlex(p[9], mode, s);
    applyHumidifier(p[10], p[11], s);
    s.hose_diameter = (p[12] & 0x01) ? 15 : 22;
    // p[13]-p[16]: resistance/general flags and auto-trial, unused in v1.
    // p[17]-p[23]: reserved.

    session.settings = s;
    return true;
}

} // namespace

std::optional<int> PhilipsParser::mapTherapyMode(uint8_t prs1_mode) {
    // Doc 5.1: operating-mode byte -> therapy_mode (0=CPAP, 1=APAP, 2=BiPAP)
    switch (prs1_mode) {
        case 0x00: return 0;  // CPAP
        case 0xA0: return 0;  // CPAP (check mode)
        case 0x40: return 1;  // APAP
        case 0x80: return 1;  // APAP
        case 0x20: return 2;  // Bilevel fixed
        case 0x60: return 2;  // Bilevel auto, variable PS
        default:   return std::nullopt;
    }
}

bool PhilipsParser::parseSummary(const std::vector<PhilipsChunk>& chunks,
                                 ParsedSession& session,
                                 PhilipsParseWarnings& warnings) {
    bool parsed_any = false;

    for (const auto& chunk : chunks) {
        if (chunk.family != 0) { warnings.unsupported_chunks++; continue; }
        if (chunk.family_version < 4) {
            // Sub-V4 .001 layout unconfirmed (doc Q3): skip, never guess.
            warnings.unsupported_chunks++;
            continue;
        }

        auto clock = chunk.timestamp;
        std::optional<std::chrono::system_clock::time_point> mask_on;
        std::optional<std::chrono::system_clock::time_point> first_start, last_end;

        const uint8_t* d = chunk.data.data();
        size_t len = chunk.data.size();
        size_t pos = 0;
        bool malformed = false;

        // Slot-coded stream (doc section 5). Body sizes include everything
        // after the 1-byte code.
        while (pos < len && !malformed) {
            uint8_t code = d[pos++];
            switch (code) {
                case 0x00:  // Equipment on + settings block
                    if (pos + 24 > len) { malformed = true; break; }
                    applySettings(d + pos, len - pos, session);
                    pos += 24;
                    break;
                case 0x01:  // Equipment off: delta + 5
                    if (pos + 7 > len) { malformed = true; break; }
                    clock += std::chrono::seconds(readU16(d + pos));
                    pos += 7;
                    break;
                case 0x02: {  // Mask on: delta + 3 + humidifier 2
                    if (pos + 7 > len) { malformed = true; break; }
                    clock += std::chrono::seconds(readU16(d + pos));
                    if (mask_on.has_value()) { malformed = true; break; }  // mismatched
                    mask_on = clock;
                    pos += 7;
                    break;
                }
                case 0x03: {  // Mask off: delta + 34
                    if (pos + 36 > len) { malformed = true; break; }
                    clock += std::chrono::seconds(readU16(d + pos));
                    if (!mask_on.has_value()) { malformed = true; break; }  // mismatched
                    if (!first_start.has_value()) first_start = mask_on;
                    last_end = clock;
                    mask_on.reset();
                    pos += 36;
                    break;
                }
                case 0x04:  // Time correction: delta only
                    if (pos + 2 > len) { malformed = true; break; }
                    clock += std::chrono::seconds(readU16(d + pos));
                    pos += 2;
                    break;
                case 0x05:
                case 0x06:  // No body observed
                    break;
                case 0x07:  // Humidifier change: delta + humidifier 2
                    if (pos + 4 > len) { malformed = true; break; }
                    clock += std::chrono::seconds(readU16(d + pos));
                    if (session.settings.has_value()) {
                        applyHumidifier(d[pos + 2], d[pos + 3], session.settings.value());
                    }
                    pos += 4;
                    break;
                case 0x08:  // CPAP-check related: delta + 9
                    if (pos + 11 > len) { malformed = true; break; }
                    clock += std::chrono::seconds(readU16(d + pos));
                    pos += 11;
                    break;
                default:
                    // Unknown slot code: lengths unknown, abandon this chunk.
                    malformed = true;
                    break;
            }
        }

        if (malformed) {
            warnings.unsupported_chunks++;
            std::cerr << "PhilipsParser: malformed/unknown .001 record in session "
                      << chunk.session_number << ", chunk skipped" << std::endl;
            continue;
        }

        // Sub-sessions merge: start = first mask-on, end = last mask-off (doc 5)
        if (first_start.has_value() && last_end.has_value()) {
            if (!session.session_start.has_value() ||
                first_start.value() < session.session_start.value()) {
                session.session_start = first_start;
            }
            if (!session.session_end.has_value() ||
                last_end.value() > session.session_end.value()) {
                session.session_end = last_end;
            }
            parsed_any = true;
        }
    }

    if (parsed_any && session.session_start && session.session_end) {
        session.duration_seconds = static_cast<int>(
            std::chrono::duration_cast<std::chrono::seconds>(
                session.session_end.value() - session.session_start.value()).count());
    }

    return parsed_any;
}

} // namespace cpapdash::parser

#endif // CPAPDASH_WITH_PHILIPS
