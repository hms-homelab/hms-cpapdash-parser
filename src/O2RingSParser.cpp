#include "cpapdash/parser/O2RingSParser.h"

#include <cctype>
#include <chrono>
#include <ctime>

namespace cpapdash::parser {

namespace {

constexpr uint8_t kHeader[O2RingSParser::HEADER_SIZE] = {0x01, 0x03, 0x00, 0x00, 0x00,
                                                         0x00, 0x00, 0x00, 0x04, 0x00};
constexpr uint8_t kTrailerMagic[4] = {0x48, 0x12, 0x5a, 0xda};
constexpr uint8_t kNoReading = 0xFF;   // OximetrySample's invalid sentinel

// YYYYMMDDhhmmss from the base name, as the ring's wall-clock digits.
std::optional<std::chrono::system_clock::time_point> startFromName(const std::string& filename) {
    const auto slash = filename.find_last_of("/\\");
    const std::string base = slash == std::string::npos ? filename : filename.substr(slash + 1);
    if (base.size() < 14) return std::nullopt;
    for (size_t i = 0; i < 14; i++)
        if (!std::isdigit(static_cast<unsigned char>(base[i]))) return std::nullopt;
    if (base.size() > 14 && std::isdigit(static_cast<unsigned char>(base[14]))) return std::nullopt;

    auto num = [&](size_t at, size_t n) { return std::stoi(base.substr(at, n)); };
    std::tm tm{};
    tm.tm_year = num(0, 4) - 1900;
    tm.tm_mon  = num(4, 2) - 1;
    tm.tm_mday = num(6, 2);
    tm.tm_hour = num(8, 2);
    tm.tm_min  = num(10, 2);
    tm.tm_sec  = num(12, 2);
    if (tm.tm_mon < 0 || tm.tm_mon > 11 || tm.tm_mday < 1 || tm.tm_mday > 31 ||
        tm.tm_hour > 23 || tm.tm_min > 59 || tm.tm_sec > 60)
        return std::nullopt;
#ifdef _WIN32
    const time_t epoch = _mkgmtime(&tm);
#else
    const time_t epoch = timegm(&tm);
#endif
    if (epoch == -1) return std::nullopt;
    return std::chrono::system_clock::from_time_t(epoch);
}

}  // namespace

bool O2RingSParser::looksLike(const uint8_t* data, size_t len) {
    if (!data || len < HEADER_SIZE) return false;
    for (size_t i = 0; i < HEADER_SIZE; i++)
        if (data[i] != kHeader[i]) return false;
    return true;
}

bool O2RingSParser::isComplete(const uint8_t* data, size_t len) {
    if (!looksLike(data, len) || len < HEADER_SIZE + TRAILER_SIZE) return false;
    const uint8_t* m = data + len - TRAILER_SIZE + 4;
    for (size_t i = 0; i < 4; i++)
        if (m[i] != kTrailerMagic[i]) return false;
    return true;
}

std::optional<OximetrySession> O2RingSParser::parse(const uint8_t* data, size_t len,
                                                    const std::string& filename) {
    if (!looksLike(data, len)) return std::nullopt;
    const auto start = startFromName(filename);
    if (!start) return std::nullopt;

    // The trailer is statistics, not samples: it is left out of them.
    const size_t body = len - HEADER_SIZE - (isComplete(data, len) ? TRAILER_SIZE : 0);
    const size_t records = body / RECORD_SIZE;
    if (records == 0) return std::nullopt;

    OximetrySession session;
    const auto slash = filename.find_last_of("/\\");
    session.filename        = slash == std::string::npos ? filename : filename.substr(slash + 1);
    session.start_time      = *start;
    session.sample_interval = 1.0;
    session.duration_seconds = static_cast<int>(records);
    session.end_time        = *start + std::chrono::seconds(records);
    session.samples.reserve(records);

    for (size_t i = 0; i < records; i++) {
        const uint8_t* r = data + HEADER_SIZE + i * RECORD_SIZE;
        OximetrySample s;
        s.timestamp    = *start + std::chrono::seconds(i);
        // This format's "no reading" values become OximetrySample's one sentinel,
        // so valid() and every metric treat them as VLDParser's are treated.
        s.spo2         = (r[0] == 0 || r[0] > 100) ? kNoReading : r[0];
        s.heart_rate   = (r[1] == 0 || r[1] == 255) ? kNoReading : r[1];
        s.invalid_flag = r[2];
        s.motion       = 0;
        s.vibration    = 0;
        session.samples.push_back(s);
    }
    session.metrics = VLDParser::calculateMetrics(session.samples, session.sample_interval);
    return session;
}

OximetryFormat detectOximetryFormat(const uint8_t* data, size_t len) {
    if (O2RingSParser::looksLike(data, len)) return OximetryFormat::O2RingS;
    if (data && len >= 2 && data[0] == 0x03 && data[1] == 0x00) return OximetryFormat::Vld3;
    return OximetryFormat::Unknown;
}

std::optional<OximetrySession> parseOximetryFile(const uint8_t* data, size_t len,
                                                 const std::string& filename) {
    switch (detectOximetryFormat(data, len)) {
        case OximetryFormat::O2RingS: return O2RingSParser::parse(data, len, filename);
        case OximetryFormat::Vld3:    return VLDParser::parse(data, len, filename);
        case OximetryFormat::Unknown: break;
    }
    return std::nullopt;
}

} // namespace cpapdash::parser
