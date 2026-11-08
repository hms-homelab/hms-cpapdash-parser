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

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
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

// Word positions as docs/BMC_FORMAT.md gives them, written out here rather than
// taken from the header, so a test pins the document and not the code.
constexpr size_t kWordPressure = 4;   // 25 samples
constexpr size_t kWordFlow = 54;      // 25 samples, signed
constexpr size_t kWordVt = 99;        // mL
constexpr size_t kWordMv = 101;       // 0.1 L/min

struct Second {
    Stamp at;
    uint16_t session = 1;
    uint16_t epap = 16;   // 8.0 cmH2O
    uint16_t ipap = 24;   // 12.0 cmH2O
    uint16_t leak = 50;   // 5.0 L/min
    uint16_t rr = 15;
    uint16_t ie = 20;     // Te/Ti 2.0
    uint16_t vt = 0;
    uint16_t mv = 0;
    std::array<uint16_t, 25> pressure{};
    std::array<uint16_t, 25> flow{};
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
    putLe16(p, 2 * kWordVt, s.vt);
    putLe16(p, 2 * kWordMv, s.mv);
    for (size_t i = 0; i < 25; ++i) {
        putLe16(p, 2 * (kWordPressure + i), s.pressure[i]);
        putLe16(p, 2 * (kWordFlow + i), s.flow[i]);
    }
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

// A .USR carrying `model` at its documented offset; every other byte is filler.
std::vector<uint8_t> usrFile(const std::string& model) {
    std::vector<uint8_t> b(9000, 0x5A);
    std::fill(b.begin() + 8854, b.begin() + 8854 + 12, 0);
    std::copy(model.begin(), model.end(), b.begin() + 8854);
    return b;
}

// A breathing night, in codes, the way the card writes it. Each breath is 4 s
// (100 samples): inspiration 1.6 s, a half sine of `insp` codes above the
// leak level; expiration 2.4 s, a half sine of insp x 40 / 60 below it, so the
// two volumes are equal and the breath's mean IS the leak level.
struct Train {
    int level = 450;                                            // codes
    int insp = 300;                                             // codes, 30 L/min
    std::function<int(int)> pressure = [](int) { return 70; };  // code per second
    std::function<uint16_t(int)> leak = [](int) { return uint16_t(50); };
    std::function<uint16_t(int)> vt = [](int) { return uint16_t(0); };
    uint16_t mv = 0, rr = 15, ie = 15;                          // ie: Te/Ti 1.5
    bool breathe = true;
};

int trainSample(const Train& t, long n) {
    if (!t.breathe) return t.level;
    const long phase = n % 100;
    const double pi = std::acos(-1.0);
    if (phase < 40) return static_cast<int>(std::lround(t.level + t.insp * std::sin(pi * phase / 40.0)));
    return static_cast<int>(
        std::lround(t.level - t.insp * 40.0 / 60.0 * std::sin(pi * (phase - 40) / 60.0)));
}

std::vector<uint8_t> breathing(Stamp start, int seconds, const Train& t) {
    std::vector<uint8_t> out;
    for (int sec = 0; sec < seconds; ++sec) {
        Second s;
        s.at = plus(start, sec);
        s.epap = s.ipap = 14;  // 7.0 cmH2O, a CPAP
        s.leak = t.leak(sec);
        s.rr = t.rr;
        s.ie = t.ie;
        s.vt = t.vt(sec);
        s.mv = t.mv;
        const int p = t.pressure(sec);
        for (int i = 0; i < 25; ++i) {
            s.flow[i] = static_cast<uint16_t>(static_cast<int16_t>(trainSample(t, sec * 25L + i)));
            s.pressure[i] = static_cast<uint16_t>(static_cast<int16_t>(p));
        }
        const auto p8 = packet(s);
        out.insert(out.end(), p8.begin(), p8.end());
    }
    return out;
}

// Breaths whose onset falls in minute `m` of a session starting at `start`.
int breathsInMinute(const ParsedSession& s, Stamp start, int m) {
    const auto t0 = bmcToTimePoint(*bmcCivilSeconds(start.y, start.mo, start.d, start.h,
                                                    start.mi, start.s)) +
                    std::chrono::minutes(m);
    int n = 0;
    for (const auto& b : s.breaths)
        if (b.onset >= t0 && b.onset < t0 + std::chrono::minutes(1)) ++n;
    return n;
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
    // Word 105 is Te/Ti x 10; the field carries Ti/Te, as for every other
    // machine. With no breath in the flow, the machine's word is what is left.
    EXPECT_DOUBLE_EQ(*s->breathing_summary[0].ie_ratio, 0.5);
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

// ── flow and pressure (the 25 Hz channels) ───────────────────────────────────

namespace {

// A card folder holding `raw` as its only raw file, an empty .evt and, unless
// `model` is null, a .USR naming that model.
class BmcWaveDir : public BmcCardDir {
protected:
    std::unique_ptr<ParsedSession> parse(const std::vector<uint8_t>& raw, const char* model) {
        write(kSerial + ".000", raw);
        write(kSerial + ".evt", evtFile({}));
        if (model) write(kSerial + ".USR", usrFile(model));
        BmcParser parser;
        return parser.parseSession(dir_.string(), "dev", "BMC");
    }
};

}  // namespace

TEST_F(BmcWaveDir, G3PressureIsTheCodeOverTen) {
    Train t;
    t.pressure = [](int sec) { return sec < 60 ? 40 : sec < 120 ? 70 : 150; };
    auto s = parse(breathing(kEvening, 180, t), "G3 A20");
    ASSERT_NE(s, nullptr);
    ASSERT_EQ(s->breathing_summary.size(), 3u);
    const double want[] = {4.0, 7.0, 15.0};
    for (int m = 0; m < 3; ++m) {
        const auto& row = s->breathing_summary[m];
        EXPECT_TRUE(row.pressure_measured);
        EXPECT_NEAR(row.avg_pressure, want[m], 1e-9);
        EXPECT_NEAR(row.min_pressure, want[m], 1e-9);
        EXPECT_NEAR(row.max_pressure, want[m], 1e-9);
    }
    ASSERT_TRUE(s->metrics->avg_pressure.has_value());
    EXPECT_NEAR(*s->metrics->avg_pressure, (4.0 + 7.0 + 15.0) / 3, 1e-9);
}

TEST_F(BmcWaveDir, G2sPressureHasAZeroOffsetAndItsOwnGain) {
    // (code - 102) / 23.23: a code / 10 reading would say 19.5 and 33.4.
    Train t;
    t.pressure = [](int sec) { return sec < 60 ? 195 : 334; };
    auto s = parse(breathing(kEvening, 120, t), "G2S B20A");
    ASSERT_EQ(s->breathing_summary.size(), 2u);
    EXPECT_TRUE(s->breathing_summary[0].pressure_measured);
    EXPECT_NEAR(s->breathing_summary[0].avg_pressure, 4.0, 0.02);
    EXPECT_NEAR(s->breathing_summary[1].avg_pressure, 10.0, 0.02);
}

TEST_F(BmcWaveDir, AnUnmeasuredModelHasNoPressureButStillHasFlow) {
    for (const char* model : {"G3 B25", static_cast<const char*>(nullptr)}) {
        SCOPED_TRACE(model ? model : "no .USR");
        auto s = parse(breathing(kEvening, 120, Train{}), model);
        ASSERT_EQ(s->breathing_summary.size(), 2u);
        for (const auto& row : s->breathing_summary) {
            EXPECT_FALSE(row.pressure_measured) << "a guessed scale is a wrong number";
            EXPECT_EQ(row.avg_pressure, 0.0);
            EXPECT_TRUE(row.flow_measured);
            EXPECT_NEAR(row.max_flow_rate, 30.0, 0.05) << "edge minutes: the level settles";
        }
        EXPECT_FALSE(s->metrics->avg_pressure.has_value());
        EXPECT_DOUBLE_EQ(*s->metrics->avg_therapy_pressure, 7.0) << "the set pressure stays";
        fs::remove(dir_ / (kSerial + ".USR"));
    }
}

TEST_F(BmcWaveDir, ASecondTheMachineLeftAtZeroIsNoPressureReading) {
    Train t;
    t.pressure = [](int sec) { return sec >= 60 && sec < 120 ? 0 : 70; };
    auto s = parse(breathing(kEvening, 180, t), "G3 A20");
    ASSERT_EQ(s->breathing_summary.size(), 3u);
    EXPECT_TRUE(s->breathing_summary[0].pressure_measured);
    EXPECT_FALSE(s->breathing_summary[1].pressure_measured) << "not 0 cmH2O";
    EXPECT_TRUE(s->breathing_summary[2].pressure_measured);
    EXPECT_NEAR(*s->metrics->min_pressure, 7.0, 1e-9) << "the empty minute is left out";
}

TEST_F(BmcWaveDir, FlowIsPatientFlowAtATenthOfALitrePerCode) {
    auto s = parse(breathing(kEvening, 180, Train{}), "G3 A20");
    ASSERT_EQ(s->breathing_summary.size(), 3u);
    const auto& row = s->breathing_summary[1];  // a whole minute away from the edges
    EXPECT_TRUE(row.flow_measured);
    EXPECT_NEAR(row.avg_flow_rate, 0.0, 0.02) << "the leak level is removed";
    EXPECT_NEAR(row.max_flow_rate, 30.0, 0.02) << "300 codes above the level";
    EXPECT_NEAR(row.min_flow_rate, -20.0, 0.02) << "200 codes below it";
    ASSERT_TRUE(row.total_leak.has_value());
    EXPECT_NEAR(*row.total_leak, (450 - 383) / 10.0, 0.01) << "mean total flow above the 383 zero";
    ASSERT_TRUE(row.inspiratory_time.has_value());
    EXPECT_DOUBLE_EQ(*row.inspiratory_time, 1.6) << "40 samples, exact to a sample";
    EXPECT_DOUBLE_EQ(*row.expiratory_time, 2.4) << "60 samples";
    EXPECT_NEAR(*row.ie_ratio, 1.6 / 2.4, 1e-9) << "Ti/Te from the breaths";
    EXPECT_FALSE(row.flow_limitation.has_value()) << "not reported for this machine";
    EXPECT_EQ(breathsInMinute(*s, kEvening, 1), 15);
    EXPECT_EQ(breathsInMinute(*s, kEvening, 2), 14)
        << "the session's last breath has no next inspiration and is dropped";
}

TEST_F(BmcWaveDir, ANegativeFlowCodeIsSigned) {
    // A leak level of 100 codes puts the expiration trough at -100 codes,
    // written 65436. Read unsigned it would be 6,500 L/min.
    Train t;
    t.level = 100;
    const auto raw = breathing(kEvening, 180, t);
    auto s = parse(raw, "G3 A20");
    const auto& row = s->breathing_summary[1];
    EXPECT_NEAR(row.min_flow_rate, -20.0, 0.02);
    EXPECT_NEAR(row.max_flow_rate, 30.0, 0.02);
    EXPECT_NEAR(row.avg_flow_rate, 0.0, 0.02);
    EXPECT_NEAR(*row.total_leak, (100 - 383) / 10.0, 0.01);
    EXPECT_EQ(breathsInMinute(*s, kEvening, 1), 15);
}

TEST_F(BmcWaveDir, AMinuteAtTheLeakCapHasNoFlow) {
    Train t;
    t.leak = [](int sec) { return uint16_t(sec >= 60 && sec < 120 ? 1000 : 50); };
    auto s = parse(breathing(kEvening, 180, t), "G3 A20");
    ASSERT_EQ(s->breathing_summary.size(), 3u);
    const auto& capped = s->breathing_summary[1];
    EXPECT_FALSE(capped.flow_measured) << "mask off: what flows is not breathing";
    EXPECT_EQ(capped.avg_flow_rate, 0.0);
    EXPECT_FALSE(capped.total_leak.has_value());
    EXPECT_FALSE(capped.inspiratory_time.has_value());
    EXPECT_TRUE(capped.pressure_measured) << "the pressure is still read";
    EXPECT_DOUBLE_EQ(*capped.leak_rate, 100.0);
    EXPECT_TRUE(s->breathing_summary[0].flow_measured);
    EXPECT_TRUE(s->breathing_summary[2].flow_measured);
    EXPECT_EQ(breathsInMinute(*s, kEvening, 1), 0);
}

TEST_F(BmcWaveDir, TidalVolumeAndMinuteVentilationAreTheMachinesWords) {
    Train t;
    t.vt = [](int sec) { return uint16_t(sec < 60 ? 480 : sec < 120 ? kBmcInvalid : 0); };
    t.mv = 72;
    auto s = parse(breathing(kEvening, 180, t), "G3 A20");
    ASSERT_EQ(s->breathing_summary.size(), 3u);
    ASSERT_TRUE(s->breathing_summary[0].tidal_volume.has_value());
    EXPECT_DOUBLE_EQ(*s->breathing_summary[0].tidal_volume, 480.0) << "word 99, mL";
    EXPECT_NEAR(*s->breathing_summary[0].minute_ventilation, 7.2, 1e-9) << "word 101, 0.1 L/min";
    EXPECT_FALSE(s->breathing_summary[1].tidal_volume.has_value())
        << "0xFFFF is no reading, and our integral does not stand in for it";
    EXPECT_FALSE(s->breathing_summary[2].tidal_volume.has_value()) << "0 is no breath yet";

    t.vt = [](int) { return uint16_t(480); };
    t.mv = kBmcInvalid;
    fs::remove_all(dir_);
    fs::create_directories(dir_);
    auto u = parse(breathing(kEvening, 60, t), "G3 A20");
    EXPECT_FALSE(u->breathing_summary[0].minute_ventilation.has_value());
}

TEST_F(BmcWaveDir, TheRateIsWord104AndTheBreathsAgreeWithWords104And105) {
    // In the generator's world the machine's words and the breaths agree:
    // 15 breaths a minute, Te/Ti = 2.4 / 1.6 = 1.5 = word 105 / 10.
    auto s = parse(breathing(kEvening, 180, Train{}), "G3 A20");
    const auto& row = s->breathing_summary[1];
    EXPECT_EQ(breathsInMinute(*s, kEvening, 1), static_cast<int>(*row.respiratory_rate));
    EXPECT_NEAR(1.0 / *row.ie_ratio, 15 / 10.0, 1e-9);

    // The reported rate is the machine's, not our count.
    Train t;
    t.rr = 12;
    fs::remove_all(dir_);
    fs::create_directories(dir_);
    auto r = parse(breathing(kEvening, 180, t), "G3 A20");
    EXPECT_DOUBLE_EQ(*r->breathing_summary[1].respiratory_rate, 12.0);
    EXPECT_EQ(breathsInMinute(*r, kEvening, 1), 15);
}

TEST(BmcSession, ARateOfZeroIsTheMachineWarmingUpNotARate) {
    // A real card writes word 104 = 0 for its first seconds, before it has a
    // rate. A short session of nothing else read 0.0 breaths/min; a longer one
    // had its mean pulled down by them.
    auto minute = [](std::function<uint16_t(int)> rr) {
        std::vector<uint8_t> bytes;
        for (int i = 0; i < 60; ++i) {
            Second sec;
            sec.at = plus(kEvening, i);
            sec.rr = rr(i);
            const auto p = packet(sec);
            bytes.insert(bytes.end(), p.begin(), p.end());
        }
        return bytes;
    };
    const auto zeros = buildBmcCard(kSerial, {minute([](int) { return uint16_t(0); })}, {});
    auto s = BmcParser::sessionFromCard(zeros, zeros.sessions[0], "dev", "BMC");
    EXPECT_FALSE(s->breathing_summary[0].respiratory_rate.has_value()) << "not measured, not 0";
    EXPECT_FALSE(s->metrics->avg_respiratory_rate.has_value());

    const auto mixed = buildBmcCard(
        kSerial, {minute([](int i) { return uint16_t(i < 20 ? 0 : 15); })}, {});
    auto m = BmcParser::sessionFromCard(mixed, mixed.sessions[0], "dev", "BMC");
    EXPECT_DOUBLE_EQ(*m->breathing_summary[0].respiratory_rate, 15.0) << "not 10.0";
}

TEST_F(BmcWaveDir, NoBreathIsCarriedAcrossSecondsTheCardDoesNotHave) {
    // Seconds 69-73 are missing, from mid-inspiration: the breath that starts
    // at 68.04 s would otherwise run to 76.04 s with a 5.96 s "inspiration",
    // the way a 4 s session spread over 12 s got one breath of I:E 25.
    Train t;
    const auto whole = breathing(kEvening, 180, t);
    std::vector<uint8_t> raw;
    for (int sec = 0; sec < 180; ++sec) {
        if (sec >= 69 && sec <= 73) continue;
        raw.insert(raw.end(), whole.begin() + sec * kBmcPacketBytes,
                   whole.begin() + (sec + 1) * kBmcPacketBytes);
    }
    auto s = parse(raw, "G3 A20");
    ASSERT_NE(s, nullptr);
    ASSERT_EQ(s->breathing_summary.size(), 3u);
    const auto& row = s->breathing_summary[1];
    EXPECT_TRUE(row.flow_measured) << "55 of its 60 seconds are on the card";
    // Within half a sample: beside the gap the leak level resettles and one
    // crossing can move by a sample. The breath across it would add 0.3 s to Ti.
    ASSERT_TRUE(row.inspiratory_time.has_value());
    EXPECT_NEAR(*row.inspiratory_time, 1.6, 0.02);
    EXPECT_NEAR(*row.expiratory_time, 2.4, 0.02);
    EXPECT_NEAR(*row.ie_ratio, 1.6 / 2.4, 0.01);
    const auto t0 = bmcToTimePoint(*bmcCivilSeconds(2031, 3, 14, 23, 0, 0));
    for (const auto& b : s->breaths) {
        const double on = std::chrono::duration<double>(b.onset - t0).count();
        const double off = on + b.inspiratory_time + b.expiratory_time;
        EXPECT_TRUE(off < 69.0 || on >= 74.0) << "a breath from " << on << " s to " << off << " s";
    }
}

TEST_F(BmcWaveDir, ASessionWithNoWholeBreathHasNoRateAndNoIe) {
    // The real 4 s session: four seconds over twelve, the rate word still 0xFFFF
    // and word 105 still 0. Nothing is measured, so nothing is reported.
    Train t;
    t.rr = kBmcInvalid;
    t.ie = 0;
    const auto whole = breathing(kEvening, 12, t);
    std::vector<uint8_t> raw;
    for (int sec : {0, 1, 10, 11})
        raw.insert(raw.end(), whole.begin() + sec * kBmcPacketBytes,
                   whole.begin() + (sec + 1) * kBmcPacketBytes);
    auto s = parse(raw, "G3 A20");
    ASSERT_NE(s, nullptr);
    ASSERT_EQ(s->breathing_summary.size(), 1u);
    const auto& row = s->breathing_summary[0];
    EXPECT_FALSE(row.respiratory_rate.has_value());
    EXPECT_FALSE(row.ie_ratio.has_value()) << "never Ti/Te across the gap";
    EXPECT_FALSE(row.inspiratory_time.has_value());
    EXPECT_TRUE(s->breaths.empty());
    EXPECT_FALSE(s->metrics->avg_ie_ratio.has_value());
    EXPECT_FALSE(s->metrics->avg_respiratory_rate.has_value());
}

TEST_F(BmcWaveDir, EachBreathCarriesTheMachinesTidalVolume) {
    // Breath b ends at second 4b + 4; the machine writes its volume, 400 + b,
    // a second later and holds it until the next breath's.
    Train t;
    t.vt = [](int sec) { return uint16_t(sec < 5 ? 0 : 400 + (sec - 5) / 4); };
    auto s = parse(breathing(kEvening, 120, t), "G3 A20");
    ASSERT_GT(s->breaths.size(), 20u);
    const auto t0 = bmcToTimePoint(*bmcCivilSeconds(2031, 3, 14, 23, 0, 0));
    for (const auto& b : s->breaths) {
        const auto ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(b.onset - t0).count();
        const int index = static_cast<int>(ms / 4000);  // breath b starts at 4b s + 40 ms
        if (index * 4 + 6 >= 120) continue;              // its word lands after the end
        EXPECT_DOUBLE_EQ(b.tidal_volume, 400 + index) << "breath " << index;
    }
}

TEST(BmcModel, TheModelStringIsReadFromTheUsrAndNothingElse) {
    const auto g3 = usrFile("G3 A20");
    EXPECT_EQ(bmcModelFromUsr(g3.data(), g3.size()), "G3 A20");
    const auto g2s = usrFile("G2S B20A");
    EXPECT_EQ(bmcModelFromUsr(g2s.data(), g2s.size()), "G2S B20A");
    EXPECT_EQ(bmcModelFromUsr(g3.data(), 8000), "") << "a short file names no model";
    auto junk = usrFile("G3 A20");
    junk[8856] = 0x07;
    EXPECT_EQ(bmcModelFromUsr(junk.data(), junk.size()), "") << "not text, not a model";
    EXPECT_TRUE(bmcPressureScale("G3 A20").has_value());
    EXPECT_TRUE(bmcPressureScale("G2S B20A").has_value());
    EXPECT_FALSE(bmcPressureScale("G3 B25").has_value());
    EXPECT_FALSE(bmcPressureScale("").has_value());
}

TEST(BmcBuffers, TheUsrAmongTheBuffersNamesTheModel) {
    Train t;
    t.pressure = [](int) { return 334; };
    const auto raw = breathing(kEvening, 120, t);
    const auto evt = evtFile({});
    const auto usr = usrFile("G2S B20A");
    std::map<std::string, std::pair<const uint8_t*, size_t>> buffers{
        {kSerial + ".000", {raw.data(), raw.size()}},
        {kSerial + ".evt", {evt.data(), evt.size()}},
        {kSerial + ".USR", {usr.data(), usr.size()}}};
    BmcParser parser;
    auto s = parser.parseSessionFromBuffers(buffers, "dev", "BMC");
    ASSERT_NE(s, nullptr);
    EXPECT_TRUE(s->breathing_summary[0].pressure_measured);
    EXPECT_NEAR(s->breathing_summary[0].avg_pressure, 10.0, 0.02);
}

TEST(BmcWaveform, TheReaderReturnsThe25HzSamplesCalibrated) {
    // Two sessions 100 s apart; the window covers the end of one, the gap and
    // the start of the other. Second 30 of the first is missing.
    Train t;
    t.pressure = [](int sec) { return 60 + sec % 3; };
    auto first = breathing(kEvening, 180, t);
    first.erase(first.begin() + 30 * kBmcPacketBytes, first.begin() + 31 * kBmcPacketBytes);
    auto card = buildBmcCard(kSerial, {cat(first, breathing(plus(kEvening, 280), 120, t))}, {});
    card.model = "G3 A20";
    ASSERT_EQ(card.sessions.size(), 2u);

    const BmcCivilSeconds s0 = *bmcCivilSeconds(2031, 3, 14, 23, 0, 0);
    const auto w = BmcParser::waveform(card, s0 + 20, s0 + 300);
    EXPECT_EQ(w.start, s0 + 20);
    EXPECT_EQ(w.sample_rate, 25.0);
    ASSERT_EQ(w.flow.size(), 280u * 25);
    ASSERT_EQ(w.pressure.size(), 280u * 25);

    // Second 100 of the first session, a whole breath from its start: sample
    // 20 of a breath is the inspiratory peak, 300 codes over the level.
    const size_t at = (100 - 20) * 25;
    EXPECT_NEAR(w.flow[at + 20], 30.0, 0.02);
    EXPECT_NEAR(w.flow[at + 70], -20.0, 0.02);
    EXPECT_NEAR(w.pressure[at], (60 + 100 % 3) / 10.0, 1e-9);
    EXPECT_TRUE(std::isnan(w.flow[(30 - 20) * 25])) << "the missing second";
    EXPECT_TRUE(std::isnan(w.pressure[(30 - 20) * 25 + 24]));
    EXPECT_TRUE(std::isnan(w.flow[(200 - 20) * 25])) << "between the sessions";
    EXPECT_FALSE(std::isnan(w.flow[(290 - 20) * 25])) << "the second session";
    EXPECT_NEAR(w.pressure[(290 - 20) * 25], (60 + 10 % 3) / 10.0, 1e-9);

    card.model = "G3 B25";
    const auto u = BmcParser::waveform(card, s0, s0 + 60);
    EXPECT_TRUE(u.pressure.empty()) << "no pressure for a model not measured";
    EXPECT_EQ(u.flow.size(), 60u * 25);
    EXPECT_TRUE(BmcParser::waveform(card, s0 + 10, s0 + 10).flow.empty());
}

#endif  // CPAPDASH_WITH_BMC
