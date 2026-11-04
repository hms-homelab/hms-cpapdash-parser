#ifdef CPAPDASH_WITH_PHILIPS

// .002 event data: Family 0, FamilyVersion >= 4 code table.
// PROVENANCE: docs/PHILIPS_FORMAT.md section 4.2 (from [B] cpap-lib
// ReadEvents @f760b21); table 4.1 (System One, sub-V4) is NOT implemented
// pending real-card confirmation (doc Q2). No OSCAR source.
//
// PRS1 stamps apnea-family events at their END with a back-distance byte;
// the unified model wants ONSET (SDD-001 F1 invariant). The conversion
// happens here, at record-decode time, and nowhere else.

#include "cpapdash/parser/PhilipsParser.h"
#include <iostream>
#include <set>

namespace cpapdash::parser {

namespace {

constexpr double kPressureTick = 0.1;  // cmH2O per unit

uint16_t readU16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

} // namespace

std::optional<EventType> PhilipsParser::mapFamily0EventCode(uint8_t code) {
    // Doc 4.2 (F0 V4). Codes that carry stats/settings rather than events
    // return nullopt here and are handled inline in parseEvents.
    switch (code) {
        case 0x05: return EventType::RERA;
        case 0x06: return EventType::OBSTRUCTIVE;
        case 0x07: return EventType::CLEAR_AIRWAY;
        case 0x0A: return EventType::HYPOPNEA;
        case 0x0B: return EventType::HYPOPNEA;
        case 0x0C: return EventType::FLOW_LIMITATION;
        case 0x0D: return EventType::VIBRATORY_SNORE;
        case 0x0E: return EventType::PERIODIC_BREATHING;  // variable breathing
        case 0x0F: return EventType::PERIODIC_BREATHING;
        case 0x10: return EventType::LARGE_LEAK;
        default:   return std::nullopt;
    }
}

bool PhilipsParser::parseEvents(const std::vector<PhilipsChunk>& chunks,
                                ParsedSession& session,
                                std::vector<EventStat>& stats,
                                PhilipsParseWarnings& warnings) {
    bool parsed_any = false;
    std::set<uint8_t> logged_unknown;

    for (const auto& chunk : chunks) {
        if (chunk.family != 0 || chunk.family_version < 4) {
            // Sub-V4 event schema differs (doc 4.1 vs 4.2, Q2): never guess.
            warnings.unsupported_chunks++;
            continue;
        }
        if (chunk.file_type != 0) {
            // Fixed-interval event storage (1160P style) is out of v1 scope.
            warnings.unsupported_chunks++;
            continue;
        }

        auto clock = chunk.timestamp;
        const uint8_t* d = chunk.data.data();
        size_t len = chunk.data.size();
        size_t pos = 0;

        // Record stream: code, u16 delta (advances clock), code-specific fields
        while (pos < len) {
            if (pos + 3 > len) { warnings.truncated_chunks++; break; }
            uint8_t code = d[pos];
            clock += std::chrono::seconds(readU16(d + pos + 1));
            pos += 3;

            auto endStamped = [&](double duration_secs, uint8_t elapsed) {
                // onset = clock - elapsed - duration (doc 4.2)
                auto onset = clock - std::chrono::seconds(elapsed) -
                             std::chrono::seconds(static_cast<int>(duration_secs));
                return onset;
            };

            switch (code) {
                case 0x01: {  // Pressure set (stat only, not an event)
                    if (pos + 1 > len) { warnings.truncated_chunks++; pos = len; break; }
                    pos += 1;
                    break;
                }
                case 0x02: {  // Pressure set, bilevel: pressure + EPAP
                    if (pos + 2 > len) { warnings.truncated_chunks++; pos = len; break; }
                    pos += 2;
                    break;
                }
                case 0x03:    // Opti-Start pressure change
                case 0x04: {  // Pressure pulse
                    if (pos + 1 > len) { warnings.truncated_chunks++; pos = len; break; }
                    pos += 1;
                    break;
                }
                case 0x05:    // RERA
                case 0x06:    // Obstructive apnea
                case 0x07:    // Clear airway
                case 0x0A:    // Hypopnea (type 1)
                case 0x0C: {  // Flow limitation
                    if (pos + 1 > len) { warnings.truncated_chunks++; pos = len; break; }
                    uint8_t elapsed = d[pos];
                    pos += 1;
                    // elapsed is back-distance to onset, NOT the duration (doc 4.2)
                    session.events.emplace_back(mapFamily0EventCode(code).value(),
                                                endStamped(0, elapsed), 0.0);
                    parsed_any = true;
                    break;
                }
                case 0x0D: {  // Vibratory snore: no fields, stamped at clock
                    session.events.emplace_back(EventType::VIBRATORY_SNORE, clock, 0.0);
                    parsed_any = true;
                    break;
                }
                case 0x0B: {  // Hypopnea (type 2): duration + elapsed
                    if (pos + 2 > len) { warnings.truncated_chunks++; pos = len; break; }
                    uint8_t duration = d[pos];
                    uint8_t elapsed = d[pos + 1];
                    pos += 2;
                    session.events.emplace_back(EventType::HYPOPNEA,
                                                clock - std::chrono::seconds(elapsed),
                                                static_cast<double>(duration));
                    parsed_any = true;
                    break;
                }
                case 0x0E:    // Variable breathing
                case 0x0F:    // Periodic breathing
                case 0x10: {  // Large leak
                    if (pos + 3 > len) { warnings.truncated_chunks++; pos = len; break; }
                    double duration = readU16(d + pos) * 2.0;  // stored halved (doc 4.2)
                    uint8_t elapsed = d[pos + 2];
                    pos += 3;
                    session.events.emplace_back(mapFamily0EventCode(code).value(),
                                                endStamped(duration, elapsed), duration);
                    parsed_any = true;
                    break;
                }
                case 0x11: {  // Statistics: leak, snore, EPAP (graph data)
                    if (pos + 3 > len) { warnings.truncated_chunks++; pos = len; break; }
                    EventStat s;
                    s.timestamp = clock;
                    s.total_leak = d[pos];
                    s.snore = d[pos + 1];
                    s.epap = d[pos + 2] * kPressureTick;
                    stats.push_back(s);
                    pos += 3;
                    break;
                }
                case 0x12: {  // Snore-related stats, skipped
                    if (pos + 2 > len) { warnings.truncated_chunks++; pos = len; break; }
                    pos += 2;
                    break;
                }
                default: {
                    // Unknown code: field lengths unknown, so the rest of this
                    // block cannot be framed. Count, log once, abandon block.
                    warnings.unknown_event_codes++;
                    if (logged_unknown.insert(code).second) {
                        std::cerr << "PhilipsParser: unknown .002 event code 0x"
                                  << std::hex << static_cast<int>(code) << std::dec
                                  << " in session " << chunk.session_number
                                  << ", abandoning block" << std::endl;
                    }
                    pos = len;
                    break;
                }
            }
        }
    }

    return parsed_any;
}

} // namespace cpapdash::parser

#endif // CPAPDASH_WITH_PHILIPS
