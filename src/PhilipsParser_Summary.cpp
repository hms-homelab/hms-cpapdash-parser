#ifdef CPAPDASH_WITH_PHILIPS

// .001 / .B01 session summary.
// PROVENANCE: v2: docs/PHILIPS_FORMAT.md section 5 (layout from [B] cpap-lib
// ReadSummary/ReadSettings @f760b21; wiki leaves .001 undocumented), FamilyVersion
// >= 4 only, older schemas skip-with-warning (doc Q3). v3 FamilyVersion 6: doc
// sections 9.5-9.7 ([C] record meanings, each checked on [S]). No OSCAR source.

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

// ── v3 FamilyVersion 6 (doc 9.5-9.7) ────────────────────────────────────────

// Doc 9.7 humidifier status bytes: byte 1 bits 7-5 mode, bits 4-2 level;
// byte 2 bits 7-5 tube temperature.
void applyHumidifierV3(uint8_t b1, uint8_t b2, DeviceSettings& s) {
    const int mode  = (b1 >> 5) & 0x07;
    const int level = (b1 >> 2) & 0x07;
    switch (mode) {
        case 0: s.humidifier_mode = 1; s.humidifier_level = level; break;  // fixed
        case 1: s.humidifier_mode = 2; s.humidifier_level = level; break;  // adaptive
        case 2:                                                             // heated tube
            s.humidifier_mode = 3;
            s.humidifier_level = level;
            s.tube_temp = (b2 >> 5) & 0x07;
            break;
        case 3:                                     // passover: no heat
        case 4: s.humidifier_mode = 0; break;       // no humidifier
        default: break;                             // 5 = error: leave unset
    }
}

// Doc 9.7: nested (code, length, value) triples. Unknown codes are skipped by
// their own length byte.
void applySettingsV3(const uint8_t* p, size_t len, ParsedSession& session) {
    DeviceSettings s = session.settings.value_or(DeviceSettings{});
    std::optional<int> relief, relief_level, prs1_mode;
    size_t q = 0;
    while (q + 2 <= len) {
        const uint8_t code = p[q];
        const size_t n = p[q + 1];
        const uint8_t* v = p + q + 2;
        if (q + 2 + n > len) break;
        switch (code) {
            case 0x00: if (n >= 1) prs1_mode = v[0]; break;
            case 0x0a: if (n >= 1) s.set_pressure = v[0] * kPressureTick; break;
            case 0x0d: if (n >= 2) { s.min_pressure = v[0] * kPressureTick;
                                     s.max_pressure = v[1] * kPressureTick; } break;
            case 0x0e: if (n >= 2) { s.epap = v[0] * kPressureTick;
                                     s.ipap = v[1] * kPressureTick; } break;
            case 0x2c: if (n >= 1) s.ramp_time = v[0]; break;
            case 0x2d: if (n >= 1) s.ramp_pressure = v[0] * kPressureTick; break;
            case 0x2e: if (n >= 1) relief = v[0]; break;
            case 0x30: if (n >= 1) relief_level = v[0]; break;
            case 0x35: if (n >= 2) applyHumidifierV3(v[0], v[1], s); break;
            case 0x3b:
            case 0x40:
            case 0x46:  // tubing: 0 22 mm, 1/2 15 mm (2 heated), 3/4 12 mm (4 heated)
                if (n >= 1) {
                    const int t = v[0];
                    s.hose_diameter = t == 0 ? 22 : (t <= 2 ? 15 : 12);
                }
                break;
            default: break;
        }
        q += 2 + n;
    }

    // Mode (doc 9.7): 0 CPAP, 1 bi-level, 2 auto CPAP, 3 auto bi-level, 4 CPAP-Check
    if (prs1_mode) {
        switch (*prs1_mode) {
            case 0: case 4: s.therapy_mode = 0; break;
            case 2:         s.therapy_mode = 1; break;
            case 1: case 3: s.therapy_mode = 2; break;
            default: break;
        }
    }
    // Pressure relief (doc 9.7): 0 off, 0x80 C-Flex / Bi-Flex, 0x90 C-Flex+ / A-Flex
    // by mode, 0xa0 rise time, 0xb0 P-Flex. flex_mode: 0 Off, 1 Flex, 2 AFlex,
    // 3 Rise, 4 BiFlex (Models.h).
    if (relief) {
        switch (*relief) {
            case 0x00: s.flex_mode = 0; break;
            case 0x80: s.flex_mode = (s.therapy_mode == 2) ? 4 : 1; break;
            case 0x90: s.flex_mode = (s.therapy_mode == 1) ? 2 : 1; break;
            case 0xa0: s.flex_mode = 3; break;
            case 0xb0: s.flex_mode = 1; break;
            default: break;
        }
        if (relief_level && *relief != 0x00) s.flex_level = *relief_level;
    }
    session.settings = s;
}

// Doc 9.6 statistics words that are confirmed on [S], summed across the
// session's records (one per mask-on period).
void addStatistics(const uint8_t* p, size_t len, ParsedSession& session) {
    if (len < 24) return;
    auto& m = session.machine_reported;
    if (!m) m = MachineReportedCounts{};
    auto word = [&](int i) { return static_cast<int>(readU16(p + 2 * i)); };
    m->obstructive_apneas        += word(1);
    m->clear_airway_apneas       += word(3);
    m->variable_breathing_minutes += word(4);
    m->large_leak_minutes        += word(5);
    m->periodic_breathing_minutes += word(6);
    m->hypopneas                 += word(8);
    m->hypopneas_variant         += word(9);
    m->reras                     += word(10);
    m->flow_limitations          += word(11);
}

} // namespace

bool PhilipsParser::parseSummaryV3(
    const PhilipsChunk& chunk,
    ParsedSession& session,
    std::vector<std::pair<std::chrono::system_clock::time_point,
                          std::chrono::system_clock::time_point>>& mask_on_spans,
    PhilipsParseWarnings& warnings)
{
    if (chunk.family != 0 || chunk.family_version != 6) {
        warnings.unsupported_chunks++;
        return false;
    }

    auto clock = chunk.timestamp;
    std::optional<std::chrono::system_clock::time_point> mask_on;
    bool any_span = false;
    const uint8_t* d = chunk.data.data();
    const size_t len = chunk.data.size();
    size_t pos = 0;

    while (pos < len) {
        const uint8_t code = d[pos];
        const int size = chunk.record_size[code];
        if (size < 0) {
            // Not in this chunk's header block: its size is unknown, so the rest
            // of the chunk cannot be framed.
            warnings.unknown_record_codes++;
            break;
        }
        if (pos + 1 + static_cast<size_t>(size) > len) { warnings.truncated_chunks++; break; }
        const uint8_t* r = d + pos + 1;

        switch (code) {
            case 0x00:  // equipment on
            case 0x05:  // pressure summary (not used yet)
            case 0x0a:  // seen on [S], meaning unknown (doc Q12)
            case 0x0c:
            case 0x0e:
                break;
            case 0x01:  // settings
                applySettingsV3(r, static_cast<size_t>(size), session);
                break;
            case 0x02:  // equipment off: u16 seconds since the previous time record
                if (size >= 2) clock += std::chrono::seconds(readU16(r));
                break;
            case 0x03:  // mask on
                if (size >= 2) {
                    clock += std::chrono::seconds(readU16(r));
                    mask_on = clock;
                }
                break;
            case 0x04:  // mask off
                if (size >= 2) {
                    clock += std::chrono::seconds(readU16(r));
                    if (mask_on) {
                        mask_on_spans.emplace_back(*mask_on, clock);
                        mask_on.reset();
                        any_span = true;
                    }
                }
                break;
            case 0x08:  // statistics, one per mask-on period
                addStatistics(r, static_cast<size_t>(size), session);
                break;
            default:    // in the header block but not in doc 9.5: skip by size
                warnings.unknown_record_codes++;
                break;
        }
        pos += 1 + static_cast<size_t>(size);
    }
    return any_span;
}

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
    // Every mask-on period of the session, from every chunk (v2 and v3).
    std::vector<std::pair<std::chrono::system_clock::time_point,
                          std::chrono::system_clock::time_point>> spans;

    for (const auto& chunk : chunks) {
        if (chunk.data_format_version == 3) {
            parseSummaryV3(chunk, session, spans, warnings);
            continue;
        }
        if (chunk.family != 0) { warnings.unsupported_chunks++; continue; }
        if (chunk.family_version < 4) {
            // Sub-V4 .001 layout unconfirmed (doc Q3): skip, never guess.
            warnings.unsupported_chunks++;
            continue;
        }

        auto clock = chunk.timestamp;
        std::optional<std::chrono::system_clock::time_point> mask_on;
        std::vector<std::pair<std::chrono::system_clock::time_point,
                              std::chrono::system_clock::time_point>> chunk_spans;

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
                    chunk_spans.emplace_back(*mask_on, clock);
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

        // A malformed chunk contributes no spans (all or nothing per chunk).
        spans.insert(spans.end(), chunk_spans.begin(), chunk_spans.end());
    }

    if (spans.empty()) return false;

    // Sub-sessions merge (doc 5): start = first mask-on, end = last mask-off.
    // Duration is the SUM of the mask-on periods, never the envelope, so a
    // mask-off break is not counted as therapy (SDD-009 D12; the ResMed
    // ticket-87 lesson). It is the AHI and usage denominator.
    long long therapy_seconds = 0;
    for (const auto& [on, off] : spans) {
        if (!session.session_start.has_value() || on < session.session_start.value())
            session.session_start = on;
        if (!session.session_end.has_value() || off > session.session_end.value())
            session.session_end = off;
        if (off > on)
            therapy_seconds += std::chrono::duration_cast<std::chrono::seconds>(off - on).count();
    }
    session.duration_seconds = static_cast<int>(therapy_seconds);
    session.mask_on_periods = spans;
    return true;
}

} // namespace cpapdash::parser

#endif // CPAPDASH_WITH_PHILIPS
