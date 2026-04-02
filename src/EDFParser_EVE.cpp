#include "sleeplink/parser/EDFParser.h"
#include <iostream>
#include <algorithm>

namespace sleeplink::parser {

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
        } else {
            // Arousal, flow limitation, etc. -- still record them
            event_type = EventType::RERA;
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

} // namespace sleeplink::parser
