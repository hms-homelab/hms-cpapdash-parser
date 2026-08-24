// The one O2 Ring CSV reader (and its inverse, the writer).
//
// hms-cpap and hms-cpapdash-api each had their own reader for this format and
// the two drifted. These tests pin the union, and in particular the numeric-date
// resolution from ticket 84 -- the corner where the two copies disagreed and
// where getting it wrong silently files a whole night on the wrong day.

#include <gtest/gtest.h>

#include <cpapdash/parser/OximetryCsv.h>

#include <chrono>
#include <string>

using namespace cpapdash::parser;

namespace {

std::string utcStamp(std::chrono::system_clock::time_point tp) {
    std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}

const char* kMonthNameHeader = "Time,Oxygen Level,Pulse Rate,Motion\r\n";

}  // namespace

// ── dialects ────────────────────────────────────────────────────────────────

TEST(O2RingCsvRead, ReadsTheUnquotedMonthNameDialect) {
    std::string csv = std::string(kMonthNameHeader) +
        "06:53:07 Apr 12 2026,96,58,0\r\n"
        "06:53:11 Apr 12 2026,95,59,3\r\n";

    auto r = readO2RingCsv(csv);
    ASSERT_EQ(r.session.samples.size(), 2u);
    EXPECT_EQ(utcStamp(r.session.start_time), "2026-04-12 06:53:07");
    EXPECT_EQ(r.session.samples[0].spo2, 96);
    EXPECT_EQ(r.session.samples[0].heart_rate, 58);
    EXPECT_EQ(r.session.samples[1].motion, 3);
    EXPECT_DOUBLE_EQ(r.session.sample_interval, 4.0);
}

TEST(O2RingCsvRead, ReadsTheQuotedAmPmDialectIncludingItsComma) {
    // The comma sits INSIDE the quotes, so a naive split would tear the field in
    // two and the row would be dropped. 11:20:29PM must land at 23:20:29.
    std::string csv =
        "Time,SpO2(%),Pulse Rate(bpm),Motion,SpO2 Reminder,PR Reminder,\r\n"
        "\"11:20:29PM Jun 19, 2026\",89,60,0,0,0,\r\n"
        "\"11:20:33PM Jun 19, 2026\",90,61,0,0,0,\r\n";

    auto r = readO2RingCsv(csv);
    ASSERT_EQ(r.session.samples.size(), 2u);
    EXPECT_EQ(utcStamp(r.session.start_time), "2026-06-19 23:20:29");
    EXPECT_EQ(r.session.samples[0].spo2, 89);
}

TEST(O2RingCsvRead, MidnightAmIsHourZeroNotTwelve) {
    std::string csv =
        "Time,SpO2(%),Pulse Rate(bpm)\r\n"
        "\"12:00:30AM Jun 20, 2026\",95,60\r\n";

    auto r = readO2RingCsv(csv);
    ASSERT_EQ(r.session.samples.size(), 1u);
    EXPECT_EQ(utcStamp(r.session.start_time), "2026-06-20 00:00:30")
        << "12 AM is 00:xx; reading it as 12:xx moves the night half a day";
}

// ── ticket 84: which number is the day ──────────────────────────────────────

TEST(O2RingCsvDateOrder, AComponentAboveTwelveSettlesItWhateverTheLocale) {
    std::string csv =
        "Time,SpO2,HR\r\n"
        "07:53:04 23/08/2026,95,60\r\n";

    auto r = readO2RingCsv(csv);
    EXPECT_TRUE(r.date_order_day_first);
    EXPECT_EQ(utcStamp(r.session.start_time), "2026-08-23 07:53:04");
}

TEST(O2RingCsvDateOrder, TheDayHintBeatsTheHeuristics) {
    // 05/08/2026 is genuinely ambiguous: the 5th of August, or the 8th of May.
    std::string csv =
        "Time,SpO2,HR\r\n"
        "23:10:00 05/08/2026,95,60\r\n";

    // Caller is filing this under 2026-05-08, so month-first is the reading that
    // agrees with the user.
    auto m = readO2RingCsv(csv, "", "20260508");
    EXPECT_FALSE(m.date_order_day_first);
    EXPECT_EQ(utcStamp(m.session.start_time), "2026-05-08 23:10:00");

    // Same bytes, the other night: the file must read the other way round.
    auto d = readO2RingCsv(csv, "", "20260805");
    EXPECT_TRUE(d.date_order_day_first);
    EXPECT_EQ(utcStamp(d.session.start_time), "2026-08-05 23:10:00");
}

TEST(O2RingCsvDateOrder, ConsecutiveDaysRevealWhichNumberIsMarching) {
    // Crossing midnight, one of the pair increments. That one is the day.
    std::string csv =
        "Time,SpO2,HR\r\n"
        "23:59:58 05/08/2026,95,60\r\n"
        "00:00:02 06/08/2026,95,61\r\n";

    auto r = readO2RingCsv(csv);
    EXPECT_TRUE(r.date_order_day_first);
    EXPECT_EQ(utcStamp(r.session.start_time), "2026-08-05 23:59:58");
}

TEST(O2RingCsvDateOrder, TheFilenameStampIsUsedWhenTheFileItselfIsSilent) {
    std::string csv =
        "Time,SpO2,HR\r\n"
        "23:10:00 05/08/2026,95,60\r\n";

    auto r = readO2RingCsv(csv, "WearO2 3719_20260508231000.csv");
    EXPECT_FALSE(r.date_order_day_first)
        << "the stamp says 2026-05-08, which only the month-first reading reproduces";
    EXPECT_EQ(utcStamp(r.session.start_time), "2026-05-08 23:10:00");
}

TEST(O2RingCsvDateOrder, AnAmPmClockImpliesAUsLocaleSoMonthFirst) {
    std::string csv =
        "Time,SpO2,HR\r\n"
        "\"11:10:00PM 05/08/2026\",95,60\r\n";

    auto r = readO2RingCsv(csv);
    EXPECT_FALSE(r.date_order_day_first);
    EXPECT_EQ(utcStamp(r.session.start_time), "2026-05-08 23:10:00");
}

TEST(O2RingCsvDateOrder, DayFirstIsTheFallbackForEveryOtherLocale) {
    std::string csv =
        "Time,SpO2,HR\r\n"
        "23:10:00 05/08/2026,95,60\r\n";

    auto r = readO2RingCsv(csv);
    EXPECT_TRUE(r.date_order_day_first);
    EXPECT_EQ(utcStamp(r.session.start_time), "2026-08-05 23:10:00");
}

TEST(O2RingCsvDateOrder, TheHintCannotOverrideAnUnambiguousDate) {
    // 23 can only be a day. A caller filing this under the wrong night must not
    // be able to talk the reader into an impossible month.
    std::string csv =
        "Time,SpO2,HR\r\n"
        "07:53:04 23/08/2026,95,60\r\n";

    auto r = readO2RingCsv(csv, "", "20260308");
    EXPECT_TRUE(r.date_order_day_first);
    EXPECT_EQ(utcStamp(r.session.start_time), "2026-08-23 07:53:04");
}

TEST(O2RingCsvDateOrder, AMonthNameFileIgnoresTheHintsEntirely) {
    std::string csv = std::string(kMonthNameHeader) +
        "06:53:07 Apr 12 2026,96,58,0\r\n";

    auto r = readO2RingCsv(csv, "whatever_20260101000000.csv", "20260101");
    EXPECT_EQ(utcStamp(r.session.start_time), "2026-04-12 06:53:07")
        << "the file states its month in words; no hint may second-guess it";
}

// ── samples, sentinels, intervals ───────────────────────────────────────────

TEST(O2RingCsvRead, SentinelReadingsAreMarkedInvalidNotStoredAsData) {
    // SpO2 255 / HR 65535 mean "no reading". Stored as a value they would drag
    // every average; 65535 into a uint8_t would also wrap to something plausible.
    std::string csv =
        "Time,SpO2,HR\r\n"
        "06:53:07 Apr 12 2026,96,58\r\n"
        "06:53:08 Apr 12 2026,255,65535\r\n";

    auto r = readO2RingCsv(csv);
    ASSERT_EQ(r.session.samples.size(), 2u);
    EXPECT_TRUE(r.session.samples[0].valid());
    EXPECT_FALSE(r.session.samples[1].valid());
    EXPECT_EQ(r.session.samples[1].spo2, 0xFF);
    EXPECT_EQ(r.session.samples[1].heart_rate, 0xFF);
    EXPECT_EQ(r.session.metrics.valid_samples, 1);
    EXPECT_DOUBLE_EQ(r.session.metrics.avg_spo2, 96.0)
        << "the sentinel must not enter the average";
}

TEST(O2RingCsvRead, TheIntervalIsTheSmallestGapNotTheFirst) {
    // A per-second export with one dropped row. Taking the first gap would call
    // this a 2-second recording and double the night's duration.
    std::string csv =
        "Time,SpO2,HR\r\n"
        "06:53:07 Apr 12 2026,96,58\r\n"
        "06:53:09 Apr 12 2026,96,58\r\n"
        "06:53:10 Apr 12 2026,96,58\r\n";

    auto r = readO2RingCsv(csv);
    EXPECT_DOUBLE_EQ(r.session.sample_interval, 1.0);
    EXPECT_EQ(r.session.duration_seconds, 3);
}

TEST(O2RingCsvRead, AnEmptyOrHeaderOnlyFileYieldsNothingRatherThanFailing) {
    EXPECT_TRUE(readO2RingCsv("").session.samples.empty());
    EXPECT_TRUE(readO2RingCsv(kMonthNameHeader).session.samples.empty());
    EXPECT_TRUE(readO2RingCsv("Time,SpO2,HR\r\nnot a row at all\r\n")
                    .session.samples.empty());
}

TEST(O2RingCsvRead, ShortRowsAreSkippedAndTheRestOfTheFileStillReads) {
    std::string csv =
        "Time,SpO2,HR\r\n"
        "06:53:07 Apr 12 2026,96\r\n"
        "06:53:08 Apr 12 2026,95,59\r\n";

    auto r = readO2RingCsv(csv);
    ASSERT_EQ(r.session.samples.size(), 1u);
    EXPECT_EQ(r.session.samples[0].spo2, 95);
}

// ── the reader and the writer are inverses ──────────────────────────────────

TEST(O2RingCsv, WriteThenReadReturnsTheSameNight) {
    // The clock a ring writes carries no zone. If the writer rendered local time
    // and the reader read UTC, this round trip would slide by the offset of
    // whatever machine ran it -- which is exactly how it broke the first time.
    std::string csv = std::string(kMonthNameHeader) +
        "06:53:07 Apr 12 2026,96,58,0\r\n"
        "06:53:11 Apr 12 2026,95,59,3\r\n"
        "06:53:15 Apr 12 2026,94,60,0\r\n";

    auto first = readO2RingCsv(csv);
    auto second = readO2RingCsv(writeO2RingCsv(first.session));

    ASSERT_EQ(second.session.samples.size(), first.session.samples.size());
    EXPECT_EQ(utcStamp(second.session.start_time), utcStamp(first.session.start_time));
    EXPECT_EQ(utcStamp(second.session.end_time), utcStamp(first.session.end_time));
    EXPECT_DOUBLE_EQ(second.session.sample_interval, first.session.sample_interval);
    for (size_t i = 0; i < first.session.samples.size(); ++i) {
        EXPECT_EQ(second.session.samples[i].spo2, first.session.samples[i].spo2) << "at " << i;
        EXPECT_EQ(second.session.samples[i].heart_rate,
                  first.session.samples[i].heart_rate) << "at " << i;
        EXPECT_EQ(utcStamp(second.session.samples[i].timestamp),
                  utcStamp(first.session.samples[i].timestamp)) << "at " << i;
    }
}

TEST(O2RingCsv, TheFilenameMatchesWhatTheRingsOwnAppWouldWrite) {
    std::string csv = std::string(kMonthNameHeader) +
        "23:20:29 Jun 19 2026,89,60,0\r\n";

    auto r = readO2RingCsv(csv);
    EXPECT_EQ(o2RingCsvFilename(r.session, "O2Ring S"), "O2Ring S_20260619232029.csv");
    EXPECT_EQ(o2RingCsvFilename(r.session), "O2Ring_20260619232029.csv");
}
