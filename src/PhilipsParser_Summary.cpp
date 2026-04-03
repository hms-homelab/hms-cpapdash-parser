#include "sleeplink/parser/PhilipsParser.h"

#ifdef SLEEPLINK_WITH_PHILIPS

#include <cstring>

namespace sleeplink::parser {

// Summary (.001) slice event types for F0V6
enum class SummarySliceType : uint8_t {
    EQUIPMENT_ON  = 0x00,
    EQUIPMENT_OFF = 0x01,
    MASK_ON       = 0x03,
    MASK_OFF      = 0x04,
    SETTINGS      = 0x05,  // Contains device configuration
};

bool PhilipsParser::parseSummary(
    const std::vector<std::pair<PRS1ChunkHeader, std::vector<uint8_t>>>& chunks,
    ParsedSession& session)
{
    if (chunks.empty()) return false;

    // Use the first chunk's header for session metadata
    const auto& first_hdr = chunks.front().first;
    session.serial_number = std::to_string(first_hdr.session_id);

    std::optional<uint32_t> equipment_on_ts;
    std::optional<uint32_t> equipment_off_ts;

    for (const auto& [hdr, data] : chunks) {
        if (hdr.family != 0 || hdr.family_version != 6) continue;
        if (hdr.ext != 1) continue;  // Only .001 summary files

        // Parse slice events from chunk data
        size_t pos = 0;
        while (pos < data.size()) {
            if (pos + 1 > data.size()) break;

            uint8_t slice_type = data[pos++];

            // Each slice has a 2-byte timestamp delta
            if (pos + 2 > data.size()) break;
            uint16_t ts_delta = data[pos] | (data[pos + 1] << 8);
            pos += 2;

            uint32_t event_ts = hdr.timestamp + ts_delta;

            switch (static_cast<SummarySliceType>(slice_type)) {
                case SummarySliceType::EQUIPMENT_ON:
                    equipment_on_ts = event_ts;
                    session.session_start = std::chrono::system_clock::from_time_t(event_ts);
                    break;

                case SummarySliceType::EQUIPMENT_OFF:
                    equipment_off_ts = event_ts;
                    session.session_end = std::chrono::system_clock::from_time_t(event_ts);
                    break;

                case SummarySliceType::MASK_ON:
                    session.has_summary = true;
                    break;

                case SummarySliceType::MASK_OFF:
                    break;

                case SummarySliceType::SETTINGS: {
                    // Parse settings block — variable length, depends on hblock
                    DeviceSettings settings;

                    // The settings data follows; exact byte layout depends on F0V6
                    // For now, extract what we can safely:
                    if (pos + 2 <= data.size()) {
                        uint8_t mode = data[pos];
                        settings.therapy_mode = mode;
                    }

                    session.settings = settings;
                    break;
                }

                default:
                    // Unknown slice type — skip remaining data
                    // Without knowing the size, we can't safely continue
                    // TODO: use hblock sizes when available
                    break;
            }

            // Skip any remaining data for this slice based on hblock
            if (hdr.hblock.count(slice_type)) {
                size_t expected_size = static_cast<size_t>(hdr.hblock.at(slice_type));
                // We already consumed 2 bytes for timestamp
                if (expected_size > 2) {
                    size_t remaining = expected_size - 2;
                    if (pos + remaining <= data.size()) {
                        pos += remaining;
                    }
                }
            }
        }
    }

    // Calculate duration
    if (equipment_on_ts && equipment_off_ts && *equipment_off_ts > *equipment_on_ts) {
        session.duration_seconds = static_cast<int>(*equipment_off_ts - *equipment_on_ts);
    }

    session.file_complete = equipment_off_ts.has_value();
    return true;
}

} // namespace sleeplink::parser

#endif // SLEEPLINK_WITH_PHILIPS
