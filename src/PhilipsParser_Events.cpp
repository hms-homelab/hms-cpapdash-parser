#include "sleeplink/parser/PhilipsParser.h"

#ifdef SLEEPLINK_WITH_PHILIPS

#include <cstring>
#include <cmath>

namespace sleeplink::parser {

// F0V6 event codes — from OSCAR prs1_parser_xpap.cpp:2155 (ParseEventsF0V6)
// Minimum data sizes per event code (excluding 2-byte timestamp delta)
static const int F0V6_MIN_SIZES[] = {
    2,  // 0x00: (never seen)
    3,  // 0x01: Pressure Set (1 pressure byte + 2 ts)
    4,  // 0x02: Bi-level Pressure (2 pressure bytes + 2 ts)
    3,  // 0x03: Auto-CPAP Start Press (1 byte + 2 ts)
    3,  // 0x04: Pressure Pulse (1 byte + 2 ts)
    3,  // 0x05: RERA (1 elapsed byte + 2 ts)
    3,  // 0x06: Obstructive Apnea (1 elapsed byte + 2 ts)
    3,  // 0x07: Clear Airway (1 elapsed byte + 2 ts)
    3,  // 0x08: (never seen)
    2,  // 0x09: (never seen)
    3,  // 0x0a: Hypopnea type A (1 elapsed byte + 2 ts)
    4,  // 0x0b: Hypopnea type B (2 bytes + 2 ts)
    3,  // 0x0c: Flow Limitation (1 elapsed byte + 2 ts)
    2,  // 0x0d: Vibratory Snore (0 data bytes + 2 ts)
    5,  // 0x0e: Variable Breathing (3 bytes + 2 ts)
    5,  // 0x0f: Periodic Breathing (3 bytes + 2 ts)
    5,  // 0x10: Large Leak (3 bytes + 2 ts)
    5,  // 0x11: Statistics (variable, min 3 + 2 ts)
    4,  // 0x12: Snore per-pressure (variable, NO timestamp delta)
    3,  // 0x13: (unknown)
    3,  // 0x14: Hypopnea type C
    3,  // 0x15: Hypopnea type D
};
static const int F0V6_NCODES = sizeof(F0V6_MIN_SIZES) / sizeof(int);

bool PhilipsParser::parseEvents(
    const std::vector<std::pair<PRS1ChunkHeader, std::vector<uint8_t>>>& chunks,
    ParsedSession& session)
{
    if (chunks.empty()) return false;

    for (const auto& [hdr, data] : chunks) {
        if (hdr.family != 0 || hdr.family_version != 6) continue;
        if (hdr.ext != 2) continue;  // Only .002 event files

        if (data.empty()) continue;

        size_t pos = 0;
        int t = 0;  // cumulative timestamp offset from chunk timestamp

        while (pos < data.size()) {
            uint8_t code = data[pos++];

            // Look up expected data size from hblock
            int16_t event_size = 0;
            auto it = hdr.hblock.find(code);
            if (it != hdr.hblock.end()) {
                event_size = it->second;
            } else if (code < F0V6_NCODES) {
                event_size = F0V6_MIN_SIZES[code];
            } else {
                break;  // Unknown code with no hblock entry — can't continue
            }

            if (pos + event_size > data.size()) break;

            size_t startpos = pos;

            // Most events have a 2-byte LE timestamp delta (except 0x12)
            if (code != 0x12) {
                if (pos + 2 > data.size()) break;
                t += data[pos] | (data[pos + 1] << 8);
                pos += 2;
            }

            auto event_time = std::chrono::system_clock::from_time_t(
                static_cast<time_t>(hdr.timestamp + t));

            switch (code) {
                case 0x01: {
                    // Pressure Set — 1 byte (0.1 cmH2O)
                    // Store as breathing summary pressure data point
                    if (pos < data.size()) {
                        double pressure = data[pos] * 0.1;
                        BreathingSummary bs(event_time);
                        bs.avg_pressure = pressure;
                        session.breathing_summary.push_back(bs);
                    }
                    break;
                }

                case 0x05: {
                    // RERA
                    if (pos < data.size()) {
                        uint8_t elapsed = data[pos];
                        auto rera_time = std::chrono::system_clock::from_time_t(
                            static_cast<time_t>(hdr.timestamp + t - elapsed));
                        session.events.emplace_back(EventType::RERA, rera_time, 0);
                    }
                    break;
                }

                case 0x06: {
                    // Obstructive Apnea
                    if (pos < data.size()) {
                        uint8_t elapsed = data[pos];
                        auto oa_time = std::chrono::system_clock::from_time_t(
                            static_cast<time_t>(hdr.timestamp + t - elapsed));
                        session.events.emplace_back(EventType::OBSTRUCTIVE, oa_time, 0);
                    }
                    break;
                }

                case 0x07: {
                    // Clear Airway
                    if (pos < data.size()) {
                        uint8_t elapsed = data[pos];
                        auto ca_time = std::chrono::system_clock::from_time_t(
                            static_cast<time_t>(hdr.timestamp + t - elapsed));
                        session.events.emplace_back(EventType::CLEAR_AIRWAY, ca_time, 0);
                    }
                    break;
                }

                case 0x0a: {
                    // Hypopnea type A
                    if (pos < data.size()) {
                        uint8_t elapsed = data[pos];
                        auto hy_time = std::chrono::system_clock::from_time_t(
                            static_cast<time_t>(hdr.timestamp + t - elapsed));
                        session.events.emplace_back(EventType::HYPOPNEA, hy_time, 0);
                    }
                    break;
                }

                case 0x0b: {
                    // Hypopnea type B — 2 bytes (unknown, elapsed)
                    if (pos + 1 < data.size()) {
                        uint8_t elapsed = data[pos + 1];
                        auto hy_time = std::chrono::system_clock::from_time_t(
                            static_cast<time_t>(hdr.timestamp + t - elapsed));
                        session.events.emplace_back(EventType::HYPOPNEA, hy_time, 0);
                    }
                    break;
                }

                case 0x0c: {
                    // Flow Limitation
                    if (pos < data.size()) {
                        uint8_t elapsed = data[pos];
                        auto fl_time = std::chrono::system_clock::from_time_t(
                            static_cast<time_t>(hdr.timestamp + t - elapsed));
                        session.events.emplace_back(EventType::FLOW_LIMITATION, fl_time, 0);
                    }
                    break;
                }

                case 0x0d: {
                    // Vibratory Snore — no data bytes, instantaneous
                    session.events.emplace_back(EventType::VIBRATORY_SNORE, event_time, 0);
                    break;
                }

                case 0x0e: {
                    // Variable Breathing — 3 bytes (duration LE16 *2, elapsed)
                    // Not mapped to a specific EventType yet, skip
                    break;
                }

                case 0x0f: {
                    // Periodic Breathing — 3 bytes (duration LE16 *2, elapsed)
                    if (pos + 2 < data.size()) {
                        uint16_t raw_duration = data[pos] | (data[pos + 1] << 8);
                        int duration = raw_duration * 2;  // F0V6 doubles duration
                        uint8_t elapsed = data[pos + 2];
                        auto pb_time = std::chrono::system_clock::from_time_t(
                            static_cast<time_t>(hdr.timestamp + t - elapsed - duration));
                        session.events.emplace_back(EventType::PERIODIC_BREATHING, pb_time,
                                                    static_cast<double>(duration));
                    }
                    break;
                }

                case 0x10: {
                    // Large Leak — 3 bytes (duration LE16 *2, elapsed)
                    if (pos + 2 < data.size()) {
                        uint16_t raw_duration = data[pos] | (data[pos + 1] << 8);
                        int duration = raw_duration * 2;  // F0V6 doubles duration
                        uint8_t elapsed = data[pos + 2];
                        auto ll_time = std::chrono::system_clock::from_time_t(
                            static_cast<time_t>(hdr.timestamp + t - elapsed - duration));
                        session.events.emplace_back(EventType::LARGE_LEAK, ll_time,
                                                    static_cast<double>(duration));
                    }
                    break;
                }

                case 0x11: {
                    // Statistics — variable length, contains per-interval metrics
                    // Layout depends on hblock size; extract what we can
                    // Common fields: leak, RR, TV, MV, snore count, pressure stats, PTB
                    // TODO: fully decode when we have sample data to validate against
                    break;
                }

                case 0x12: {
                    // Snore per-pressure — no timestamp delta
                    // TODO: decode with sample data
                    break;
                }

                case 0x14:
                case 0x15: {
                    // Additional hypopnea types (F0V6-specific)
                    if (pos < data.size()) {
                        uint8_t elapsed = data[pos];
                        auto hy_time = std::chrono::system_clock::from_time_t(
                            static_cast<time_t>(hdr.timestamp + t - elapsed));
                        session.events.emplace_back(EventType::HYPOPNEA, hy_time, 0);
                    }
                    break;
                }

                default:
                    break;
            }

            // Advance pos to end of event data
            pos = startpos + event_size;
        }
    }

    return !session.events.empty();
}

} // namespace sleeplink::parser

#endif // SLEEPLINK_WITH_PHILIPS
