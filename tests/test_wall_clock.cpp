// SDD-013: the machine's wall clock as a label, on request; and the default,
// which must be exactly what every parser did before.
//
// Host zones are switched with TZ inside the test (a test green on one zone can
// be red on another), so the zone tests need a C library that takes IANA names:
// Linux and macOS, which is what CI runs. They skip on Windows.
#include <gtest/gtest.h>

#include "cpapdash/parser/EDFFile.h"
#include "cpapdash/parser/EDFParser.h"
#include "cpapdash/parser/EDFWriter.h"
#include "cpapdash/parser/WallClock.h"
#ifdef CPAPDASH_WITH_SEFAM
#include "cpapdash/parser/SefamParser.h"
#endif
#ifdef CPAPDASH_WITH_BMC
#include "cpapdash/parser/BmcParser.h"
#endif

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace cpapdash::parser;
using Clock = std::chrono::system_clock;

namespace {

// The host's zone for the life of the object, then the one before it.
class HostZone {
public:
    explicit HostZone(const char* tz) {
        const char* old = std::getenv("TZ");
        if (old) old_ = old;
#ifndef _WIN32
        setenv("TZ", tz, 1);
        tzset();
#endif
    }
    ~HostZone() {
#ifndef _WIN32
        if (old_) setenv("TZ", old_->c_str(), 1);
        else unsetenv("TZ");
        tzset();
#endif
    }

private:
    std::optional<std::string> old_;
};

#ifdef _WIN32
#define SKIP_WITHOUT_ZONES() GTEST_SKIP() << "the Windows CRT takes no IANA zone names"
#else
#define SKIP_WITHOUT_ZONES() (void)0
#endif

const char* const kZones[] = {"UTC", "America/New_York", "Australia/Sydney", "Pacific/Kiritimati"};

bool sameWall(const WallClock& a, const WallClock& b) {
    return a.year == b.year && a.month == b.month && a.day == b.day && a.hour == b.hour &&
           a.minute == b.minute && a.second == b.second;
}

std::string text(const WallClock& w) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d:%02d", w.year, w.month, w.day, w.hour,
                  w.minute, w.second);
    return buf;
}

WallClock hostWall(Clock::time_point tp) {
    const std::time_t t = Clock::to_time_t(tp);
    std::tm lt{};
#ifdef _WIN32
    localtime_s(&lt, &t);
#else
    localtime_r(&t, &lt);
#endif
    return WallClock{lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec};
}

// One ResMed night laid out the way a card holds it: a BRP, a PLD, a SAD and an
// EVE from `start`, `minutes` long, with events ENDING at `event_ends_s`.
struct Night {
    std::vector<uint8_t> brp, pld, sad, eve;
    std::string stem;
};

Night night(const WallClock& start, int minutes, const std::vector<double>& event_ends_s) {
    const std::string serial = "12345678901";  // invented
    Night n;
    StreamedBRP brp;
    brp.start = start;
    brp.serial = serial;
    const double pi = std::acos(-1.0);
    for (int i = 0; i < minutes * 60 * 25; ++i) {
        brp.flow_lpm.push_back(30.0 * std::sin(2 * pi * i / 100.0));
        brp.press_cmh2o.push_back(8.0);
    }
    n.brp = writeResMedBRP(brp);
    StreamedPLD pld;
    pld.start = start;
    pld.serial = serial;
    pld.press_cmh2o.assign(static_cast<size_t>(minutes * 30), 8.0);
    pld.leak_lps.assign(static_cast<size_t>(minutes * 30), 0.1);
    n.pld = writeResMedPLD(pld);
    StreamedSAD sad;
    sad.start = start;
    sad.serial = serial;
    sad.spo2_pct.assign(static_cast<size_t>(minutes * 60), 96.0);
    sad.pulse_bpm.assign(static_cast<size_t>(minutes * 60), 60.0);
    n.sad = writeResMedSAD(sad);
    ResMedAnnotationFile eve;
    eve.start = start;
    eve.serial = serial;
    for (double e : event_ends_s) eve.annotations.push_back({e, 10.0, "Obstructive Apnea"});
    n.eve = writeResMedAnnotations(eve);
    n.stem = resmedFileName(start, "BRP").substr(0, 15);
    return n;
}

std::unique_ptr<ParsedSession> parse(const Night& n) {
    EDFParser::SessionBuffers b;
    b.brp.push_back({n.brp.data(), n.brp.size()});
    b.pld.push_back({n.pld.data(), n.pld.size()});
    b.sad.push_back({n.sad.data(), n.sad.size()});
    b.eve.push_back({n.eve.data(), n.eve.size()});
    return EDFParser::parseSessionFromBuffers(b, "dev", "ResMed", n.stem);
}

// Every time point a session carries, in one list, in a fixed order.
std::vector<int64_t> timesOf(const ParsedSession& s) {
    auto us = [](Clock::time_point tp) {
        return static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(tp.time_since_epoch()).count());
    };
    std::vector<int64_t> out;
    if (s.session_start) out.push_back(us(*s.session_start));
    if (s.session_end) out.push_back(us(*s.session_end));
    for (const auto& [k, v] : s.brp_spans) out.push_back(us(k));
    for (const auto& e : s.events) out.push_back(us(e.timestamp));
    for (const auto& m : s.breathing_summary) out.push_back(us(m.timestamp));
    for (const auto& b : s.breaths) out.push_back(us(b.onset));
    for (const auto& v : s.vitals) out.push_back(us(v.timestamp));
    return out;
}

}  // namespace

// ── the label ────────────────────────────────────────────────────────────────

TEST(WallClockLabel, IsTheSixNumbersCountedAsUtc) {
    EXPECT_EQ(Clock::to_time_t(wallClockLabel({2026, 1, 1, 0, 0, 0})), 1767225600);
    EXPECT_EQ(wallClockLabel({2026, 3, 9, 0, 0, 0}) - wallClockLabel({2026, 3, 8, 0, 0, 0}),
              std::chrono::hours(24))
        << "a label has no daylight saving: every day is 24 h";
}

TEST(WallClockLabel, ReadsBackToItsSixNumbers) {
    for (const WallClock& w : {WallClock{2028, 2, 29, 23, 59, 59}, WallClock{2026, 3, 8, 2, 30, 0},
                               WallClock{1999, 12, 31, 0, 0, 1}}) {
        EXPECT_TRUE(sameWall(wallClockOf(wallClockLabel(w)), w)) << text(w);
    }
    EXPECT_TRUE(sameWall(wallClockOf(wallClockLabel({2026, 3, 8, 2, 30, 0}) +
                                     std::chrono::milliseconds(999)),
                         WallClock{2026, 3, 8, 2, 30, 0}))
        << "sub-seconds are dropped";
}

TEST(WallClockLabel, AnOverflowingDayNormalisesAsMktimeWould) {
    WallClockLabels labels;
    std::tm t{};
    t.tm_year = 2026 - 1900;
    t.tm_mon = 0;
    t.tm_mday = 32;  // the STR's day + offset relies on this
    t.tm_hour = 12;
    EXPECT_TRUE(sameWall(wallClockOf(Clock::from_time_t(wallClockToTimeT(t))),
                         WallClock{2026, 2, 1, 12, 0, 0}));
    t.tm_mon = 13;  // a month past December
    t.tm_mday = 1;
    EXPECT_TRUE(sameWall(wallClockOf(Clock::from_time_t(wallClockToTimeT(t))),
                         WallClock{2027, 2, 1, 12, 0, 0}));
}

// ── the frame ────────────────────────────────────────────────────────────────

TEST(ClockFrame, TheHostIsTheDefaultAndAScopeRestoresWhatWasThere) {
    EXPECT_EQ(clockFrame(), ClockFrame::HostLocal);
    {
        WallClockLabels outer;
        EXPECT_EQ(clockFrame(), ClockFrame::Label);
        {
            WallClockLabels inner;
            EXPECT_EQ(clockFrame(), ClockFrame::Label);
        }
        EXPECT_EQ(clockFrame(), ClockFrame::Label) << "the inner scope gave back the outer's";
    }
    EXPECT_EQ(clockFrame(), ClockFrame::HostLocal);
}

TEST(ClockFrame, AnotherThreadIsNotAffected) {
    WallClockLabels labels;
    ClockFrame seen = ClockFrame::Label;
    std::thread([&seen] { seen = clockFrame(); }).join();
    EXPECT_EQ(seen, ClockFrame::HostLocal);
}

// ── the default is today's code ──────────────────────────────────────────────

TEST(WallClockDefault, AnEdfStartIsTheHostsMktimeUnderEveryZone) {
    SKIP_WITHOUT_ZONES();
    const Night n = night({2026, 3, 8, 1, 30, 0}, 2, {});
    for (const char* zone : kZones) {
        SCOPED_TRACE(zone);
        HostZone z(zone);
        EDFFile edf;
        ASSERT_TRUE(edf.open(n.brp.data(), n.brp.size()));
        std::tm t{};
        t.tm_year = edf.start_year - 1900;
        t.tm_mon = edf.start_month - 1;
        t.tm_mday = edf.start_day;
        t.tm_hour = edf.start_hour;
        t.tm_min = edf.start_minute;
        t.tm_sec = edf.start_second;
        t.tm_isdst = -1;
        const auto legacy = Clock::from_time_t(std::mktime(&t));  // what 12.2.0 returned
        EXPECT_EQ(edf.getStartTime(), legacy);
        { WallClockLabels labels; (void)edf.getStartTime(); }
        EXPECT_EQ(edf.getStartTime(), legacy) << "and still after a scope has ended";
    }
}

TEST(WallClockDefault, ASessionParsesIdenticallyBeforeAndAfterALabelScope) {
    SKIP_WITHOUT_ZONES();
    // Across the host's spring-forward, where the two frames differ most.
    const Night n = night({2026, 3, 8, 1, 30, 0}, 120, {600, 1800, 5400});
    for (const char* zone : kZones) {
        SCOPED_TRACE(zone);
        HostZone z(zone);
        const auto before = parse(n);
        ASSERT_NE(before, nullptr);
        std::unique_ptr<ParsedSession> labelled;
        {
            WallClockLabels labels;
            labelled = parse(n);
        }
        const auto after = parse(n);
        ASSERT_NE(after, nullptr);
        EXPECT_EQ(timesOf(*before), timesOf(*after));
        EXPECT_EQ(before->events.size(), after->events.size());
        EXPECT_EQ(before->metrics->total_events, after->metrics->total_events);
        EXPECT_DOUBLE_EQ(before->metrics->ahi, after->metrics->ahi);
        EXPECT_EQ(before->duration_seconds, after->duration_seconds);
        EXPECT_EQ(timesOf(*before).size(), timesOf(*labelled).size())
            << "labels change times, never how many there are";
    }
}

TEST(WallClockLabels, OnAUtcHostTheLabelIsTheDefault) {
    SKIP_WITHOUT_ZONES();
    HostZone z("UTC");
    const Night n = night({2026, 3, 8, 1, 30, 0}, 60, {600, 1800});
    const auto plain = parse(n);
    WallClockLabels labels;
    const auto labelled = parse(n);
    EXPECT_EQ(timesOf(*plain), timesOf(*labelled)) << "a pure re-framing";
}

// ── what the label is for: the host's DST cannot move the machine's clock ─────

TEST(WallClockLabels, AStartInsideTheHostsSpringForwardGapIsKeptAsWritten) {
    SKIP_WITHOUT_ZONES();
    HostZone z("America/New_York");  // 2026-03-08 02:00 -> 03:00
    const Night n = night({2026, 3, 8, 2, 30, 0}, 10, {});
    WallClockLabels labels;
    const auto s = parse(n);
    ASSERT_NE(s, nullptr);
    EXPECT_TRUE(sameWall(wallClockOf(*s->session_start), WallClock{2026, 3, 8, 2, 30, 0}))
        << text(wallClockOf(*s->session_start));
    EDFFile edf;
    ASSERT_TRUE(edf.open(n.brp.data(), n.brp.size()));
    EXPECT_TRUE(sameWall(wallClockOf(edf.getStartTime()), WallClock{2026, 3, 8, 2, 30, 0}));
}

TEST(WallClockLabels, ANightAcrossSpringForwardKeepsTheMachinesClock) {
    SKIP_WITHOUT_ZONES();
    HostZone z("America/New_York");
    const Night n = night({2026, 3, 8, 1, 30, 0}, 120, {1800, 5400});
    {
        // Today's frame, pinned so the finding is visible (hms-cpap SDD-074 2 E):
        // after 02:00 every time reads one hour later on the host.
        const auto s = parse(n);
        ASSERT_EQ(s->events.size(), 2u);
        EXPECT_TRUE(sameWall(hostWall(s->events[0].timestamp), WallClock{2026, 3, 8, 3, 0, 0}));
        EXPECT_TRUE(sameWall(hostWall(s->events[1].timestamp), WallClock{2026, 3, 8, 4, 0, 0}));
    }
    WallClockLabels labels;
    const auto s = parse(n);
    ASSERT_EQ(s->events.size(), 2u);
    EXPECT_TRUE(sameWall(wallClockOf(*s->session_start), WallClock{2026, 3, 8, 1, 30, 0}));
    EXPECT_TRUE(sameWall(wallClockOf(s->events[0].timestamp), WallClock{2026, 3, 8, 2, 0, 0}));
    EXPECT_TRUE(sameWall(wallClockOf(s->events[1].timestamp), WallClock{2026, 3, 8, 3, 0, 0}));
    ASSERT_FALSE(s->breathing_summary.empty());
    EXPECT_TRUE(sameWall(wallClockOf(s->breathing_summary[60].timestamp),
                         WallClock{2026, 3, 8, 2, 30, 0}))
        << "minute 60 is 02:30 on the machine, an hour the host skips";
}

TEST(WallClockLabels, StrDaysAcrossDstAreNoonsExactlyADayApart) {
    SKIP_WITHOUT_ZONES();
    HostZone z("America/New_York");
    EDFWriteSpec spec;
    spec.start = {2026, 3, 7, 12, 0, 0};
    spec.record_duration = 86400;
    EDFWriteSignal dur;
    dur.label = "Duration";
    dur.phys_dim = "min";
    dur.phys_min = 0;
    dur.phys_max = 1440;
    dur.dig_min = 0;
    dur.dig_max = 1440;
    dur.samples = {420, 420, 420};
    spec.signals.push_back(dur);
    const auto bytes = writeEDF(spec);
    WallClockLabels labels;
    const auto days = EDFParser::parseSTRFromBuffer(bytes.data(), bytes.size(), "dev");
    ASSERT_EQ(days.size(), 3u);
    for (int i = 0; i < 3; ++i) {
        EXPECT_TRUE(sameWall(wallClockOf(days[i].record_date), WallClock{2026, 3, 7 + i, 12, 0, 0}))
            << text(wallClockOf(days[i].record_date));
    }
    EXPECT_EQ(days[2].record_date - days[1].record_date, std::chrono::hours(24));
}

#ifdef CPAPDASH_WITH_SEFAM
TEST(WallClockLabels, ASefamStartRecordReadsBackAsItsSixNumbers) {
    SKIP_WITHOUT_ZONES();
    HostZone z("America/New_York");
    const std::string text_ini =
        "[Start Record]\r\nHour=2\r\nMin=30\r\nSec=5\r\n"
        "Day=8\r\nMonth=3\r\nYear=2026\r\n"
        "[Chan0]\r\nName=FLW\r\nMin=0\r\nMax=255\r\nFreq=25\r\nBit=8\r\n";
    WallClockLabels labels;
    const SefamIni ini = SefamParser::parseIniText(text_ini);
    ASSERT_TRUE(ini.start.has_value());
    EXPECT_TRUE(sameWall(wallClockOf(*ini.start), WallClock{2026, 3, 8, 2, 30, 5}));
}
#endif

#ifdef CPAPDASH_WITH_BMC
namespace {

// A bare BMC packet: sync, session number, stamp; every other word 0.
std::vector<uint8_t> bmcSeconds(const WallClock& start, int count, uint16_t session) {
    std::vector<uint8_t> out;
    const auto t0 = wallClockLabel(start);
    for (int i = 0; i < count; ++i) {
        const WallClock w = wallClockOf(t0 + std::chrono::seconds(i));
        std::vector<uint8_t> p(kBmcPacketBytes, 0);
        p[0] = 0xAA;
        p[1] = 0xAA;
        p[2] = static_cast<uint8_t>(session);
        const size_t ts = 2 * kBmcPacketWords;
        p[ts] = static_cast<uint8_t>(w.year & 0xFF);
        p[ts + 1] = static_cast<uint8_t>(w.year >> 8);
        p[ts + 2] = static_cast<uint8_t>(w.month);
        p[ts + 3] = static_cast<uint8_t>(w.day);
        p[ts + 4] = static_cast<uint8_t>(w.hour);
        p[ts + 5] = static_cast<uint8_t>(w.minute);
        p[ts + 6] = static_cast<uint8_t>(w.second);
        out.insert(out.end(), p.begin(), p.end());
    }
    return out;
}

std::vector<uint8_t> bmcEvt(uint16_t session, uint32_t onset_since_noon) {
    std::vector<uint8_t> b(kBmcEventRegionBytes, 0xFF);
    std::vector<uint8_t> r(kBmcEventRecordBytes, 0);
    r[0] = r[1] = 0xAA;
    r[2] = static_cast<uint8_t>(session);
    r[4] = static_cast<uint8_t>(bmc_event::kObstructive);
    for (int i = 0; i < 4; ++i) r[8 + i] = static_cast<uint8_t>(onset_since_noon >> (8 * i));
    r[12] = 12;
    b.insert(b.end(), r.begin(), r.end());
    return b;
}

}  // namespace

TEST(WallClockLabels, ABmcSessionIsItsOwnStampsAndALabelHintPicksIt) {
    SKIP_WITHOUT_ZONES();
    HostZone z("America/New_York");
    // Two sessions: 01:30 to 03:30 across the host's spring-forward, and 05:00.
    auto raw = bmcSeconds({2026, 3, 8, 1, 30, 0}, 7200, 1);
    const auto later = bmcSeconds({2026, 3, 8, 5, 0, 0}, 600, 2);
    raw.insert(raw.end(), later.begin(), later.end());
    // 02:30 on the 8th is 14.5 h after noon of the 7th.
    const auto card = buildBmcCard("11002233", {raw}, bmcEvt(1, 14 * 3600 + 1800));

    WallClockLabels labels;
    auto s = BmcParser::sessionFromCard(card, card.sessions[0], "dev", "BMC");
    EXPECT_TRUE(sameWall(wallClockOf(*s->session_start), WallClock{2026, 3, 8, 1, 30, 0}));
    ASSERT_EQ(s->events.size(), 1u);
    EXPECT_TRUE(sameWall(wallClockOf(s->events[0].timestamp), WallClock{2026, 3, 8, 2, 30, 0}))
        << "an hour the host does not have";
    ASSERT_GE(s->breathing_summary.size(), 61u);
    EXPECT_TRUE(sameWall(wallClockOf(s->breathing_summary[60].timestamp),
                         WallClock{2026, 3, 8, 2, 30, 0}));

    // A time-point hint comes back through the same frame. 05:05 as a label is
    // inside the second session; read through the host's localtime it would be
    // 00:05 and pick the first.
    const auto dir = std::filesystem::temp_directory_path() / "sdd013_bmc_hint";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto evt = bmcEvt(1, 14 * 3600 + 1800);
    std::ofstream(dir / "11002233.000", std::ios::binary)
        .write(reinterpret_cast<const char*>(raw.data()), static_cast<std::streamsize>(raw.size()));
    std::ofstream(dir / "11002233.evt", std::ios::binary)
        .write(reinterpret_cast<const char*>(evt.data()), static_cast<std::streamsize>(evt.size()));
    BmcParser parser;
    auto picked = parser.parseSession(dir.string(), "dev", "BMC",
                                      wallClockLabel({2026, 3, 8, 5, 5, 0}));
    std::filesystem::remove_all(dir);
    ASSERT_NE(picked, nullptr);
    EXPECT_EQ(picked->duration_seconds, 600);
}
#endif
