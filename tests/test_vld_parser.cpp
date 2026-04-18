#include <gtest/gtest.h>
#include "cpapdash/parser/VLDParser.h"
#include <cstring>
#include <vector>

using namespace cpapdash::parser;

static std::vector<uint8_t> make_vld(uint16_t year, uint8_t month, uint8_t day,
                                      uint8_t hour, uint8_t min, uint8_t sec,
                                      uint16_t duration_s,
                                      const std::vector<std::array<uint8_t, 5>>& records)
{
    std::vector<uint8_t> buf(40 + records.size() * 5, 0);

    // Version 3
    buf[0] = 3; buf[1] = 0;
    // Date
    buf[2] = year & 0xFF; buf[3] = (year >> 8) & 0xFF;
    buf[4] = month; buf[5] = day;
    buf[6] = hour; buf[7] = min; buf[8] = sec;
    // Duration at offset 18
    buf[18] = duration_s & 0xFF; buf[19] = (duration_s >> 8) & 0xFF;

    for (size_t i = 0; i < records.size(); i++) {
        memcpy(buf.data() + 40 + i * 5, records[i].data(), 5);
    }
    return buf;
}

TEST(VLDParser, RejectsShortData) {
    uint8_t buf[10] = {};
    auto r = VLDParser::parse(buf, sizeof(buf));
    EXPECT_FALSE(r.has_value());
}

TEST(VLDParser, RejectsWrongVersion) {
    auto data = make_vld(2026, 4, 12, 6, 53, 0, 40, {
        {96, 85, 0, 1, 0}, {97, 86, 0, 0, 0}
    });
    data[0] = 2; // wrong version
    auto r = VLDParser::parse(data.data(), data.size());
    EXPECT_FALSE(r.has_value());
}

TEST(VLDParser, ParsesHeaderCorrectly) {
    auto data = make_vld(2026, 4, 12, 6, 53, 7, 80, {
        {96, 85, 0, 1, 0},
        {97, 86, 0, 0, 0},
        {95, 88, 0, 2, 0},
        {96, 84, 0, 0, 0},
    });

    auto r = VLDParser::parse(data.data(), data.size(), "20260412065307.vld");
    ASSERT_TRUE(r.has_value());

    auto& s = *r;
    EXPECT_EQ(s.filename, "20260412065307.vld");
    EXPECT_EQ(s.duration_seconds, 80);
    EXPECT_EQ(s.samples.size(), 4u);
    EXPECT_DOUBLE_EQ(s.sample_interval, 20.0); // 80s / 4 records
    EXPECT_EQ(s.date_str(), "20260412");
}

TEST(VLDParser, ParsesSamplesCorrectly) {
    auto data = make_vld(2026, 4, 12, 22, 0, 0, 16, {
        {96, 72, 0, 5, 0},
        {0xFF, 0xFF, 1, 0, 0},  // invalid
        {94, 68, 0, 0, 1},      // vibration alert
        {98, 75, 0, 3, 0},
    });

    auto r = VLDParser::parse(data.data(), data.size());
    ASSERT_TRUE(r.has_value());
    auto& s = r->samples;

    EXPECT_EQ(s[0].spo2, 96);
    EXPECT_EQ(s[0].heart_rate, 72);
    EXPECT_EQ(s[0].motion, 5);
    EXPECT_TRUE(s[0].valid());

    EXPECT_EQ(s[1].spo2, 0xFF);
    EXPECT_FALSE(s[1].valid());

    EXPECT_EQ(s[2].vibration, 1);
    EXPECT_TRUE(s[2].valid());

    EXPECT_EQ(s[3].spo2, 98);
}

TEST(VLDParser, MetricsCalculation) {
    // 10 samples at 4s interval = 40s recording
    std::vector<OximetrySample> samples;
    auto base = std::chrono::system_clock::now();
    uint8_t spo2_vals[] = {96, 95, 94, 93, 92, 91, 90, 89, 88, 96};
    uint8_t hr_vals[]   = {70, 72, 74, 76, 78, 80, 75, 73, 71, 69};

    for (int i = 0; i < 10; i++) {
        OximetrySample s;
        s.timestamp = base + std::chrono::seconds(i * 4);
        s.spo2 = spo2_vals[i];
        s.heart_rate = hr_vals[i];
        s.invalid_flag = 0;
        s.motion = 0;
        s.vibration = 0;
        samples.push_back(s);
    }

    auto m = VLDParser::calculateMetrics(samples, 4.0);

    EXPECT_EQ(m.valid_samples, 10);
    EXPECT_EQ(m.total_samples, 10);
    EXPECT_DOUBLE_EQ(m.min_spo2, 88);
    EXPECT_DOUBLE_EQ(m.max_spo2, 96);
    EXPECT_EQ(m.min_hr, 69);
    EXPECT_EQ(m.max_hr, 80);
    EXPECT_GT(m.avg_spo2, 91.0);
    EXPECT_LT(m.avg_spo2, 93.0);
    EXPECT_GT(m.time_below_90_pct, 0.0);  // 88, 89 are below 90
    EXPECT_DOUBLE_EQ(m.time_below_88_pct, 0.0);  // 88 is NOT below 88 (strict <)
}

TEST(VLDParser, DateStr) {
    auto data = make_vld(2026, 12, 31, 23, 59, 0, 4, {
        {96, 72, 0, 0, 0},
    });
    auto r = VLDParser::parse(data.data(), data.size());
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->date_str(), "20261231");
}
