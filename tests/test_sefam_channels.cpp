#ifdef CPAPDASH_WITH_SEFAM

// The two things a Sefam channel file will not tell you outright -- where the
// samples start, and how the bytes are scrambled -- are derived at parse time
// rather than hard-coded (SDD-005 sections 6.2 and 6.3). These tests are what
// stands behind that derivation until a donor card exists, and they matter most
// for the cases where the answer is "I do not know", because a wrong header
// offset shifts every sample in a night and nothing downstream would notice.

#include <gtest/gtest.h>

#include "cpapdash/parser/SefamParser.h"

#include <chrono>
#include <cmath>
#include <map>
#include <string>
#include <vector>

using namespace cpapdash::parser;

namespace {

SefamChannel channel(const std::string& name, int freq, int bits = 8,
                     double min = 0, double max = 255) {
    SefamChannel c;
    c.name = name;
    c.freq = freq;
    c.bits = bits;
    c.min = min;
    c.max = max;
    return c;
}

SefamIni iniWith(std::vector<SefamChannel> channels) {
    SefamIni ini;
    ini.channels = std::move(channels);
    ini.valid = !ini.channels.empty();
    return ini;
}

// A smooth waveform in raw codes, the way a real channel looks.
std::vector<uint8_t> smooth(size_t n, double period, double centre = 128,
                            double amplitude = 60) {
    std::vector<uint8_t> out;
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        const double v = centre + amplitude * std::sin(2 * M_PI * i / period);
        out.push_back(static_cast<uint8_t>(std::lround(v)));
    }
    return out;
}

std::vector<uint8_t> xorAll(std::vector<uint8_t> v, uint8_t key) {
    for (auto& b : v) b = static_cast<uint8_t>(b ^ key);
    return v;
}

} // namespace

// ── Where the samples start ─────────────────────────────────────────────────

TEST(SefamHeaderLength, StubsGiveItAway) {
    // Three channels were declared and never recorded, so their files are the
    // header and nothing else.
    const SefamIni ini = iniWith({channel("FLW", 25), channel("PRE", 5),
                                  channel("SPO", 1), channel("HRT", 1),
                                  channel("POS", 1)});
    const std::map<std::string, size_t> sizes = {
        {"FLW", 3101}, {"PRE", 701}, {"SPO", 101}, {"HRT", 101}, {"POS", 101}};

    const auto h = SefamParser::deduceHeaderLength(ini, sizes);
    ASSERT_TRUE(h.has_value());
    EXPECT_EQ(*h, 101u);
}

TEST(SefamHeaderLength, TwoRatesSolveItWithoutAnyStub) {
    const SefamIni ini = iniWith({channel("FLW", 25), channel("PRE", 5), channel("LK", 1)});
    const std::map<std::string, size_t> sizes = {
        {"FLW", 3101}, {"PRE", 701}, {"LK", 221}};

    const auto h = SefamParser::deduceHeaderLength(ini, sizes);
    ASSERT_TRUE(h.has_value());
    EXPECT_EQ(*h, 101u);
}

TEST(SefamHeaderLength, AStubInTheMixDoesNotDragTheArithmetic) {
    // The stub is excluded before the rate arithmetic runs; left in, it would
    // force h to its own size and every sample would land 101 bytes early.
    const SefamIni ini = iniWith({channel("FLW", 25), channel("PRE", 5),
                                  channel("SPO", 1), channel("HRT", 1)});
    const std::map<std::string, size_t> sizes = {
        {"FLW", 3101}, {"PRE", 701}, {"SPO", 101}, {"HRT", 101}};

    const auto h = SefamParser::deduceHeaderLength(ini, sizes);
    ASSERT_TRUE(h.has_value());
    EXPECT_EQ(*h, 101u);
}

TEST(SefamHeaderLength, ADifferentHeaderLengthIsFoundJustAsWell) {
    // Nothing in the library knows the number 101.
    const SefamIni ini = iniWith({channel("FLW", 25), channel("LK", 1)});
    const std::map<std::string, size_t> sizes = {{"FLW", 3064}, {"LK", 184}};

    const auto h = SefamParser::deduceHeaderLength(ini, sizes);
    ASSERT_TRUE(h.has_value());
    EXPECT_EQ(*h, 64u);
}

TEST(SefamHeaderLength, OneRateAndNoStubsIsNotEnough) {
    // Two channels at the same rate: the equation degenerates, and a single
    // smallest file could just as well be a shorter recording.
    const SefamIni ini = iniWith({channel("FLW", 25), channel("DET", 25)});
    const std::map<std::string, size_t> sizes = {{"FLW", 3101}, {"DET", 2851}};

    EXPECT_FALSE(SefamParser::deduceHeaderLength(ini, sizes).has_value());
}

TEST(SefamHeaderLength, AChannelEndingASampleShortIsStillTheSameRecording) {
    // PRE stops one sample before the others. A recording does not become
    // unreadable because one channel ended a fifth of a second early.
    const SefamIni ini = iniWith({channel("FLW", 25), channel("PRE", 5), channel("LK", 1)});
    const std::map<std::string, size_t> sizes = {
        {"FLW", 3101}, {"PRE", 700}, {"LK", 221}};

    const auto h = SefamParser::deduceHeaderLength(ini, sizes);
    ASSERT_TRUE(h.has_value());
    EXPECT_EQ(*h, 101u);
}

TEST(SefamHeaderLength, ChannelsThatCoverDifferentSpansYieldNothing) {
    // Here LK holds a full minute less than the others. No offset reconciles
    // that, and inventing one would shift every sample in the night.
    const SefamIni ini = iniWith({channel("FLW", 25), channel("PRE", 5), channel("LK", 1)});
    const std::map<std::string, size_t> sizes = {
        {"FLW", 3101}, {"PRE", 701}, {"LK", 161}};

    EXPECT_FALSE(SefamParser::deduceHeaderLength(ini, sizes).has_value());
}

TEST(SefamHeaderLength, StubsAndRatesMustAgree) {
    const SefamIni ini = iniWith({channel("FLW", 25), channel("PRE", 5),
                                  channel("SPO", 1), channel("HRT", 1)});
    // The stubs say 90; the populated pair says 101.
    const std::map<std::string, size_t> sizes = {
        {"FLW", 3101}, {"PRE", 701}, {"SPO", 90}, {"HRT", 90}};

    EXPECT_FALSE(SefamParser::deduceHeaderLength(ini, sizes).has_value());
}

TEST(SefamHeaderLength, AHeaderLongerThanAFileIsImpossible) {
    const SefamIni ini = iniWith({channel("FLW", 25), channel("PRE", 5),
                                  channel("SPO", 1), channel("HRT", 1),
                                  channel("TINY", 1)});
    const std::map<std::string, size_t> sizes = {
        {"FLW", 3101}, {"PRE", 701}, {"SPO", 101}, {"HRT", 101}, {"TINY", 40}};

    EXPECT_FALSE(SefamParser::deduceHeaderLength(ini, sizes).has_value());
}

TEST(SefamHeaderLength, ARemainderThatIsNotWholeSamplesIsRejected) {
    // A 16-bit channel whose payload is an odd number of bytes cannot be right.
    const SefamIni ini = iniWith({channel("FLW", 25), channel("PRE", 5),
                                  channel("SPO", 1), channel("HRT", 1),
                                  channel("PLS", 75, 16)});
    const std::map<std::string, size_t> sizes = {
        {"FLW", 3101}, {"PRE", 701}, {"SPO", 101}, {"HRT", 101}, {"PLS", 1000}};

    EXPECT_FALSE(SefamParser::deduceHeaderLength(ini, sizes).has_value());
}

TEST(SefamHeaderLength, NothingDeclaredMeansNothingToDeduce) {
    EXPECT_FALSE(SefamParser::deduceHeaderLength(iniWith({}), {}).has_value());
    EXPECT_FALSE(SefamParser::deduceHeaderLength(
        iniWith({channel("FLW", 25)}), {{"PRE", 701}}).has_value());
}

// ── How the bytes are scrambled ─────────────────────────────────────────────

TEST(SefamDescrambling, RecoversASingleByteKey) {
    const auto flow = smooth(3000, 100);
    const auto pressure = smooth(600, 40, 100, 4);
    const std::vector<uint8_t> idle(500, 0);  // a detection channel with nothing to say

    const std::vector<std::vector<uint8_t>> scrambled = {
        xorAll(flow, 0x9F), xorAll(pressure, 0x9F), xorAll(idle, 0x9F)};

    const auto d = SefamParser::deduceDescrambler(scrambled);
    EXPECT_EQ(d.kind, SefamDescrambler::Kind::SingleByteXor);
    EXPECT_EQ(d.key, 0x9F);
    EXPECT_FALSE(d.ambiguous);
    EXPECT_LT(d.roughness, d.identity_roughness);
}

// Smoothness cannot get past two candidates, ever. XOR by 0xFF is x -> 255 - x,
// which mirrors the signal and leaves every step between neighbouring samples
// exactly the length it was, so a key and its complement score identically. This
// is a property of the measure, not a shortcoming of the search, and the honest
// output for a card with nothing to break the tie is "I do not know".
TEST(SefamDescrambling, WithoutAnIdleStretchTheKeyIsAmbiguous) {
    const auto flow = smooth(3000, 100);
    const auto d = SefamParser::deduceDescrambler({xorAll(flow, 0x9F)});

    EXPECT_TRUE(d.ambiguous);
    EXPECT_FALSE(d.confirmed_by_idle_run);
    // The two it cannot separate are the true key and its mirror.
    EXPECT_TRUE(d.key == 0x9F || d.key == static_cast<uint8_t>(0x9F ^ 0xFF));
}

TEST(SefamDescrambling, AnIdleStretchPinsTheKeyExactly) {
    auto flow = smooth(3000, 100);
    std::vector<uint8_t> idle(500, 0);

    const std::vector<std::vector<uint8_t>> scrambled = {
        xorAll(flow, 0x51), xorAll(idle, 0x51)};

    const auto d = SefamParser::deduceDescrambler(scrambled);
    EXPECT_EQ(d.key, 0x51);
    EXPECT_TRUE(d.confirmed_by_idle_run);
    EXPECT_FALSE(d.ambiguous);
}

TEST(SefamDescrambling, UnscrambledDataIsLeftAlone) {
    const auto flow = smooth(3000, 100);
    const std::vector<uint8_t> idle(500, 0);  // already zero, so nothing to undo

    const auto d = SefamParser::deduceDescrambler({flow, idle});

    EXPECT_EQ(d.kind, SefamDescrambler::Kind::Identity);
    EXPECT_EQ(d.key, 0);
    EXPECT_FALSE(d.ambiguous);
}

TEST(SefamDescrambling, ConstantDataStaysIdentity) {
    // Every key scores the same on a file that never changes, and inventing one
    // from a tie would be a guess.
    const std::vector<uint8_t> flat(500, 0x00);
    const auto d = SefamParser::deduceDescrambler({flat});

    EXPECT_EQ(d.kind, SefamDescrambler::Kind::Identity);
    EXPECT_EQ(d.key, 0);
}

TEST(SefamDescrambling, NothingToJudgeLeavesIdentity) {
    EXPECT_EQ(SefamParser::deduceDescrambler({}).kind, SefamDescrambler::Kind::Identity);
    EXPECT_EQ(SefamParser::deduceDescrambler({{0x41}}).kind, SefamDescrambler::Kind::Identity);
}

// ── Decoding ────────────────────────────────────────────────────────────────

TEST(SefamDecodeChannel, SkipsTheHeaderAndScalesToTheDeclaredRange) {
    std::vector<uint8_t> raw(8, 0xAA);          // header
    raw.insert(raw.end(), {0, 255, 128});       // samples

    const SefamChannel ch = channel("FLW", 25, 8, -180, 280);
    const auto out = SefamParser::decodeChannel(raw, 8, ch, SefamDescrambler{});

    ASSERT_EQ(out.size(), 3u);
    EXPECT_DOUBLE_EQ(out[0], -180.0);
    EXPECT_DOUBLE_EQ(out[1], 280.0);
    EXPECT_NEAR(out[2], -180.0 + 128.0 * 460.0 / 255.0, 1e-9);
}

TEST(SefamDecodeChannel, AppliesTheDescrambler) {
    SefamDescrambler d;
    d.kind = SefamDescrambler::Kind::SingleByteXor;
    d.key = 0x9F;

    std::vector<uint8_t> raw(4, 0x00);
    raw.push_back(static_cast<uint8_t>(200 ^ 0x9F));

    const SefamChannel ch = channel("PRE", 5, 8, 0, 255);
    const auto out = SefamParser::decodeChannel(raw, 4, ch, d);

    ASSERT_EQ(out.size(), 1u);
    EXPECT_DOUBLE_EQ(out[0], 200.0);
}

TEST(SefamDecodeChannel, AStubDecodesToNothing) {
    const std::vector<uint8_t> raw(101, 0x11);
    const SefamChannel ch = channel("SPO", 1);
    EXPECT_TRUE(SefamParser::decodeChannel(raw, 101, ch, SefamDescrambler{}).empty());
    EXPECT_TRUE(SefamParser::decodeChannel(raw, 200, ch, SefamDescrambler{}).empty());
}

// ── Events ──────────────────────────────────────────────────────────────────

TEST(SefamEvents, ARunOfOneCodeIsOneEvent) {
    const int freq = 25;
    std::vector<double> codes(freq * 60, 0);
    for (int i = 10 * freq; i < 30 * freq; ++i) codes[i] = 2;

    int distinct = 0;
    const auto start = std::chrono::system_clock::from_time_t(1'700'000'000);
    const auto events = SefamParser::decodeEvents(codes, freq, start, &distinct);

    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].event_type, EventType::OTHER);
    EXPECT_DOUBLE_EQ(events[0].duration_seconds, 20.0);
    EXPECT_EQ(events[0].timestamp, start + std::chrono::seconds(10));
    ASSERT_TRUE(events[0].details.has_value());
    EXPECT_EQ(*events[0].details, "Sefam detection code 2");
    EXPECT_EQ(distinct, 1);
}

// Every detection is OTHER. We do not know what a Sefam code means, and a guess
// would put an invented number into somebody's AHI. SDD-004 keeps OTHER inside
// total_events and inside no index, which is exactly the property wanted here.
TEST(SefamEvents, NothingIsClassifiedAsApneaOrHypopnea) {
    const int freq = 10;
    std::vector<double> codes(freq * 120, 0);
    for (int i = 0 * freq; i < 20 * freq; ++i) codes[i] = 1;
    for (int i = 40 * freq; i < 55 * freq; ++i) codes[i] = 2;
    for (int i = 80 * freq; i < 95 * freq; ++i) codes[i] = 9;

    int distinct = 0;
    const auto events = SefamParser::decodeEvents(
        codes, freq, std::chrono::system_clock::from_time_t(0), &distinct);

    ASSERT_EQ(events.size(), 3u);
    for (const auto& e : events) EXPECT_EQ(e.event_type, EventType::OTHER);
    EXPECT_EQ(distinct, 3);
}

TEST(SefamEvents, ABlipShorterThanASecondIsNotAnEvent) {
    // At 25 Hz a single sample is 40 ms. Nothing the device detects is over in
    // that time, and counting them would turn a noisy channel into a diagnosis.
    const int freq = 25;
    std::vector<double> codes(freq * 60, 0);
    for (int i = 100; i < 110; ++i) codes[i] = 3;          // 0.4 s
    for (int i = 20 * freq; i < 24 * freq; ++i) codes[i] = 4;  // 4 s

    const auto events = SefamParser::decodeEvents(
        codes, freq, std::chrono::system_clock::from_time_t(0), nullptr);

    ASSERT_EQ(events.size(), 1u);
    EXPECT_DOUBLE_EQ(events[0].duration_seconds, 4.0);
}

TEST(SefamEvents, AdjacentDifferentCodesAreSeparateEvents) {
    const int freq = 5;
    std::vector<double> codes(freq * 60, 0);
    for (int i = 5 * freq; i < 15 * freq; ++i) codes[i] = 2;
    for (int i = 15 * freq; i < 25 * freq; ++i) codes[i] = 7;

    const auto events = SefamParser::decodeEvents(
        codes, freq, std::chrono::system_clock::from_time_t(0), nullptr);

    ASSERT_EQ(events.size(), 2u);
    EXPECT_DOUBLE_EQ(events[0].duration_seconds, 10.0);
    EXPECT_DOUBLE_EQ(events[1].duration_seconds, 10.0);
    EXPECT_EQ(*events[1].details, "Sefam detection code 7");
}

TEST(SefamEvents, NoRateMeansNoEvents) {
    const std::vector<double> codes(100, 2);
    EXPECT_TRUE(SefamParser::decodeEvents(
        codes, 0, std::chrono::system_clock::from_time_t(0), nullptr).empty());
}

#endif // CPAPDASH_WITH_SEFAM
