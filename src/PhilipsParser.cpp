#include "sleeplink/parser/PhilipsParser.h"

#ifdef SLEEPLINK_WITH_PHILIPS

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

namespace sleeplink::parser {

PhilipsParser::PhilipsParser() = default;

std::vector<uint8_t> PhilipsParser::getDecryptedData(const std::string& filepath) {
    // Check if file is encrypted (.B##) or plain (.0##)
    std::string ext = filepath.substr(filepath.find_last_of('.') + 1);
    std::string ext_upper = ext;
    std::transform(ext_upper.begin(), ext_upper.end(), ext_upper.begin(),
                   [](unsigned char c) { return std::toupper(c); });

    if (ext_upper.size() >= 1 && ext_upper[0] == 'B') {
        // Encrypted DS2 file
        return crypto_.decryptFile(filepath);
    } else {
        // Plain PRS1 file — read raw
        std::ifstream file(filepath, std::ios::binary | std::ios::ate);
        if (!file) return {};
        auto size = file.tellg();
        file.seekg(0);
        std::vector<uint8_t> data(static_cast<size_t>(size));
        file.read(reinterpret_cast<char*>(data.data()), size);
        return data;
    }
}

std::vector<uint8_t> PhilipsParser::getDecryptedData(const uint8_t* data, size_t len,
                                                      const std::string& extension) {
    std::string ext_upper = extension;
    std::transform(ext_upper.begin(), ext_upper.end(), ext_upper.begin(),
                   [](unsigned char c) { return std::toupper(c); });

    if (ext_upper.size() >= 1 && ext_upper[0] == 'B') {
        return crypto_.decrypt(data, len);
    } else {
        return std::vector<uint8_t>(data, data + len);
    }
}

std::unique_ptr<ParsedSession> PhilipsParser::parseSession(
    const std::string& session_dir,
    const std::string& device_id,
    const std::string& device_name,
    std::optional<std::chrono::system_clock::time_point> /*session_start*/)
{
    auto session = std::make_unique<ParsedSession>();
    session->device_id = device_id;
    session->device_name = device_name;
    session->manufacturer = DeviceManufacturer::PHILIPS;

    // Find summary (.001 or .B01) and events (.002 or .B02) files
    std::string summary_file, events_file;

    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(session_dir, ec)) return nullptr;

    for (const auto& entry : fs::directory_iterator(session_dir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        std::string name = entry.path().filename().string();
        std::string name_upper = name;
        std::transform(name_upper.begin(), name_upper.end(), name_upper.begin(),
                       [](unsigned char c) { return std::toupper(c); });

        // Match .001/.B01 (summary) and .002/.B02 (events)
        if (name_upper.size() >= 4) {
            std::string ext = name_upper.substr(name_upper.size() - 4);
            if (ext == ".001" || ext == ".B01") summary_file = session_dir + "/" + name;
            else if (ext == ".002" || ext == ".B02") events_file = session_dir + "/" + name;
        }
    }

    // Parse summary
    if (!summary_file.empty()) {
        auto data = getDecryptedData(summary_file);
        if (!data.empty()) {
            auto chunks = PRS1ChunkReader::parseChunks(data.data(), data.size());
            parseSummary(chunks, *session);
        }
    }

    // Parse events
    if (!events_file.empty()) {
        auto data = getDecryptedData(events_file);
        if (!data.empty()) {
            auto chunks = PRS1ChunkReader::parseChunks(data.data(), data.size());
            parseEvents(chunks, *session);
            session->has_events = true;
            session->status = ParsedSession::Status::COMPLETED;
        }
    }

    session->calculateMetrics();
    return session;
}

std::unique_ptr<ParsedSession> PhilipsParser::parseSessionFromBuffers(
    const std::map<std::string, std::pair<const uint8_t*, size_t>>& buffers,
    const std::string& device_id,
    const std::string& device_name,
    const std::string& /*session_start_str*/)
{
    auto session = std::make_unique<ParsedSession>();
    session->device_id = device_id;
    session->device_name = device_name;
    session->manufacturer = DeviceManufacturer::PHILIPS;

    for (const auto& [key, buf] : buffers) {
        std::string k = key;
        std::transform(k.begin(), k.end(), k.begin(),
                       [](unsigned char c) { return std::toupper(c); });

        auto data = getDecryptedData(buf.first, buf.second, k);
        if (data.empty()) continue;

        auto chunks = PRS1ChunkReader::parseChunks(data.data(), data.size());

        if (k.find("001") != std::string::npos || k.find("B01") != std::string::npos) {
            parseSummary(chunks, *session);
        } else if (k.find("002") != std::string::npos || k.find("B02") != std::string::npos) {
            parseEvents(chunks, *session);
            session->has_events = true;
            session->status = ParsedSession::Status::COMPLETED;
        }
    }

    session->calculateMetrics();
    return session;
}

std::unique_ptr<ISessionParser> createPhilipsParser() {
    return std::make_unique<PhilipsParser>();
}

} // namespace sleeplink::parser

#endif // SLEEPLINK_WITH_PHILIPS
