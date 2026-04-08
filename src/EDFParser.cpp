#include "cpapdash/parser/EDFParser.h"
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
    if (std::regex_search(rec, m, std::regex("MID=(\\d+)"))) {
        model_id = std::stoi(m[1].str());
    }
    if (std::regex_search(rec, m, std::regex("VID=(\\d+)"))) {
        version_id = std::stoi(m[1].str());
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

    // Find ALL checkpoint files (multiple BRP/PLD/SAD per session)
    std::vector<std::string> brp_files, pld_files, sad_files;
    std::string eve_file, csl_file;

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
            eve_file = entry.path().string();
        } else if (lower.find("_csl.edf") != std::string::npos) {
            csl_file = entry.path().string();
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

    // Parse EVE (optional - written hours after session)
    if (!eve_file.empty()) {
        EDFFile edf;
        if (edf.open(eve_file)) {
            parseEVEFile(edf, *session);
            session->has_events = true;
        }
    } else {
        session->has_events = false;
    }

    // Parse CSL (optional - summary)
    if (!csl_file.empty()) {
        session->has_summary = true;
    } else {
        session->has_summary = false;
    }

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
    if (!session_start_str.empty() && session_start_str.size() >= 15) {
        std::tm t = {};
        t.tm_year = std::stoi(session_start_str.substr(0, 4)) - 1900;
        t.tm_mon  = std::stoi(session_start_str.substr(4, 2)) - 1;
        t.tm_mday = std::stoi(session_start_str.substr(6, 2));
        t.tm_hour = std::stoi(session_start_str.substr(9, 2));
        t.tm_min  = std::stoi(session_start_str.substr(11, 2));
        t.tm_sec  = std::stoi(session_start_str.substr(13, 2));
        t.tm_isdst = -1;
        session->session_start = std::chrono::system_clock::from_time_t(std::mktime(&t));
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
