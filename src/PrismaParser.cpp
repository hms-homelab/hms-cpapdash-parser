#ifdef CPAPDASH_WITH_LOWENSTEIN

#include "cpapdash/parser/PrismaParser.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <algorithm>

namespace cpapdash::parser {

namespace fs = std::filesystem;

// ── Factory ─────────────────────────────────────────────────────────────────

std::unique_ptr<ISessionParser> createLowensteinParser() {
    return std::make_unique<PrismaParser>();
}

// ── device.xml parsing ──────────────────────────────────────────────────────

PrismaDeviceInfo PrismaParser::parseDeviceXml(const std::string& filepath) {
    PrismaDeviceInfo info;
    std::ifstream f(filepath);
    if (!f.is_open()) return info;

    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());

    auto extract = [&](const std::string& tag) -> std::string {
        std::string pattern = "<" + tag + " value=\"";
        auto pos = content.find(pattern);
        if (pos == std::string::npos) return "";
        pos += pattern.size();
        auto end = content.find("\"", pos);
        if (end == std::string::npos) return "";
        return content.substr(pos, end - pos);
    };

    info.serial_number = extract("DeviceSerialNumber");
    std::string dt = extract("DeviceType");
    if (!dt.empty()) info.device_type = std::stoi(dt);
    info.fw_version = extract("FWVersion");
    info.fw_build = extract("FWBuild");
    std::string hw = extract("MainboardHWVersion");
    if (!hw.empty()) info.hw_version = std::stoi(hw);

    return info;
}

// ── Therapy mode mapping (Prisma Line param 1003 -> our mode enum) ──────────

std::optional<int> PrismaParser::mapTherapyMode(int prisma_mode) {
    switch (prisma_mode) {
        case 1:  return 0;  // CPAP
        case 2:  return 1;  // APAP
        case 3:  return 7;  // AcSV -> ASV
        case 9:  return 2;  // Auto-S -> BiPAP
        case 10: return 2;  // Auto-S/T -> BiPAP
        default: return std::nullopt;
    }
}

// ── parseSession ────────────────────────────────────────────────────────────

std::unique_ptr<ParsedSession> PrismaParser::parseSession(
    const std::string& session_dir,
    const std::string& device_id,
    const std::string& device_name,
    std::optional<std::chrono::system_clock::time_point> session_start)
{
    std::error_code ec;
    if (!fs::is_directory(session_dir, ec)) {
        std::cerr << "PrismaParser: not a directory: " << session_dir << std::endl;
        return nullptr;
    }

    std::string event_file, signal_file, device_file;

    for (const auto& entry : fs::directory_iterator(session_dir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        std::string name = entry.path().filename().string();
        if (name.find("event_") == 0 && name.find(".xml") != std::string::npos) {
            event_file = entry.path().string();
        } else if (name.find("signal_") == 0 && name.find(".wmedf") != std::string::npos) {
            signal_file = entry.path().string();
        } else if (name == "device.xml") {
            device_file = entry.path().string();
        }
    }

    if (signal_file.empty()) {
        std::cerr << "PrismaParser: no signal .wmedf file in " << session_dir << std::endl;
        return nullptr;
    }

    auto session = std::make_unique<ParsedSession>();
    session->device_id = device_id;
    session->device_name = device_name;
    session->manufacturer = DeviceManufacturer::LOWENSTEIN;

    if (!device_file.empty()) {
        auto dev = parseDeviceXml(device_file);
        session->serial_number = dev.serial_number;
        session->model_id = dev.device_type;
    }

    if (!parseSignalWmedf(signal_file, *session)) {
        std::cerr << "PrismaParser: failed to parse WMEDF: " << signal_file << std::endl;
        return nullptr;
    }

    auto actual_start = session->session_start.value_or(
        session_start.value_or(std::chrono::system_clock::now()));

    if (!event_file.empty()) {
        parseEventXml(event_file, *session, actual_start);
        session->has_events = true;
    }

    session->status = ParsedSession::Status::COMPLETED;
    session->file_complete = true;
    session->calculateMetrics();

    return session;
}

// ── parseSessionFromBuffers ─────────────────────────────────────────────────

std::unique_ptr<ParsedSession> PrismaParser::parseSessionFromBuffers(
    const std::map<std::string, std::pair<const uint8_t*, size_t>>& buffers,
    const std::string& device_id,
    const std::string& device_name,
    const std::string& session_start_str)
{
    // Write buffers to temp files and delegate to parseSession
    auto tmp = fs::temp_directory_path() / ("prisma_" + std::to_string(
        std::chrono::system_clock::now().time_since_epoch().count()));
    fs::create_directories(tmp);

    for (const auto& [name, buf] : buffers) {
        std::ofstream out(tmp / name, std::ios::binary);
        out.write(reinterpret_cast<const char*>(buf.first), buf.second);
    }

    auto result = parseSession(tmp.string(), device_id, device_name);

    fs::remove_all(tmp);
    return result;
}

} // namespace cpapdash::parser

#endif // CPAPDASH_WITH_LOWENSTEIN
