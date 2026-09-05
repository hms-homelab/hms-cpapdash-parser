#include "cpapdash/parser/EDFParser.h"
#include <iostream>
#include <algorithm>

namespace cpapdash::parser {

bool EDFParser::parseEVEFile(EDFFile& edf, ParsedSession& session) {
    if (!edf.isEDFPlus()) {
        return true;
    }

    auto start_time = edf.getStartTime();
    auto annotations = edf.readAnnotations();

    for (const auto& annot : annotations) {
        EventType event_type = EventType::APNEA;  // Default
        std::string desc_lower = annot.description;
        std::transform(desc_lower.begin(), desc_lower.end(), desc_lower.begin(), ::tolower);

        if (desc_lower.find("hypopnea") != std::string::npos) {
            event_type = EventType::HYPOPNEA;
        } else if (desc_lower.find("obstructive") != std::string::npos &&
                   desc_lower.find("apnea") != std::string::npos) {
            event_type = EventType::OBSTRUCTIVE;
        } else if (desc_lower.find("central") != std::string::npos &&
                   desc_lower.find("apnea") != std::string::npos) {
            event_type = EventType::CENTRAL;
        } else if (desc_lower.find("clear") != std::string::npos &&
                   desc_lower.find("airway") != std::string::npos) {
            event_type = EventType::CLEAR_AIRWAY;
        } else if (desc_lower.find("apnea") != std::string::npos) {
            event_type = EventType::APNEA;
        } else if (desc_lower.find("rera") != std::string::npos) {
            event_type = EventType::RERA;
        } else if (desc_lower.find("csr") != std::string::npos) {
            event_type = EventType::CSR;
        } else if (desc_lower.find("arousal") != std::string::npos) {
            // Deliberate, not a fallthrough: RIN computed from arousals alone
            // reconciles against ResMed's own RIN channel on 172 of 175 nights
            // (docs/RESMED_CALCULATION_RULES.md).
            event_type = EventType::RERA;
        } else {
            // Anything we do not recognise -- 'Recording starts' file markers, and
            // whatever a future firmware invents. Recorded, counted in total_events,
            // part of no index. It used to land in RERA, which inflated that count by
            // one per EVE file (SDD-004 1.1).
            event_type = EventType::OTHER;
        }

        double dur = (annot.duration_sec > 0) ? annot.duration_sec : 0.0;

        SleepEvent event(
            event_type,
            start_time + std::chrono::milliseconds(static_cast<long long>(annot.onset_sec * 1000)),
            dur
        );
        event.details = annot.description;
        session.events.push_back(event);
    }

    return true;
}

} // namespace cpapdash::parser
