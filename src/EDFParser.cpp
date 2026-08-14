#include "cpapdash/parser/EDFParser.h"
#include "ParseNum.h"
#include <iostream>
#include <filesystem>
#include <regex>
#include <algorithm>
#include <iomanip>
#include <sstream>

namespace cpapdash::parser {

bool EDFParser::parseDeviceInfo(EDFFile& edf,
                                 std::string& serial_number,
                                 int& model_id,
                                 int& version_id) {
    // Recording field format: "Startdate DD-MMM-YYYY ... SRN=XXXXX MID=XX VID=XX"
    const std::string& rec = edf.recording;

    std::smatch m;
    if (std::regex_search(rec, m, std::regex("SRN=(\\d+)"))) {
        serial_number = m[1].str();
    }
    // \d+ matches arbitrarily long digit runs, which overflow a plain stoi —
    // parseIntOr keeps the existing value instead of throwing (src/ParseNum.h).
    if (std::regex_search(rec, m, std::regex("MID=(\\d+)"))) {
        model_id = parseIntOr(m[1].str(), model_id);
    }
    if (std::regex_search(rec, m, std::regex("VID=(\\d+)"))) {
        version_id = parseIntOr(m[1].str(), version_id);
    }

    return true;
}

std::unique_ptr<ParsedSession> EDFParser::parseSession(
    const std::string& session_dir,
    const std::string& device_id,
    const std::string& device_name,
    std::optional<std::chrono::system_clock::time_point> session_start_from_filename
) {
    if (!std::filesystem::exists(session_dir)) {
        std::cerr << "Parser: Directory not found: " << session_dir << std::endl;
        return nullptr;
    }

    // Find ALL checkpoint files (multiple BRP/PLD/SAD per session).
    //
    // EVE and CSL are vectors for the same reason the others are: a ResMed night
    // is several mask-on blocks, and each block writes its own EVE. These used to
    // be single strings, so the last file directory_iterator happened to yield
    // won that session and every other block's annotations were dropped -- a
    // night could read AHI 0.0 with all its apneas sitting unread on the card.
    // Iteration order is unspecified, so it was not even the same file twice.
    std::vector<std::string> brp_files, pld_files, sad_files;
    std::vector<std::string> eve_files, csl_files;

    for (const auto& entry : std::filesystem::directory_iterator(session_dir)) {
        if (!entry.is_regular_file()) continue;
        std::string filename = entry.path().filename().string();
        std::string lower = filename;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

        if (lower.find("_brp.edf") != std::string::npos) {
            brp_files.push_back(entry.path().string());
        } else if (lower.find("_pld.edf") != std::string::npos) {
            pld_files.push_back(entry.path().string());
        } else if (lower.find("_sad.edf") != std::string::npos || lower.find("_sa2.edf") != std::string::npos) {
            sad_files.push_back(entry.path().string());
        } else if (lower.find("_eve.edf") != std::string::npos) {
            eve_files.push_back(entry.path().string());
        } else if (lower.find("_csl.edf") != std::string::npos) {
            csl_files.push_back(entry.path().string());
        }
    }

    if (brp_files.empty()) {
        std::cerr << "Parser: No BRP.edf files found in " << session_dir << std::endl;
        return nullptr;
    }

    // Sort checkpoint files chronologically (by filename timestamp)
    auto sort_by_filename = [](const std::string& a, const std::string& b) {
        return std::filesystem::path(a).filename() < std::filesystem::path(b).filename();
    };
    std::sort(brp_files.begin(), brp_files.end(), sort_by_filename);
    std::sort(pld_files.begin(), pld_files.end(), sort_by_filename);
    std::sort(sad_files.begin(), sad_files.end(), sort_by_filename);
    std::sort(eve_files.begin(), eve_files.end(), sort_by_filename);
    std::sort(csl_files.begin(), csl_files.end(), sort_by_filename);

    // Create session
    auto session = std::make_unique<ParsedSession>();
    session->device_id = device_id;
    session->device_name = device_name;

    // Use filename timestamp as session identifier
    if (session_start_from_filename.has_value()) {
        session->session_start = session_start_from_filename;
    }

    // Parse ALL BRP files and combine breathing data
    for (size_t i = 0; i < brp_files.size(); ++i) {
        EDFFile edf;
        if (!edf.open(brp_files[i])) continue;

        // Parse device info from first BRP header
        if (i == 0) {
            int mid = 0, vid = 0;
            parseDeviceInfo(edf, session->serial_number, mid, vid);
            session->model_id = mid;
            session->version_id = vid;
        }

        parseBRPFile(edf, *session);
    }

    // Parse ALL SAD files and combine vitals data
    for (const auto& sad_path : sad_files) {
        EDFFile edf;
        if (!edf.open(sad_path)) continue;
        parseSADFile(edf, *session);
    }

    // Parse ALL PLD files and extract machine-calculated metrics
    for (const auto& pld_path : pld_files) {
        EDFFile edf;
        if (!edf.open(pld_path)) continue;
        parsePLDFile(edf, *session);
    }

    // Parse ALL EVE files (optional - written hours after the block they cover).
    // parseEVEFile only ever appends to session->events, so the union of every
    // block's annotations falls out of the loop.
    session->has_events = false;
    for (const auto& eve_path : eve_files) {
        EDFFile edf;
        if (!edf.open(eve_path)) continue;
        parseEVEFile(edf, *session);
        session->has_events = true;
    }

    // Events arrive per EVE file, so the concatenation is only sorted within
    // each block. Anything reading them as a timeline needs the whole night.
    std::sort(session->events.begin(), session->events.end(),
              [](const SleepEvent& a, const SleepEvent& b) {
                  return a.timestamp < b.timestamp;
              });

    // Parse CSL (optional - summary)
    session->has_summary = !csl_files.empty();

    // Calculate aggregated metrics
    session->calculateMetrics();

    return session;
}

std::unique_ptr<ParsedSession> EDFParser::parseSessionFromBuffers(
    const uint8_t* brp, size_t brp_len,
    const uint8_t* pld, size_t pld_len,
    const uint8_t* sad, size_t sad_len,
    const uint8_t* eve, size_t eve_len,
    const std::string& device_id,
    const std::string& device_name,
    const std::string& session_start_str
) {
    if (!brp || brp_len == 0) {
        std::cerr << "Parser: BRP buffer is required" << std::endl;
        return nullptr;
    }

    auto session = std::make_unique<ParsedSession>();
    session->device_id = device_id;
    session->device_name = device_name;

    // Parse session_start_str if provided ("YYYYMMDD_HHMMSS")
    // The length check above says nothing about the characters being digits —
    // this string comes from an uploaded filename. Parse every field
    // non-throwing (src/ParseNum.h) and drop the timestamp entirely if any
    // component is unusable, rather than recording a garbage date.
    if (!session_start_str.empty() && session_start_str.size() >= 15) {
        const int year = parseIntOr(session_start_str.substr(0, 4), -1);
        const int mon  = parseIntOr(session_start_str.substr(4, 2), -1);
        const int mday = parseIntOr(session_start_str.substr(6, 2), -1);
        const int hour = parseIntOr(session_start_str.substr(9, 2), -1);
        const int min  = parseIntOr(session_start_str.substr(11, 2), -1);
        const int sec  = parseIntOr(session_start_str.substr(13, 2), -1);

        if (year >= 0 && mon >= 0 && mday >= 0 && hour >= 0 && min >= 0 && sec >= 0) {
            std::tm t = {};
            t.tm_year = year - 1900;
            t.tm_mon  = mon - 1;
            t.tm_mday = mday;
            t.tm_hour = hour;
            t.tm_min  = min;
            t.tm_sec  = sec;
            t.tm_isdst = -1;
            session->session_start = std::chrono::system_clock::from_time_t(std::mktime(&t));
        }
    }

    // Parse BRP
    {
        EDFFile edf;
        if (!edf.open(brp, brp_len)) {
            std::cerr << "Parser: Failed to parse BRP buffer" << std::endl;
            return nullptr;
        }

        int mid = 0, vid = 0;
        parseDeviceInfo(edf, session->serial_number, mid, vid);
        session->model_id = mid;
        session->version_id = vid;

        if (!parseBRPFile(edf, *session)) {
            return nullptr;
        }
    }

    // Parse SAD
    if (sad && sad_len > 0) {
        EDFFile edf;
        if (edf.open(sad, sad_len)) {
            parseSADFile(edf, *session);
        }
    }

    // Parse PLD
    if (pld && pld_len > 0) {
        EDFFile edf;
        if (edf.open(pld, pld_len)) {
            parsePLDFile(edf, *session);
        }
    }

    // Parse EVE
    if (eve && eve_len > 0) {
        EDFFile edf;
        if (edf.open(eve, eve_len)) {
            parseEVEFile(edf, *session);
            session->has_events = true;
        }
    }

    // Calculate aggregated metrics
    session->calculateMetrics();

    return session;
}

} // namespace cpapdash::parser
