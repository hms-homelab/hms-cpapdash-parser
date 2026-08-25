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

    // session_end stays WALL CLOCK -- the charts need the real end of the night.
    // duration_seconds does not: it is therapy time, so the 58-minute mask-off
    // break between these two checkpoints is excluded. 3 + 2 minutes of data,
    // not the 62-minute envelope.
    //
    // This assertion used to read 62 * 60, deliberately, with a note that mask-on
    // time was "a separate figure". No such figure exists on a session -- the only
    // per-session duration is this one, and it is what hms-cpap shows. Support
    // ticket 87: a night of 1 + 114 + 107 minutes split by a 7h evening break was
    // reported as 11h22m instead of 3h42m, and AHI fell from 0.54 to 0.18 with it,
    // because AHI is events / duration. OSCAR, summing sessions, says 3h42m.
    EXPECT_EQ(s.duration_seconds, 5 * 60)
        << "duration must be therapy time, not first-start-to-last-end";
}

// The shape of ticket 87: several checkpoints with real breaks between them.
// The envelope and the therapy time differ by hours, and only one of them is
// a duration a patient would recognise.
TEST_F(BrpCheckpoints, DurationSumsCheckpointsAndExcludesTheGaps) {
    writeCheckpoint(dir_, "20260701_171500", "01.07.26", "17.15.00", 1);
    writeCheckpoint(dir_, "20260702_002400", "02.07.26", "00.24.00", 114);
    writeCheckpoint(dir_, "20260702_024900", "02.07.26", "02.49.00", 107);
    auto sp = parseDir();
    ASSERT_TRUE(sp != nullptr);
    const auto& s = *sp;

    EXPECT_EQ(s.duration_seconds, (1 + 114 + 107) * 60) << "sum of the data held";
    ASSERT_TRUE(s.session_end.has_value());
    EXPECT_EQ(hhmm(s.session_end.value()), "04:36:00") << "end is still wall clock";
}

// A live checkpoint grows between reads and a merge re-parses earlier ones.
// Both must leave the total correct rather than compounding it.
TEST_F(BrpCheckpoints, RereadingAndGrowthDoNotInflateDuration) {
    writeCheckpoint(dir_, "20260701_220000", "01.07.26", "22.00.00", 3);
    writeCheckpoint(dir_, "20260701_230000", "01.07.26", "23.00.00", 2);
    ASSERT_EQ(parseDir()->duration_seconds, 5 * 60);

    // Same directory parsed again: every checkpoint is re-read.
    EXPECT_EQ(parseDir()->duration_seconds, 5 * 60) << "re-parse must be idempotent";

    // The later checkpoint grows, as a live file does; the total follows it up
    // by exactly the new data rather than adding a second copy of the file.
    writeCheckpoint(dir_, "20260701_230000", "01.07.26", "23.00.00", 9);
    EXPECT_EQ(parseDir()->duration_seconds, (3 + 9) * 60) << "growth replaces, not adds";
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

// ── PLD claims the NEAREST BRP minute, not the first one in tolerance ───────
//
// A machine writes BRP and PLD a second or two apart, so with several
// checkpoints in a night more than one BRP row can sit inside the 30s window.
// Taking the first one claimed the wrong row and left the right one empty, and
// coverage came out alternating: on one real night 119 of 863 minutes had no
// leak or respiratory rate while flow, which comes from BRP, ran unbroken. The
// spacing between holes was ALWAYS even — 2, 4, 6, 8, never odd — which is what
// alternating claims look like, and what absent data does not. Ticket 67.
//
// Written against the public parseSession, so it exercises the real BRP+PLD
// interaction rather than the matcher in isolation.

// A PLD-shaped EDF: the ten ResMed 0.5 Hz channels, one record per minute.
std::vector<uint8_t> buildPLD(const std::string& ddmmyy, const std::string& hhmmss,
                              int num_records) {
    const std::vector<std::string> labels = {
        "MaskPress.2s", "Press.2s", "EprPress.2s", "Leak.2s", "RespRate.2s",
        "TidVol.2s", "MinVent.2s", "Snore.2s", "FlowLim.2s", "Crc16"};
    const int ns = static_cast<int>(labels.size());
    const int per_rec = 30;                       // 0.5 Hz x 60 s
    const int header_bytes = 256 + 256 * ns;
    const int rec_vals = per_rec * (ns - 1) + 1;  // Crc16 carries one
    const int total = header_bytes + num_records * rec_vals * 2;

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
    field(244, 8, "60");
    field(252, 4, std::to_string(ns));

    int base = 256;
    for (int i = 0; i < ns; ++i) field(base + i * 16, 16, labels[i]);
    base += ns * 16;
    auto block = [&](int width, const std::string& v) {
        for (int i = 0; i < ns; ++i) field(base + i * width, width, v);
        base += ns * width;
    };
    block(80, "");                       // transducer
    block(8, "L/s");                     // dimension
    block(8, "0"); block(8, "2");        // physical min/max
    block(8, "0"); block(8, "100");      // digital min/max
    block(80, "");                       // prefilter
    for (int i = 0; i < ns; ++i)
        field(base + i * 8, 8, std::to_string(i == ns - 1 ? 1 : per_rec));
    base += ns * 8;
    block(32, "");

    for (int i = 0; i < num_records * rec_vals; ++i) {
        int16_t v = static_cast<int16_t>(20 + (i % 5));
        buf[header_bytes + i * 2]     = static_cast<uint8_t>(v & 0xFF);
        buf[header_bytes + i * 2 + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    }
    return buf;
}

TEST_F(BrpCheckpoints, EveryBrpMinuteGetsItsPldValues) {
    // Two checkpoints, PLD written a second after BRP the way a machine does it.
    writeCheckpoint(dir_, "20260701_220000", "01.07.26", "22.00.00", 4);
    {
        const std::string p = (dir_ / "20260701_220001_PLD.edf").string();
        auto buf = buildPLD("01.07.26", "22.00.01", 4);
        std::ofstream f(p, std::ios::binary);
        f.write(reinterpret_cast<const char*>(buf.data()),
                static_cast<std::streamsize>(buf.size()));
    }
    writeCheckpoint(dir_, "20260701_221000", "01.07.26", "22.10.00", 3);
    {
        const std::string p = (dir_ / "20260701_221001_PLD.edf").string();
        auto buf = buildPLD("01.07.26", "22.10.01", 3);
        std::ofstream f(p, std::ios::binary);
        f.write(reinterpret_cast<const char*>(buf.data()),
                static_cast<std::streamsize>(buf.size()));
    }

    auto sp = parseDir();
    ASSERT_TRUE(sp != nullptr);

    // Every minute that has flow must also have carried its PLD values across.
    // A first-match matcher leaves some of them empty, in an even-spaced pattern.
    int with_flow = 0, with_leak = 0;
    for (const auto& b : sp->breathing_summary) {
        if (b.avg_flow_rate != 0.0) ++with_flow;
        if (b.leak_rate.has_value()) ++with_leak;
    }
    EXPECT_GT(with_flow, 0);
    EXPECT_EQ(with_leak, static_cast<int>(sp->breathing_summary.size()))
        << "every minute should carry PLD values; missing ones are the alternating-claim bug";
}
