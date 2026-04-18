#include "cpapdash/parser/VLDParser.h"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <numeric>
#include <sstream>
#include <iomanip>
#include <ctime>

namespace cpapdash::parser {

static uint16_t read_u16(const uint8_t* p) {
    return p[0] | (static_cast<uint16_t>(p[1]) << 8);
}

static uint32_t read_u32(const uint8_t* p) {
    return p[0] | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

std::string OximetrySession::date_str() const {
    auto tt = std::chrono::system_clock::to_time_t(start_time);
    std::tm tm{};
    gmtime_r(&tt, &tm);
    char buf[9];
    std::strftime(buf, sizeof(buf), "%Y%m%d", &tm);
    return buf;
}

std::optional<OximetrySession> VLDParser::parse(
    const uint8_t* data, size_t len, const std::string& filename)
{
    if (!data || len < HEADER_SIZE) return std::nullopt;

    uint16_t version = read_u16(data);
    if (version != 3) return std::nullopt;

    uint16_t year   = read_u16(data + 2);
    uint8_t  month  = data[4];
    uint8_t  day    = data[5];
    uint8_t  hour   = data[6];
    uint8_t  minute = data[7];
    uint8_t  second = data[8];

    // Duration at offset 0x12 (18) — uint16 LE in seconds
    uint16_t duration_s = read_u16(data + 18);

    // Build start time
    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon  = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min  = minute;
    tm.tm_sec  = second;
    time_t epoch = timegm(&tm);
    if (epoch == -1) return std::nullopt;

    auto start = std::chrono::system_clock::from_time_t(epoch);

    size_t data_len = len - HEADER_SIZE;
    size_t record_count = data_len / RECORD_SIZE;
    if (record_count == 0) return std::nullopt;

    double interval = static_cast<double>(duration_s) / record_count;
    if (interval <= 0) interval = 4.0;

    OximetrySession session;
    session.filename = filename;
    session.start_time = start;
    session.duration_seconds = duration_s;
    session.sample_interval = interval;
    session.samples.reserve(record_count);

    for (size_t i = 0; i < record_count; i++) {
        const uint8_t* rec = data + HEADER_SIZE + i * RECORD_SIZE;
        OximetrySample s;
        s.timestamp = start + std::chrono::milliseconds(
            static_cast<int64_t>(i * interval * 1000));
        s.spo2         = rec[0];
        s.heart_rate   = rec[1];
        s.invalid_flag = rec[2];
        s.motion       = rec[3];
        s.vibration    = rec[4];
        session.samples.push_back(s);
    }

    session.end_time = start + std::chrono::seconds(duration_s);
    session.metrics = calculateMetrics(session.samples, interval);

    return session;
}

std::optional<OximetrySession> VLDParser::parseFile(const std::string& filepath)
{
    std::ifstream f(filepath, std::ios::binary | std::ios::ate);
    if (!f) return std::nullopt;

    auto size = f.tellg();
    if (size < static_cast<std::streamoff>(HEADER_SIZE)) return std::nullopt;

    std::vector<uint8_t> buf(size);
    f.seekg(0);
    f.read(reinterpret_cast<char*>(buf.data()), size);

    // Extract filename from path
    std::string fname;
    auto pos = filepath.find_last_of("/\\");
    fname = (pos != std::string::npos) ? filepath.substr(pos + 1) : filepath;

    return parse(buf.data(), buf.size(), fname);
}

OximetryMetrics VLDParser::calculateMetrics(
    const std::vector<OximetrySample>& samples, double sample_interval)
{
    OximetryMetrics m;
    m.total_samples = static_cast<int>(samples.size());

    std::vector<double> valid_spo2;
    std::vector<int> valid_hr;
    int below_90 = 0, below_88 = 0;

    for (auto& s : samples) {
        if (!s.valid()) continue;
        m.valid_samples++;
        double sp = s.spo2;
        int hr = s.heart_rate;

        valid_spo2.push_back(sp);
        valid_hr.push_back(hr);

        if (sp < m.min_spo2) m.min_spo2 = sp;
        if (sp > m.max_spo2) m.max_spo2 = sp;
        if (hr < m.min_hr) m.min_hr = hr;
        if (hr > m.max_hr) m.max_hr = hr;

        if (sp < 90.0) below_90++;
        if (sp < 88.0) below_88++;
    }

    if (valid_spo2.empty()) return m;

    m.avg_spo2 = std::accumulate(valid_spo2.begin(), valid_spo2.end(), 0.0) / valid_spo2.size();
    m.avg_hr = std::accumulate(valid_hr.begin(), valid_hr.end(), 0.0) / valid_hr.size();
    m.time_below_90_pct = 100.0 * below_90 / m.valid_samples;
    m.time_below_88_pct = 100.0 * below_88 / m.valid_samples;
    m.recording_hours = m.valid_samples * sample_interval / 3600.0;

    // Baseline (95th percentile)
    std::vector<double> sorted_spo2 = valid_spo2;
    std::sort(sorted_spo2.begin(), sorted_spo2.end());
    size_t p95_idx = static_cast<size_t>(sorted_spo2.size() * 0.95);
    if (p95_idx >= sorted_spo2.size()) p95_idx = sorted_spo2.size() - 1;
    m.spo2_baseline = sorted_spo2[p95_idx];

    // ODI: count 3% desaturation events
    // A desaturation = SpO2 drops >= 3% from a rolling 120s baseline
    size_t window = static_cast<size_t>(120.0 / sample_interval);
    if (window < 2) window = 2;
    bool in_desat = false;

    for (size_t i = 0; i < samples.size(); i++) {
        if (!samples[i].valid()) continue;

        // Compute local baseline: max SpO2 in preceding window
        double local_base = samples[i].spo2;
        size_t start = (i > window) ? i - window : 0;
        for (size_t j = start; j < i; j++) {
            if (samples[j].valid() && samples[j].spo2 > local_base)
                local_base = samples[j].spo2;
        }

        double drop = local_base - samples[i].spo2;
        if (drop >= 3.0 && !in_desat) {
            m.desat_count_3pct++;
            in_desat = true;
        } else if (drop < 1.0) {
            in_desat = false;
        }
    }

    if (m.recording_hours > 0)
        m.odi_3pct = m.desat_count_3pct / m.recording_hours;

    return m;
}

} // namespace cpapdash::parser
