// A night is not one file. ResMed opens a fresh BRP checkpoint every few minutes
// and after any mask-off break, so a session routinely arrives as several files
// separated by real gaps.
//
// Every checkpoint used to be anchored to session_start, which stacked them all
// from the same instant: the break disappeared because each segment restarted at
// the beginning, and the series ended early by the total of the gaps it had
// swallowed. That is hms-cpap issue #21 — a 2:20am bathroom break that never
// appeared and a flow chart that stopped around 4am while the summary metrics,
// which come from STR, covered the whole night.
//
// Reproduced on real data before these were written: a card night with six BRP
// files, four of them header-only, put 70 minutes of genuine flow at the empty
// first checkpoint's timestamp — 2m21s early and a minute short.
#include <gtest/gtest.h>

#include "cpapdash/parser/EDFParser.h"
#include "cpapdash/parser/EDFFile.h"
#include "cpapdash/parser/Models.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace cpapdash::parser;
namespace fs = std::filesystem;

namespace {

// A BRP-shaped EDF: one "Flow.40ms" signal at 25 Hz, one record per minute.
std::vector<uint8_t> buildBRP(const std::string& ddmmyy, const std::string& hhmmss,
                              int num_records) {
    const int ns = 1;
    const int samples_per_record = 1500;          // 25 Hz x 60 s
    const int header_bytes = 256 + 256 * ns;
    const int total = header_bytes + num_records * samples_per_record * 2;

    std::vector<uint8_t> buf(total, ' ');
    auto field = [&](int off, int len, const std::string& v) {
        for (int i = 0; i < len; ++i)
            buf[off + i] = (i < static_cast<int>(v.size())) ? v[i] : ' ';
    };

    field(0, 8, "0");
    field(8, 80, "TestPatient");
    field(88, 80, "Startdate");
    field(168, 8, ddmmyy);
    field(176, 8, hhmmss);
    field(184, 8, std::to_string(header_bytes));
    field(192, 44, "");
    field(236, 8, std::to_string(num_records));
    field(244, 8, "60");                          // one record = 60 s
    field(252, 4, std::to_string(ns));

    int base = 256;
    auto block = [&](int width, const std::string& v) {
        for (int i = 0; i < ns; ++i) field(base + i * width, width, v);
        base += ns * width;
    };
    block(16, "Flow.40ms");
    block(80, "");
    block(8, "L/s");
    block(8, "-10");
    block(8, "10");
    block(8, "-32768");
    block(8, "32767");
    block(80, "");
    block(8, std::to_string(samples_per_record));
    block(32, "");

    // Non-zero samples so the flow series is not empty.
    for (int i = 0; i < num_records * samples_per_record; ++i) {
        int16_t v = static_cast<int16_t>((i % 200) - 100);
        buf[header_bytes + i * 2]     = static_cast<uint8_t>(v & 0xFF);
        buf[header_bytes + i * 2 + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    }
    return buf;
}

// Written under a real ResMed checkpoint name, because the name is the clock the
// parser trusts.
std::string writeCheckpoint(const fs::path& dir, const std::string& stamp,
                            const std::string& ddmmyy, const std::string& hhmmss,
                            int records) {
    const std::string path = (dir / (stamp + "_BRP.edf")).string();
    auto buf = buildBRP(ddmmyy, hhmmss, records);
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(buf.data()),
            static_cast<std::streamsize>(buf.size()));
    return path;
}

std::string hhmm(std::chrono::system_clock::time_point tp) {
    std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
    // Local, because the parser stores wall-clock local time (EDFFile::getStartTime
    // and the filename anchor both go through mktime).
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char out[16];
    std::snprintf(out, sizeof(out), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    return out;
}

class BrpCheckpoints : public ::testing::Test {
protected:
    fs::path dir_;
    void SetUp() override {
        dir_ = fs::temp_directory_path() / "cpapdash_brp_checkpoints";
        fs::remove_all(dir_);
        fs::create_directories(dir_);
    }
    void TearDown() override { fs::remove_all(dir_); }

    // Through the public entry point, the same one hms-cpap calls on a staged
    // session directory.
    std::unique_ptr<ParsedSession> parseDir() {
        return EDFParser::parseSession(dir_.string(), "dev-test", "Test Machine");
    }
};

} // namespace

// The headline case. Two checkpoints with a 57-minute break between them: the
// second must land at its OWN time, not continue from the first.
TEST_F(BrpCheckpoints, EachCheckpointKeepsItsOwnClock) {
    // 22:00:00 + 3 min, then a break, then 23:00:00 + 2 min.
    writeCheckpoint(dir_, "20260701_220000", "01.07.26", "22.00.00", 3);
    writeCheckpoint(dir_, "20260701_230000", "01.07.26", "23.00.00", 2);
    auto sp = parseDir();
    ASSERT_TRUE(sp != nullptr);
    const auto& s = *sp;

    ASSERT_EQ(s.breathing_summary.size(), 5u);

    EXPECT_EQ(hhmm(s.breathing_summary[0].timestamp), "22:00:00");
    EXPECT_EQ(hhmm(s.breathing_summary[1].timestamp), "22:01:00");
    EXPECT_EQ(hhmm(s.breathing_summary[2].timestamp), "22:02:00");
    // The ones that used to be wrong: stacked onto the first checkpoint as
    // 22:03:00 and 22:04:00, which erased the break and ended the night early.
    EXPECT_EQ(hhmm(s.breathing_summary[3].timestamp), "23:00:00");
    EXPECT_EQ(hhmm(s.breathing_summary[4].timestamp), "23:01:00");
}

// The gap has to survive as a gap, or no chart can draw the break.
TEST_F(BrpCheckpoints, TheBreakBetweenCheckpointsIsPreserved) {
    writeCheckpoint(dir_, "20260701_220000", "01.07.26", "22.00.00", 3);
    writeCheckpoint(dir_, "20260701_230000", "01.07.26", "23.00.00", 2);
    auto sp = parseDir();
    ASSERT_TRUE(sp != nullptr);
    const auto& s = *sp;

    ASSERT_EQ(s.breathing_summary.size(), 5u);
    const auto gap = std::chrono::duration_cast<std::chrono::minutes>(
        s.breathing_summary[3].timestamp - s.breathing_summary[2].timestamp);
    EXPECT_EQ(gap.count(), 58) << "mask-off break must remain visible in the series";
}

// session_end is the latest end seen, not whichever file happened to be parsed last.
TEST_F(BrpCheckpoints, SessionEndIsTheLatestCheckpointEnd) {
    writeCheckpoint(dir_, "20260701_220000", "01.07.26", "22.00.00", 3);
    writeCheckpoint(dir_, "20260701_230000", "01.07.26", "23.00.00", 2);
    auto sp = parseDir();
    ASSERT_TRUE(sp != nullptr);
    const auto& s = *sp;

    ASSERT_TRUE(s.session_end.has_value());
    EXPECT_EQ(hhmm(s.session_end.value()), "23:02:00");
    EXPECT_EQ(hhmm(s.session_start.value()), "22:00:00");
    // 22:00 -> 23:02 wall clock, gap included. Mask-on time is a separate figure.
    EXPECT_EQ(s.duration_seconds, 62 * 60);
}

// Re-parsing an earlier checkpoint after a later one must not drag the end backwards.
TEST_F(BrpCheckpoints, AnEarlierCheckpointDoesNotPullTheEndBackwards) {
    writeCheckpoint(dir_, "20260701_230000", "01.07.26", "23.00.00", 2);
    writeCheckpoint(dir_, "20260701_220000", "01.07.26", "22.00.00", 3);
    auto sp = parseDir();
    ASSERT_TRUE(sp != nullptr);

    ASSERT_TRUE(sp->session_end.has_value());
    EXPECT_EQ(hhmm(sp->session_end.value()), "23:02:00");
}

// The filename is the clock, not the header. An AirSense 10 was observed writing
// a header a full week behind the name it gave the same file; anchoring to that
// drops a night's data a week away from the session it belongs to.
TEST_F(BrpCheckpoints, FilenameWinsOverAContradictoryHeader) {
    // Name says 01 July 22:00, header says 24 June 19:53.
    writeCheckpoint(dir_, "20260701_220000", "24.06.26", "19.53.39", 2);
    auto sp = parseDir();
    ASSERT_TRUE(sp != nullptr);
    const auto& s = *sp;

    ASSERT_EQ(s.breathing_summary.size(), 2u);
    EXPECT_EQ(hhmm(s.breathing_summary[0].timestamp), "22:00:00");
    EXPECT_EQ(hhmm(s.breathing_summary[1].timestamp), "22:01:00");
}

// Buffer-mode reads have no name to consult, so the header is still the fallback.
TEST_F(BrpCheckpoints, HeaderIsUsedWhenThereIsNoFilename) {
    auto buf = buildBRP("01.07.26", "22.00.00", 2);
    auto sp = EDFParser::parseSessionFromBuffers(
        buf.data(), buf.size(), nullptr, 0, nullptr, 0, nullptr, 0,
        "dev-test", "Test Machine");
    ASSERT_TRUE(sp != nullptr);
    ASSERT_EQ(sp->breathing_summary.size(), 2u);
    EXPECT_EQ(hhmm(sp->breathing_summary[0].timestamp), "22:00:00");
}
