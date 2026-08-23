#include <gtest/gtest.h>
#include "cpapdash/parser/VLDParser.h"
#include <cstring>
#include <vector>

using namespace cpapdash::parser;

// Builds a header in the REAL Wellue layout:
//   offset  9  u32  file size
//   offset 13  u32  duration in seconds
//   offset 22  u16  sample interval in seconds
//
// This used to write the duration to offset 18 and nothing else, which is where the
// parser used to read it from. Both were wrong, and because the fixture wrote what the
// parser read, the tests agreed with the bug instead of catching it. Offset 18 is now
// deliberately filled with a DECOY so that any future reliance on it fails loudly.
static std::vector<uint8_t> make_vld(uint16_t year, uint8_t month, uint8_t day,
                                      uint8_t hour, uint8_t min, uint8_t sec,
                                      uint32_t duration_s,
                                      const std::vector<std::array<uint8_t, 5>>& records,
                                      uint16_t interval_s = 4,
                                      uint16_t decoy_at_18 = 0xBEEF)
{
    std::vector<uint8_t> buf(40 + records.size() * 5, 0);
    auto put_u16 = [&](size_t off, uint16_t v) {
        buf[off] = v & 0xFF; buf[off + 1] = (v >> 8) & 0xFF;
    };
    auto put_u32 = [&](size_t off, uint32_t v) {
        buf[off] = v & 0xFF;         buf[off + 1] = (v >> 8) & 0xFF;
        buf[off + 2] = (v >> 16) & 0xFF; buf[off + 3] = (v >> 24) & 0xFF;
    };

    put_u16(0, 3);                                  // version
    buf[2] = year & 0xFF; buf[3] = (year >> 8) & 0xFF;
    buf[4] = month; buf[5] = day;
    buf[6] = hour; buf[7] = min; buf[8] = sec;

    put_u32(9,  static_cast<uint32_t>(buf.size())); // file size
    put_u32(13, duration_s);                        // duration
    put_u16(18, decoy_at_18);                       // NOT the duration
    put_u16(22, interval_s);                        // sample interval

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
    // 4 records at 4s = 16s. Duration and interval both come from the header now,
    // NOT from dividing one by the record count.
    auto data = make_vld(2026, 4, 12, 6, 53, 7, 16, {
        {96, 85, 0, 1, 0},
        {97, 86, 0, 0, 0},
        {95, 88, 0, 2, 0},
        {96, 84, 0, 0, 0},
    });

    auto r = VLDParser::parse(data.data(), data.size(), "20260412065307.vld");
    ASSERT_TRUE(r.has_value());

    auto& s = *r;
    EXPECT_EQ(s.filename, "20260412065307.vld");
    EXPECT_EQ(s.duration_seconds, 16);
    EXPECT_EQ(s.samples.size(), 4u);
    EXPECT_DOUBLE_EQ(s.sample_interval, 4.0);
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

// ── Header offsets: the defect a real ring file exposed ───────────────────────
//
// Found 2026-08-23 on the first REAL Wellue export we have ever had (a customer's
// overnight recording, kept out of the repo — it is a real person's health data).
// The parser read a u16 at offset 18 as the duration and divided by the record
// count to get the interval. On that file: 7591 records, a true 4s interval and a
// true 30364s duration, but offset 18 held 3673, giving an interval of 0.4839s.
// Nothing samples at 0.48s. The recording came out as 1.02h instead of 8.43h, and
// ODI as 43.1/h instead of 9.13/h — severe instead of mild. The CPAP session for
// the same night recorded 8.20 therapy hours, which is what caught it.
//
// The old fixtures could not have caught this: make_vld wrote offset 18 and the
// parser read offset 18, so the fixture agreed with the bug.

TEST(VLDParser, DurationAndIntervalComeFromTheHeaderNotFromDivision) {
    // 7591 records at 4s = 30364s, the real file's shape. If the parser divided
    // instead, a decoy at offset 18 would produce a nonsense sub-second interval.
    std::vector<std::array<uint8_t, 5>> recs(7591, {96, 70, 0, 0, 0});
    auto data = make_vld(2026, 8, 18, 16, 53, 19, 30364, recs,
                         /*interval_s=*/4, /*decoy_at_18=*/3673);

    auto r = VLDParser::parse(data.data(), data.size());
    ASSERT_TRUE(r.has_value());
    EXPECT_DOUBLE_EQ(r->sample_interval, 4.0);
    EXPECT_EQ(r->duration_seconds, 30364);
    EXPECT_EQ(r->samples.size(), 7591u);
    EXPECT_NEAR(r->metrics.recording_hours, 8.43, 0.01);
}

TEST(VLDParser, OffsetEighteenIsIgnoredWhateverItHolds) {
    // The decoy must not be able to move anything. Same file, wildly different
    // values at offset 18, identical results.
    std::vector<std::array<uint8_t, 5>> recs(100, {96, 70, 0, 0, 0});
    auto a = make_vld(2026, 8, 18, 16, 53, 19, 400, recs, 4, 0x0000);
    auto b = make_vld(2026, 8, 18, 16, 53, 19, 400, recs, 4, 0xFFFF);

    auto ra = VLDParser::parse(a.data(), a.size());
    auto rb = VLDParser::parse(b.data(), b.size());
    ASSERT_TRUE(ra.has_value());
    ASSERT_TRUE(rb.has_value());
    EXPECT_DOUBLE_EQ(ra->sample_interval, rb->sample_interval);
    EXPECT_EQ(ra->duration_seconds, rb->duration_seconds);
    EXPECT_DOUBLE_EQ(ra->sample_interval, 4.0);
}

TEST(VLDParser, AnImplausibleIntervalIsRefusedRatherThanPropagated) {
    // Every metric that scales with the interval inherits it — duration,
    // recording_hours, time_below_90/88, ODI, and the 120s smoothing window in
    // calculateMetrics. A rate no ring produces must not reach any of them.
    std::vector<std::array<uint8_t, 5>> recs(100, {96, 70, 0, 0, 0});

    auto tiny = make_vld(2026, 8, 18, 16, 53, 19, 400, recs, /*interval_s=*/0);
    auto rt = VLDParser::parse(tiny.data(), tiny.size());
    ASSERT_TRUE(rt.has_value());
    EXPECT_GE(rt->sample_interval, 1.0);
    EXPECT_LE(rt->sample_interval, 10.0);

    auto huge = make_vld(2026, 8, 18, 16, 53, 19, 400, recs, /*interval_s=*/3600);
    auto rh = VLDParser::parse(huge.data(), huge.size());
    ASSERT_TRUE(rh.has_value());
    EXPECT_GE(rh->sample_interval, 1.0);
    EXPECT_LE(rh->sample_interval, 10.0);
}

TEST(VLDParser, SamplesWinWhenTheDurationDisagreesWithThem) {
    // The samples are what gets plotted. A duration that contradicts them silently
    // distorts every rate computed against it, so it is corrected, not trusted.
    std::vector<std::array<uint8_t, 5>> recs(100, {96, 70, 0, 0, 0});
    auto data = make_vld(2026, 8, 18, 16, 53, 19, /*duration_s=*/99999, recs, 4);

    auto r = VLDParser::parse(data.data(), data.size());
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->duration_seconds, 400);   // 100 records * 4s, not the header's claim
}

TEST(VLDParser, HeaderClockIsTakenVerbatimAsAWallClock) {
    // The ring records a wall clock and no zone. Confirmed against a real file whose
    // header read 2026-08-18 16:53:19 and whose owner's CPAP session that night
    // started 16:51:28 local — 1m51s apart. Interpreting those digits as UTC would
    // have put the recording five hours before the machine was switched on.
    //
    // The parser therefore carries the digits through unchanged (timegm in, gmtime
    // out is lossless) and leaves the zone to the caller. hms-cpap wants them as-is;
    // the cloud interprets them in the patient's zone. Do NOT "fix" this to UTC.
    std::vector<std::array<uint8_t, 5>> recs(10, {96, 70, 0, 0, 0});
    auto data = make_vld(2026, 8, 18, 16, 53, 19, 40, recs, 4);

    auto r = VLDParser::parse(data.data(), data.size());
    ASSERT_TRUE(r.has_value());
    auto tt = std::chrono::system_clock::to_time_t(r->start_time);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &tt);
#else
    gmtime_r(&tt, &tm);
#endif
    EXPECT_EQ(tm.tm_year + 1900, 2026);
    EXPECT_EQ(tm.tm_mon + 1, 8);
    EXPECT_EQ(tm.tm_mday, 18);
    EXPECT_EQ(tm.tm_hour, 16);
    EXPECT_EQ(tm.tm_min, 53);
    EXPECT_EQ(tm.tm_sec, 19);
}
