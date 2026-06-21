#ifdef CPAPDASH_WITH_LOWENSTEIN

#include "cpapdash/parser/PrismaParser.h"
#include <fstream>
#include <iostream>
#include <regex>
#include <cstdlib>
#include <cctype>

namespace cpapdash::parser {

// ── RespEventID -> EventType ────────────────────────────────────────────────

std::optional<EventType> PrismaParser::mapRespEventId(int id) {
    switch (id) {
        case 101: return EventType::OBSTRUCTIVE;
        case 102: return EventType::CENTRAL;
        case 103: case 105: case 106:
                  return EventType::APNEA;
        case 111: case 112: case 113:
                  return EventType::HYPOPNEA;
        case 121: return EventType::RERA;
        case 131: return EventType::VIBRATORY_SNORE;
        case 151: return EventType::FLOW_LIMITATION;
        case 161: return EventType::LARGE_LEAK;
        case 181: return EventType::CSR;
        default:  return std::nullopt;
    }
}

// ── XML attribute extractor ─────────────────────────────────────────────────

namespace {

// Extract attr="value", tolerating optional whitespace around '=' and after the
// opening quote. Löwenstein SMART max writes RespEvent attributes spaced out
// (RespEventID = "1230") while DeviceEvent uses the tight form, so we accept
// both. A left-boundary check avoids matching an attr name embedded in a longer
// one (e.g. "Time" inside "EndTime").
std::string extractAttr(const std::string& line, const std::string& attr) {
    size_t pos = 0;
    while ((pos = line.find(attr, pos)) != std::string::npos) {
        bool left_ok = (pos == 0) || !std::isalnum(static_cast<unsigned char>(line[pos - 1]));
        size_t k = pos + attr.size();
        while (k < line.size() && std::isspace(static_cast<unsigned char>(line[k]))) k++;
        if (left_ok && k < line.size() && line[k] == '=') {
            k++;
            while (k < line.size() && std::isspace(static_cast<unsigned char>(line[k]))) k++;
            if (k < line.size() && line[k] == '"') {
                k++;
                auto end = line.find('"', k);
                if (end != std::string::npos) return line.substr(k, end - k);
            }
            return "";
        }
        pos += 1;
    }
    return "";
}

} // anonymous namespace

// ── parseEventXml ───────────────────────────────────────────────────────────

bool PrismaParser::parseEventXml(
    const std::string& filepath,
    ParsedSession& session,
    std::chrono::system_clock::time_point session_start)
{
    std::ifstream f(filepath);
    if (!f.is_open()) {
        std::cerr << "PrismaParser: cannot open event XML: " << filepath << std::endl;
        return false;
    }

    std::string line;
    while (std::getline(f, line)) {
        if (line.find("<RespEvent") != std::string::npos) {
            std::string id_str = extractAttr(line, "RespEventID");
            std::string end_str = extractAttr(line, "EndTime");
            std::string dur_str = extractAttr(line, "Duration");

            if (id_str.empty() || end_str.empty()) continue;

            int resp_id = std::atoi(id_str.c_str());
            auto event_type = mapRespEventId(resp_id);
            if (!event_type) continue;

            double end_time_sec = std::atof(end_str.c_str());
            double duration_sec = dur_str.empty() ? 0.0 : std::atof(dur_str.c_str());
            double start_time_sec = end_time_sec - duration_sec;
            if (start_time_sec < 0) start_time_sec = 0;

            auto event_ts = session_start +
                std::chrono::milliseconds(static_cast<long long>(start_time_sec * 1000));

            SleepEvent evt(*event_type, event_ts, duration_sec);
            session.events.push_back(std::move(evt));

        } else if (line.find("<DeviceEvent") != std::string::npos) {
            std::string dev_id = extractAttr(line, "DeviceEventID");
            if (dev_id != "0") continue;

            std::string param_str = extractAttr(line, "ParameterID");
            std::string value_str = extractAttr(line, "NewValue");
            if (param_str.empty() || value_str.empty()) continue;

            int param_id = std::atoi(param_str.c_str());
            int value = std::atoi(value_str.c_str());

            if (!session.settings) session.settings = DeviceSettings{};
            auto& s = *session.settings;

            switch (param_id) {
                case 1003: s.therapy_mode = mapTherapyMode(value); break;
                case 1083: s.humidifier_level = value; break;
                case 1084: // auto-start (bool, not in DeviceSettings)
                    break;
                case 1085: s.ramp_pressure = value / 100.0 * 1.0197; break;
                case 1091: s.hose_diameter = value; break;
                case 1123: s.flex_level = value; break;
                case 1125: s.ramp_pressure = value / 100.0 * 1.0197; break;
                case 1127: s.ramp_time = value; break;
                case 1199: s.max_pressure = value / 100.0 * 1.0197; break;
                case 1200: s.ipap = value / 100.0 * 1.0197; break;
                case 1201: s.epap = value / 100.0 * 1.0197; break;
                default: break;
            }
        }
    }

    return true;
}

} // namespace cpapdash::parser

#endif // CPAPDASH_WITH_LOWENSTEIN
