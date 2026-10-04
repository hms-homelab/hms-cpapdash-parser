#ifdef CPAPDASH_WITH_BMC

// BMC / React Health Luna (SDD-007), on synthetic cards built here in code.
//
// They follow the layout of a real G2S card (docs/BMC_FORMAT.md) with an invented
// serial and invented dates. The real card is the accuracy check, run by hand
// through tools/bmc_probe; these pin the rules one card cannot exercise on
// demand: a clock set backwards, a duplicate second, the 60 s session gap, the
// noon anchor across midnight, "not valid" words, a torn final packet.

#include <gtest/gtest.h>

#include "cpapdash/parser/BmcParser.h"
#include "cpapdash/parser/ISessionParser.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace cpapdash::parser;
namespace fs = std::filesystem;

namespace {

const std::string kSerial = "11002233";  // invented

struct Stamp {
    int y, mo, d, h, mi, s;
};

Stamp plus(Stamp t, int seconds) {
    const BmcCivilSeconds c = *bmcCivilSeconds(t.y, t.mo, t.d, t.h, t.mi, t.s) + seconds;
    // Back through the parser's own arithmetic would be circular; walk it here.
    int64_t day = c / 86400, tod = c % 86400;
    // days -> civil (same algorithm, written independently for the test)
    day += 719468;
    const int64_t era = day / 146097;
    const unsigned doe = static_cast<unsigned>(day - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    Stamp out;
    out.d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    out.mo = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    out.y = static_cast<int>(yoe + era * 400 + (out.mo <= 2));
    out.h = static_cast<int>(tod / 3600);
    out.mi = static_cast<int>((tod % 3600) / 60);
    out.s = static_cast<int>(tod % 60);
    return out;
}

struct Second {
    Stamp at;
    uint16_t session = 1;
    uint16_t epap = 16;   // 8.0 cmH2O
    uint16_t ipap = 24;   // 12.0 cmH2O
    uint16_t leak = 50;   // 5.0 L/min
    uint16_t rr = 15;
    uint16_t ie = 20;     // 2.0
};

void putLe16(std::vector<uint8_t>& b, size_t off, uint16_t v) {
    b[off] = static_cast<uint8_t>(v & 0xFF);
    b[off + 1] = static_cast<uint8_t>(v >> 8);
}

void putLe32(std::vector<uint8_t>& b, size_t off, uint32_t v) {
    for (int i = 0; i < 4; ++i) b[off + i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
}

std::vector<uint8_t> packet(const Second& s, uint16_t sync = kBmcSync) {
    std::vector<uint8_t> p(kBmcPacketBytes, 0);
    putLe16(p, 0, sync);
    putLe16(p, 2 * bmc_word::kSessionNumber, s.session);
    putLe16(p, 2 * bmc_word::kEpap, s.epap);
    putLe16(p, 2 * bmc_word::kIpap, s.ipap);
    putLe16(p, 2 * bmc_word::kLeak, s.leak);
    putLe16(p, 2 * bmc_word::kRespRate, s.rr);
    putLe16(p, 2 * bmc_word::kIeRatio, s.ie);
    const size_t ts = 2 * kBmcPacketWords;
    putLe16(p, ts, static_cast<uint16_t>(s.at.y));
    p[ts + 2] = static_cast<uint8_t>(s.at.mo);
    p[ts + 3] = static_cast<uint8_t>(s.at.d);
    p[ts + 4] = static_cast<uint8_t>(s.at.h);
    p[ts + 5] = static_cast<uint8_t>(s.at.mi);
    p[ts + 6] = static_cast<uint8_t>(s.at.s);
    return p;
}

// `count` consecutive seconds from `start`.
std::vector<uint8_t> run(Stamp start, int count, uint16_t session = 1) {
    std::vector<uint8_t> out;
    for (int i = 0; i < count; ++i) {
        Second s;
        s.at = plus(start, i);
        s.session = session;
        const auto p = packet(s);
        out.insert(out.end(), p.begin(), p.end());
    }
    return out;
}

struct Event {
    uint16_t session;
    uint16_t type;
    uint32_t onset;  // seconds since noon of the sleep day
    uint32_t duration;
};

std::vector<uint8_t> evtFile(const std::vector<Event>& events, int fill_slots = 4) {
    std::vector<uint8_t> b(kBmcEventRegionBytes, 0xFF);
    for (const auto& e : events) {
        std::vector<uint8_t> r(kBmcEventRecordBytes, 0);
        putLe16(r, 0, kBmcSync);
        putLe16(r, 2, e.session);
        putLe16(r, 4, e.type);
        putLe32(r, 8, e.onset);
        putLe32(r, 12, e.duration);
        b.insert(b.end(), r.begin(), r.end());
    }
    for (int i = 0; i < fill_slots; ++i) b.insert(b.end(), kBmcEventRecordBytes, 0xFF);
    return b;
}

std::vector<uint8_t> cat(std::vector<uint8_t> a, const std::vector<uint8_t>& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

class BmcCardDir : public ::testing::Test {
protected:
    fs::path dir_;
    void SetUp() override {
        dir_ = fs::temp_directory_path() /
               ("bmc_test_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
                "_" + ::testing::UnitTest::GetInstance()->current_test_info()->name());
        fs::remove_all(dir_);
        fs::create_directories(dir_);
    }
    void TearDown() override { fs::remove_all(dir_); }
    void write(const std::string& name, const std::vector<uint8_t>& bytes) {
        std::ofstream(dir_ / name, std::ios::binary)
            .write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    }
};

const Stamp kEvening{2031, 3, 14, 23, 0, 0};  // invented date

}  // namespace

// ── detection ────────────────────────────────────────────────────────────────

TEST(BmcDetect, AnEightDigitG2sSerialNamesTheBrand) {
    // The rule this card broke: a G2S serial is eight digits, not \d\dC\d{5}.
    EXPECT_EQ(detectManufacturer(std::vector<std::string>{"11002233.USR", "11002233.000"}),
              DeviceManufacturer::BMC);
    EXPECT_EQ(detectManufacturer(std::vector<std::string>{"16C01034.usr"}),
              DeviceManufacturer::BMC)
        << "the RESmart GII shape still detects";
}

TEST(BmcDetect, AWordNamedUsrIsNotABmcCard) {
    EXPECT_NE(detectManufacturer(std::vector<std::string>{"settings.usr"}), DeviceManufacturer::BMC);
    EXPECT_NE(detectManufacturer(std::vector<std::string>{"AB12CD34.usr"}), DeviceManufacturer::BMC)
        << "four digits is too few to be a serial";
}

TEST(BmcDetect, TheFactoryBuildsAParser) {
    auto p = createParser(DeviceManufacturer::BMC);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->manufacturer(), DeviceManufacturer::BMC);
}

// ── packets ──────────────────────────────────────────────────────────────────

TEST(BmcPacket, AWrongSyncOrAFalseDateIsRefused) {
    Second s;
    s.at = kEvening;
    EXPECT_TRUE(decodeBmcPacket(packet(s).data()).has_value());
    EXPECT_FALSE(decodeBmcPacket(packet(s, 0x1234).data()).has_value());
    s.at = Stamp{2031, 2, 30, 1, 0, 0};
    EXPECT_FALSE(decodeBmcPacket(packet(s).data()).has_value()) << "there is no 30 February";
}

TEST(BmcPacket, PacketsAreSortedByTimeNotFileOrder) {
    // The clock set back mid-card: the later-written seconds carry earlier stamps.
    const auto card = buildBmcCard(kSerial, {cat(run(plus(kEvening, 600), 30), run(kEvening, 30))},
                                   {});
    EXPECT_TRUE(card.notes.out_of_order);
    ASSERT_EQ(card.sessions.size(), 2u);
    EXPECT_EQ(card.sessions[0].start, *bmcCivilSeconds(2031, 3, 14, 23, 0, 0));
    EXPECT_EQ(card.sessions[1].start, *bmcCivilSeconds(2031, 3, 14, 23, 10, 0));
}

TEST(BmcPacket, ADuplicateSecondKeepsTheFirst) {
    const auto card = buildBmcCard(kSerial, {cat(run(kEvening, 10), run(plus(kEvening, 9), 1))}, {});
    EXPECT_EQ(card.notes.duplicate_seconds, 1);
    ASSERT_EQ(card.sessions.size(), 1u);
    EXPECT_EQ(card.sessions[0].seconds, 10);
}

TEST(BmcPacket, ATornFinalPacketIsIgnored) {
    auto bytes = run(kEvening, 5);
    bytes.resize(bytes.size() + 100, 0xAA);
    const auto card = buildBmcCard(kSerial, {bytes}, {});
    EXPECT_EQ(card.notes.packets, 5);
    EXPECT_EQ(card.notes.refused_packets, 0);
}

// ── sessions ─────────────────────────────────────────────────────────────────

TEST(BmcSessions, SixtySecondsApartIsOneSessionSixtyOneIsTwo) {
    auto a = buildBmcCard(kSerial, {cat(run(kEvening, 10), run(plus(kEvening, 9 + 60), 10))}, {});
    EXPECT_EQ(a.sessions.size(), 1u);
    auto b = buildBmcCard(kSerial, {cat(run(kEvening, 10), run(plus(kEvening, 9 + 61), 10))}, {});
    EXPECT_EQ(b.sessions.size(), 2u);
}

TEST(BmcSessions, TheSleepDayTurnsAtNoon) {
    EXPECT_EQ(bmcSleepDayNoon(*bmcCivilSeconds(2031, 3, 15, 1, 30, 0)),
              *bmcCivilSeconds(2031, 3, 14, 12, 0, 0));
    EXPECT_EQ(bmcSleepDayNoon(*bmcCivilSeconds(2031, 3, 14, 23, 0, 0)),
              *bmcCivilSeconds(2031, 3, 14, 12, 0, 0));
    EXPECT_EQ(bmcSleepDayNoon(*bmcCivilSeconds(2031, 3, 15, 12, 0, 0)),
              *bmcCivilSeconds(2031, 3, 15, 12, 0, 0));
}

// ── one session ──────────────────────────────────────────────────────────────

TEST(BmcSession, ApneasAcrossMidnightAreAnchoredToNoon) {
    // 23:00 to 01:00 next day. Onsets count from noon of the 14th: 11.5 h is
    // 23:30, 13 h is 01:00 -- both inside, the second one past midnight.
    const auto card = buildBmcCard(kSerial, {run(kEvening, 7201)},
                                   evtFile({{1, bmc_event::kObstructive, 11 * 3600 + 1800, 12},
                                            {1, bmc_event::kClearAirway, 13 * 3600, 10},
                                            {1, 7, 11 * 3600, 3600}}));
    EXPECT_EQ(card.notes.events_outside_session, 0);
    BmcNotes notes;
    auto s = BmcParser::sessionFromCard(card, card.sessions[0], "dev", "BMC", &notes);
    ASSERT_NE(s, nullptr);
    ASSERT_EQ(s->events.size(), 2u);
    EXPECT_EQ(s->events[0].event_type, EventType::OBSTRUCTIVE);
    EXPECT_EQ(s->events[0].timestamp, bmcToTimePoint(*bmcCivilSeconds(2031, 3, 14, 23, 30, 0)));
    EXPECT_EQ(s->events[1].event_type, EventType::CLEAR_AIRWAY);
    EXPECT_EQ(s->events[1].timestamp, bmcToTimePoint(*bmcCivilSeconds(2031, 3, 15, 1, 0, 0)));
    EXPECT_EQ(notes.span_types[7], 1) << "a span type is counted, never emitted as an event";
}

TEST(BmcSession, TheIndexIsARealAhi) {
    const auto card = buildBmcCard(kSerial, {run(kEvening, 3600)},
                                   evtFile({{1, bmc_event::kObstructive, 11 * 3600 + 60, 12},
                                            {1, bmc_event::kClearAirway, 11 * 3600 + 120, 10}}));
    auto s = BmcParser::sessionFromCard(card, card.sessions[0], "dev", "BMC");
    ASSERT_TRUE(s->metrics.has_value());
    EXPECT_DOUBLE_EQ(s->metrics->ahi, 2.0) << "two apneas in one hour";
    EXPECT_EQ(s->metrics->obstructive_apneas, 1);
    EXPECT_EQ(s->metrics->clear_airway_apneas, 1);
    EXPECT_EQ(s->metrics->index_kind, SessionMetrics::IndexKind::AHI)
        << "the machine classifies its apneas; the index matched an independent AHI";
}

TEST(BmcSession, AnEventOutsideEverySessionIsCountedNotPlaced) {
    const auto card = buildBmcCard(kSerial, {run(kEvening, 600)},
                                   evtFile({{1, bmc_event::kObstructive, 2 * 3600, 12}}));
    EXPECT_EQ(card.notes.events_outside_session, 1);
    auto s = BmcParser::sessionFromCard(card, card.sessions[0], "dev", "BMC");
    EXPECT_TRUE(s->events.empty());
}

TEST(BmcSession, UnitsAreTheOnesTheFormatDocumentNames) {
    const auto card = buildBmcCard(kSerial, {run(kEvening, 120)}, {});
    auto s = BmcParser::sessionFromCard(card, card.sessions[0], "dev", "BMC");
    ASSERT_EQ(s->breathing_summary.size(), 2u);
    EXPECT_DOUBLE_EQ(*s->breathing_summary[0].leak_rate, 5.0) << "0.1 L/min per unit";
    EXPECT_DOUBLE_EQ(*s->breathing_summary[0].therapy_pressure, 12.0) << "0.5 cmH2O per unit";
    ASSERT_TRUE(s->breathing_summary[0].epr_pressure.has_value()) << "EPAP per minute";
    EXPECT_DOUBLE_EQ(*s->breathing_summary[0].epr_pressure, 8.0) << "EPAP, 0.5 cmH2O per unit";
    ASSERT_TRUE(s->metrics.has_value());
    EXPECT_DOUBLE_EQ(*s->metrics->avg_therapy_pressure, 12.0);
    EXPECT_DOUBLE_EQ(*s->metrics->avg_epr_pressure, 8.0) << "the night's EPAP, from the minutes";
    EXPECT_DOUBLE_EQ(*s->breathing_summary[0].respiratory_rate, 15.0);
    EXPECT_DOUBLE_EQ(*s->breathing_summary[0].ie_ratio, 2.0);
    EXPECT_EQ(s->duration_seconds, 120);
    ASSERT_TRUE(s->settings.has_value());
    EXPECT_EQ(s->settings->therapy_mode, 2) << "IPAP above EPAP: bi-level";
    EXPECT_EQ(s->manufacturer, DeviceManufacturer::BMC);
    EXPECT_EQ(s->serial_number, kSerial);
    // SDD-008: a Luna card names neither its model nor its firmware.
    EXPECT_EQ(s->product_name, "");
    EXPECT_EQ(s->firmware, "");
}

TEST(BmcSession, ANotValidWordIsLeftOut) {
    std::vector<uint8_t> bytes;
    for (int i = 0; i < 60; ++i) {
        Second sec;
        sec.at = plus(kEvening, i);
        if (i < 30) {
            sec.leak = kBmcInvalid;
            sec.rr = kBmcInvalid;
            sec.epap = kBmcInvalid;
        }
        const auto p = packet(sec);
        bytes.insert(bytes.end(), p.begin(), p.end());
    }
    const auto card = buildBmcCard(kSerial, {bytes}, {});
    auto s = BmcParser::sessionFromCard(card, card.sessions[0], "dev", "BMC");
    EXPECT_EQ(s->native_samples.leak.size(), 30u);
    EXPECT_DOUBLE_EQ(*s->breathing_summary[0].respiratory_rate, 15.0)
        << "a 0xFFFF must not pull the mean toward 65535";
    EXPECT_DOUBLE_EQ(*s->breathing_summary[0].epr_pressure, 8.0)
        << "nor the EPAP mean";
}

// ── events file ──────────────────────────────────────────────────────────────

TEST(BmcEvents, FillIsNotABrokenRecordButGarbageIs) {
    auto b = evtFile({{1, bmc_event::kObstructive, 100, 12}}, 3);
    int refused = -1;
    EXPECT_EQ(decodeBmcEvents(b.data(), b.size(), &refused).size(), 1u);
    EXPECT_EQ(refused, 0);
    b.insert(b.end(), kBmcEventRecordBytes, 0x11);
    EXPECT_EQ(decodeBmcEvents(b.data(), b.size(), &refused).size(), 1u);
    EXPECT_EQ(refused, 1);
}

// ── the parser on a folder ───────────────────────────────────────────────────

TEST_F(BmcCardDir, TheHintPicksTheSessionAndNoHintTheLatest) {
    // Two nights a day apart, in two numbered raw files.
    write(kSerial + ".000", run(kEvening, 1800, 1));
    write(kSerial + ".001", run(plus(kEvening, 86400), 900, 2));
    write(kSerial + ".evt", evtFile({{2, bmc_event::kObstructive, 11 * 3600 + 60, 12}}));
    write(kSerial + ".USR", std::vector<uint8_t>(64, 0));

    EXPECT_EQ(detectManufacturer(dir_.string()), DeviceManufacturer::BMC);
    const auto sessions = BmcParser::listSessions(dir_.string());
    ASSERT_EQ(sessions.size(), 2u);

    BmcParser parser;
    auto latest = parser.parseSession(dir_.string(), "dev", "BMC");
    ASSERT_NE(latest, nullptr);
    EXPECT_EQ(latest->duration_seconds, 900);
    EXPECT_EQ(latest->events.size(), 1u);

    auto first = parser.parseSession(dir_.string(), "dev", "BMC",
                                     bmcToTimePoint(*bmcCivilSeconds(2031, 3, 14, 23, 10, 0)));
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first->duration_seconds, 1800);
    EXPECT_TRUE(first->events.empty());
}

TEST_F(BmcCardDir, NumberedFilesWithNoEventsBesideThemAreNotACard) {
    write(kSerial + ".000", run(kEvening, 60));
    BmcParser parser;
    EXPECT_EQ(parser.parseSession(dir_.string(), "dev", "BMC"), nullptr);
}

TEST(BmcBuffers, BuffersReadTheSameAsAFolder) {
    const auto raw0 = run(kEvening, 1800, 1);
    const auto raw1 = run(plus(kEvening, 86400), 900, 2);
    const auto evt = evtFile({{2, bmc_event::kObstructive, 11 * 3600 + 60, 12}});
    std::map<std::string, std::pair<const uint8_t*, size_t>> buffers{
        {kSerial + ".000", {raw0.data(), raw0.size()}},
        {kSerial + ".001", {raw1.data(), raw1.size()}},
        {kSerial + ".evt", {evt.data(), evt.size()}}};
    BmcParser parser;
    auto latest = parser.parseSessionFromBuffers(buffers, "dev", "BMC");
    ASSERT_NE(latest, nullptr);
    EXPECT_EQ(latest->duration_seconds, 900);
    EXPECT_EQ(latest->serial_number, kSerial);
    auto first = parser.parseSessionFromBuffers(buffers, "dev", "BMC", "20310314_231000");
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first->duration_seconds, 1800);
}

#endif  // CPAPDASH_WITH_BMC
