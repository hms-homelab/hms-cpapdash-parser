#include <gtest/gtest.h>
#include "cpapdash/parser/O2RingSParser.h"

#include <array>
#include <chrono>
#include <vector>

using namespace cpapdash::parser;

// A synthetic O2Ring-S recording in the documented layout: the 10-byte header,
// 3-byte records, and (when finalised) a 48-byte trailer whose sub-magic sits at
// size - 44. Built here rather than copied from a ring: a real recording is
// somebody's night.
static std::vector<uint8_t> make_o2s(const std::vector<std::array<uint8_t, 3>>& records,
                                     bool finalised = true) {
    std::vector<uint8_t> b = {0x01, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00};
    for (const auto& r : records) b.insert(b.end(), r.begin(), r.end());
    if (finalised) {
        std::vector<uint8_t> t(48, 0);
        t[0] = 0x11; t[1] = 0x22; t[2] = 0x33;            // per-recording bytes
        t[4] = 0x48; t[5] = 0x12; t[6] = 0x5a; t[7] = 0xda;
        t[16] = 0x01; t[17] = 0x01; t[18] = 0x03;         // format stamp
        t[34] = 96; t[35] = 91; t[47] = 60;               // avg / min SpO2, avg HR
        b.insert(b.end(), t.begin(), t.end());
    }
    return b;
}

static std::chrono::system_clock::time_point utc(int y, int mo, int d, int h, int mi, int s) {
    std::tm tm{};
    tm.tm_year = y - 1900; tm.tm_mon = mo - 1; tm.tm_mday = d;
    tm.tm_hour = h; tm.tm_min = mi; tm.tm_sec = s;
    return std::chrono::system_clock::from_time_t(timegm(&tm));
}

TEST(O2RingS, AFinalisedRecordingKeepsItsTrailerOutOfTheSamples) {
    const auto b = make_o2s({{96, 58, 0}, {95, 59, 0}, {94, 60, 0}});
    ASSERT_TRUE(O2RingSParser::isComplete(b.data(), b.size()));
    const auto s = O2RingSParser::parse(b.data(), b.size(), "20261004221500.o2s");
    ASSERT_TRUE(s.has_value());
    ASSERT_EQ(s->samples.size(), 3u) << "48 trailer bytes would be 16 bogus samples";
    EXPECT_EQ(s->samples[0].spo2, 96);
    EXPECT_EQ(s->samples[2].heart_rate, 60);
    EXPECT_EQ(s->filename, "20261004221500.o2s");
}

TEST(O2RingS, TheStartIsTheNameAndTheRateIsOnePerSecond) {
    const auto b = make_o2s({{96, 58, 0}, {95, 59, 0}, {94, 60, 0}});
    const auto s = O2RingSParser::parse(b.data(), b.size(), "/data/rings/20261004221500.o2s");
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(s->start_time, utc(2026, 10, 4, 22, 15, 0)) << "the ring's digits, no zone applied";
    EXPECT_EQ(s->samples[2].timestamp, utc(2026, 10, 4, 22, 15, 2));
    EXPECT_DOUBLE_EQ(s->sample_interval, 1.0);
    EXPECT_EQ(s->duration_seconds, 3);
    EXPECT_EQ(s->end_time, utc(2026, 10, 4, 22, 15, 3));
    EXPECT_EQ(s->date_str(), "20261004");
    EXPECT_EQ(s->filename, "20261004221500.o2s") << "the base name, not the path";
}

TEST(O2RingS, ANameWithoutExtensionIsTheRingsOwnName) {
    const auto b = make_o2s({{96, 58, 0}});
    EXPECT_TRUE(O2RingSParser::parse(b.data(), b.size(), "20261004221500").has_value());
}

TEST(O2RingS, NoStartTimeInTheNameIsNoRecording) {
    const auto b = make_o2s({{96, 58, 0}});
    EXPECT_FALSE(O2RingSParser::parse(b.data(), b.size(), "").has_value());
    EXPECT_FALSE(O2RingSParser::parse(b.data(), b.size(), "ring.o2s").has_value());
    EXPECT_FALSE(O2RingSParser::parse(b.data(), b.size(), "2026100422150.o2s").has_value())
        << "13 digits";
    EXPECT_FALSE(O2RingSParser::parse(b.data(), b.size(), "202610042215001.o2s").has_value())
        << "15 digits";
    EXPECT_FALSE(O2RingSParser::parse(b.data(), b.size(), "20261304221500.o2s").has_value())
        << "month 13";
}

TEST(O2RingS, TheRingsNoReadingValuesAreTheOneInvalidSentinel) {
    const auto b = make_o2s({{0, 58, 0}, {96, 0, 0}, {96, 255, 0}, {96, 58, 0x02}, {97, 61, 0}});
    const auto s = O2RingSParser::parse(b.data(), b.size(), "20261004221500.o2s");
    ASSERT_TRUE(s.has_value());
    EXPECT_FALSE(s->samples[0].valid()) << "spo2 0";
    EXPECT_FALSE(s->samples[1].valid()) << "hr 0";
    EXPECT_FALSE(s->samples[2].valid()) << "hr 255, no finger contact";
    EXPECT_FALSE(s->samples[3].valid()) << "flags set: suspect";
    EXPECT_TRUE(s->samples[4].valid());
    EXPECT_EQ(s->metrics.valid_samples, 1);
    EXPECT_EQ(s->metrics.total_samples, 5);
    EXPECT_DOUBLE_EQ(s->metrics.avg_spo2, 97.0);
}

TEST(O2RingS, AnUnfinishedRecordingParsesButIsNotComplete) {
    // Full of records, no trailer yet: the ring is still writing it.
    const auto b = make_o2s({{96, 58, 0}, {95, 59, 0}}, /*finalised=*/false);
    EXPECT_FALSE(O2RingSParser::isComplete(b.data(), b.size()));
    const auto s = O2RingSParser::parse(b.data(), b.size(), "20261004221500.o2s");
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(s->samples.size(), 2u);
}

TEST(O2RingS, MetricsScaleWithOneSecondSamples) {
    std::vector<std::array<uint8_t, 3>> r(3600, {96, 60, 0});
    const auto b = make_o2s(r);
    const auto s = O2RingSParser::parse(b.data(), b.size(), "20261004221500.o2s");
    ASSERT_TRUE(s.has_value());
    EXPECT_NEAR(s->metrics.recording_hours, 1.0, 1e-9) << "an hour of 1 s samples is an hour";
    EXPECT_EQ(s->duration_seconds, 3600);
}

TEST(O2RingS, ShortOrForeignBytesAreRefused) {
    const std::vector<uint8_t> header_only = {0x01, 0x03, 0, 0, 0, 0, 0, 0, 0x04, 0x00};
    EXPECT_FALSE(O2RingSParser::parse(header_only.data(), header_only.size(),
                                      "20261004221500").has_value());
    EXPECT_FALSE(O2RingSParser::looksLike(nullptr, 0));
    std::vector<uint8_t> wrong = make_o2s({{96, 58, 0}});
    wrong[8] = 0x05;
    EXPECT_FALSE(O2RingSParser::looksLike(wrong.data(), wrong.size()));
    EXPECT_FALSE(O2RingSParser::parse(wrong.data(), wrong.size(), "20261004221500").has_value());
}

TEST(OximetryFile, TheFormatIsReadFromTheBytesNotTheName) {
    const auto o2s = make_o2s({{96, 58, 0}});
    EXPECT_EQ(detectOximetryFormat(o2s.data(), o2s.size()), OximetryFormat::O2RingS);

    std::vector<uint8_t> vld(40 + 5, 0);
    vld[0] = 3;                                         // .vld v3
    vld[2] = 2026 & 0xFF; vld[3] = 2026 >> 8;
    vld[4] = 10; vld[5] = 4; vld[6] = 22; vld[7] = 15;
    vld[22] = 4;                                        // 4 s interval
    vld[40] = 96; vld[41] = 58;
    EXPECT_EQ(detectOximetryFormat(vld.data(), vld.size()), OximetryFormat::Vld3);

    const std::vector<uint8_t> photo = {0xFF, 0xD8, 0xFF, 0xE0};
    EXPECT_EQ(detectOximetryFormat(photo.data(), photo.size()), OximetryFormat::Unknown);
    EXPECT_FALSE(parseOximetryFile(photo.data(), photo.size(), "20261004221500").has_value());

    // A .vld named like an O2Ring-S file is still read as a .vld, and the reverse.
    const auto as_vld = parseOximetryFile(vld.data(), vld.size(), "20261004221500.o2s");
    ASSERT_TRUE(as_vld.has_value());
    EXPECT_DOUBLE_EQ(as_vld->sample_interval, 4.0);
    const auto as_o2s = parseOximetryFile(o2s.data(), o2s.size(), "20261004221500.vld");
    ASSERT_TRUE(as_o2s.has_value());
    EXPECT_DOUBLE_EQ(as_o2s->sample_interval, 1.0);
}
