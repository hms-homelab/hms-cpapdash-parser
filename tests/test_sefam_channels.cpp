#ifdef CPAPDASH_WITH_SEFAM

// The file header and the block framing, tested on their own.
//
// Both are measured from a real 242-session card rather than reasoned about, so
// these tests are mostly a record of what that card does -- including the two
// places where it contradicts what a reasonable person would have assumed.

#include <gtest/gtest.h>

#include "cpapdash/parser/SefamParser.h"

#include <cmath>
#include <string>
#include <vector>

using namespace cpapdash::parser;

namespace {

SefamChannel channel(const std::string& name, int freq, int bits = 8) {
    SefamChannel c;
    c.name = name;
    c.freq = freq;
    c.bits = bits;
    return c;
}

std::vector<uint8_t> header(const std::string& identity = "1263R24337476",
                            const std::string& stamp = "251110222516") {
    const size_t pad = kSefamFileHeaderBytes - (4 + 1 + stamp.size() + 1)
                     - identity.size();
    const std::string text = "#02/" + identity + std::string(pad, ' ') + "/" + stamp + "/";
    EXPECT_EQ(text.size(), kSefamFileHeaderBytes);

    std::vector<uint8_t> out;
    for (char c : text) out.push_back(static_cast<uint8_t>(c) ^ kSefamHeaderXor);
    return out;
}

// The block length these helpers write. Ten seconds is what the S.Box AUTO donor
// card uses; the SleepBox demo recording uses thirty, which is why the parser
// derives it rather than knowing it.
constexpr int kTestBlockSeconds = 10;

// The 1263R layout: ten-second blocks, three-byte trailers.
constexpr SefamBlockLayout kTestLayout{kTestBlockSeconds, 3};

// Wrap sample bytes the way the device does: a fixed number of seconds a block,
// then a checksum and a big-endian block index.
std::vector<uint8_t> frame(const std::vector<uint8_t>& samples, int freq,
                           int corrupt_block = -1, int first_index = 1,
                           int block_seconds = kTestBlockSeconds) {
    const size_t per_block = static_cast<size_t>(block_seconds) * freq;
    std::vector<uint8_t> out;
    int index = first_index;

    for (size_t at = 0; at < samples.size(); at += per_block) {
        const size_t take = std::min(per_block, samples.size() - at);
        unsigned sum = 0;
        for (size_t i = 0; i < take; ++i) {
            out.push_back(samples[at + i]);
            sum += samples[at + i];
        }
        uint8_t checksum = static_cast<uint8_t>(sum & 0xFF);
        if (index == corrupt_block) checksum ^= 0x5A;
        out.push_back(checksum);
        out.push_back(static_cast<uint8_t>((index >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>(index & 0xFF));
        ++index;
    }
    return out;
}

std::vector<uint8_t> file(const std::vector<uint8_t>& samples, int freq,
                          int corrupt_block = -1, int first_index = 1,
                          const std::string& identity = "1263R24337476",
                          int block_seconds = kTestBlockSeconds) {
    std::vector<uint8_t> out = header(identity);
    const auto framed = frame(samples, freq, corrupt_block, first_index, block_seconds);
    out.insert(out.end(), framed.begin(), framed.end());
    return out;
}

std::vector<uint8_t> ramp(size_t n, uint8_t from = 0) {
    std::vector<uint8_t> out;
    for (size_t i = 0; i < n; ++i) out.push_back(static_cast<uint8_t>(from + i));
    return out;
}

} // namespace

// ── The 38-byte header ──────────────────────────────────────────────────────

TEST(SefamFileHeader, DecodesToTheDeviceIdentityAndTheRecordingTime) {
    const auto h = SefamParser::decodeFileHeader(header());

    ASSERT_TRUE(h.valid);
    EXPECT_EQ(h.text, "#02/1263R24337476       /251110222516/");
    EXPECT_EQ(h.record_type, "#02");
    EXPECT_EQ(h.identity, "1263R24337476");
    ASSERT_TRUE(h.stamp.has_value());

    const std::time_t t = std::chrono::system_clock::to_time_t(*h.stamp);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif
    EXPECT_EQ(local.tm_year + 1900, 2025);
    EXPECT_EQ(local.tm_mon + 1, 11);
    EXPECT_EQ(local.tm_mday, 10);
    EXPECT_EQ(local.tm_hour, 22);
    EXPECT_EQ(local.tm_min, 25);
    EXPECT_EQ(local.tm_sec, 16);
}

// The obfuscation is a plain XOR, and 0x9F is where it shows: that is a space,
// which is why the padding in the middle of the header reads as a run of 0x9F
// bytes and why the whole file used to look scrambled.
TEST(SefamFileHeader, IsXor0xBF) {
    const auto raw = header();
    EXPECT_EQ(raw[0] ^ kSefamHeaderXor, '#');
    EXPECT_EQ(raw[17], 0x9F);
    EXPECT_EQ(raw[17] ^ kSefamHeaderXor, ' ');
}

TEST(SefamFileHeader, RejectsBytesThatDoNotDecodeToText) {
    std::vector<uint8_t> junk(kSefamFileHeaderBytes, 0x00);
    EXPECT_FALSE(SefamParser::decodeFileHeader(junk).valid);

    std::vector<uint8_t> tooShort(10, 0x9F);
    EXPECT_FALSE(SefamParser::decodeFileHeader(tooShort).valid);
}

// ── Block framing ───────────────────────────────────────────────────────────

TEST(SefamFraming, StripsTheTrailersAndKeepsEverySample) {
    const auto samples = ramp(250 * 3);  // three full blocks at 25 Hz
    const auto raw = file(samples, 25);

    SefamFraming f;
    const auto out = SefamParser::deframe(raw, channel("FLW", 25), kTestLayout, &f);

    EXPECT_EQ(out, samples);
    EXPECT_EQ(f.blocks, 3u);
    EXPECT_EQ(f.samples, 750u);
    EXPECT_EQ(f.checksum_failures, 0u);
    EXPECT_EQ(f.index_breaks, 0u);
}

// A block is ten seconds whatever the rate, so the block size differs per
// channel: 250 samples at 25 Hz, 10 at 1 Hz.
TEST(SefamFraming, BlockLengthFollowsTheChannelRate) {
    const auto samples = ramp(10 * 4);
    SefamFraming f;
    const auto out = SefamParser::deframe(file(samples, 1), channel("LK", 1),
                                          kTestLayout, &f);

    EXPECT_EQ(out, samples);
    EXPECT_EQ(f.blocks, 4u);
}

TEST(SefamFraming, TheLastShortBlockIsFramedLikeAnyOther) {
    const auto samples = ramp(250 * 2 + 37);
    SefamFraming f;
    const auto out = SefamParser::deframe(file(samples, 25), channel("FLW", 25), kTestLayout, &f);

    EXPECT_EQ(out, samples);
    EXPECT_EQ(f.blocks, 3u);
    EXPECT_EQ(f.samples, samples.size());
}

TEST(SefamFraming, ABadChecksumIsCounted) {
    const auto samples = ramp(250 * 4);
    SefamFraming f;
    SefamParser::deframe(file(samples, 25, /*corrupt_block=*/3), channel("FLW", 25), kTestLayout, &f);

    EXPECT_EQ(f.blocks, 4u);
    EXPECT_EQ(f.checksum_failures, 1u);
}

// Two sessions on the donor card open at block index 2, with every checksum
// correct. Requiring a first index of 1 would throw away two good nights, so
// continuity is checked and the starting value is not.
TEST(SefamFraming, AFirstIndexOtherThanOneIsFine) {
    const auto samples = ramp(250 * 3);
    SefamFraming f;
    const auto out = SefamParser::deframe(file(samples, 25, -1, /*first_index=*/2),
                                          channel("FLW", 25), kTestLayout, &f);

    EXPECT_EQ(out, samples);
    EXPECT_EQ(f.index_breaks, 0u);
    EXPECT_EQ(f.checksum_failures, 0u);
}

TEST(SefamFraming, ABreakInTheIndexSequenceIsCounted) {
    auto raw = file(ramp(250 * 3), 25);
    // Third block's index, in the trailer that follows it.
    const size_t third_trailer = kSefamFileHeaderBytes + 2 * (250 + 3) + 250;
    raw[third_trailer + 2] = 0x77;

    SefamFraming f;
    SefamParser::deframe(raw, channel("FLW", 25), kTestLayout, &f);
    EXPECT_EQ(f.index_breaks, 1u);
}

// A short run at the end of the file is either a legitimate final block or a
// write cut off mid-block, and the sizes alone cannot tell them apart. The
// checksum can: one that verifies is real, one that does not is a truncated
// tail, and dropping it costs at most ten seconds.
TEST(SefamFraming, ATruncatedTailIsDroppedRatherThanRead) {
    auto raw = file(ramp(250 * 3), 25);
    raw.resize(raw.size() - 2);  // eat into the last trailer

    SefamFraming f;
    const auto out = SefamParser::deframe(raw, channel("FLW", 25), kTestLayout, &f);
    EXPECT_EQ(f.blocks, 2u);
    EXPECT_EQ(out.size(), 500u);
    EXPECT_TRUE(f.truncated_tail);
    EXPECT_EQ(f.checksum_failures, 0u);
}

TEST(SefamFraming, AStubHasNoSamples) {
    SefamFraming f;
    EXPECT_TRUE(SefamParser::deframe(header(), channel("SPO", 1),
                                     kTestLayout, &f).empty());
    EXPECT_EQ(f.blocks, 0u);
}

// ── Physical values ─────────────────────────────────────────────────────────

// The INI's Min/Max do NOT scale a channel. Reading a flow channel declaring
// -180..280 as a quantisation of that span puts its centre at code 99.8; the
// donor card's flow is centred on 128 to within half a code, over a whole night.
TEST(SefamScaling, FlowIsCentredOnMidScaleNotOnTheDeclaredRange) {
    SefamChannel flw = channel("FLW", 25);
    flw.min = -180;
    flw.max = 280;

    const SefamScale s = SefamParser::scaleFor(flw);
    EXPECT_DOUBLE_EQ(s.offset, 128.0);
    EXPECT_DOUBLE_EQ(s.apply(128), 0.0);
    EXPECT_DOUBLE_EQ(s.apply(188), 60.0);
    EXPECT_DOUBLE_EQ(s.apply(68), -60.0);
}

TEST(SefamScaling, PressureAndLeakAreTenthsOfTheirUnit) {
    SefamChannel pre = channel("PRE", 5);
    pre.min = 0;
    pre.max = 255;
    EXPECT_NEAR(SefamParser::scaleFor(pre).apply(110), 11.0, 1e-9);

    SefamChannel lk = channel("LK", 1);
    lk.min = 0;
    lk.max = 153;
    EXPECT_NEAR(SefamParser::scaleFor(lk).apply(122), 12.2, 1e-9);
}

// None of the factors are confirmed. They are round numbers chosen because the
// alternative reading is physically impossible, which is evidence and not proof,
// and SDD-005's oracle step is what settles them.
TEST(SefamScaling, NothingClaimsToBeConfirmed) {
    for (const char* name : {"FLW", "PRE", "LK", "SPO", "HRT"})
        EXPECT_FALSE(SefamParser::scaleFor(channel(name, 1)).confirmed) << name;
}

TEST(SefamScaling, AnUnknownChannelIsPassedThroughUntouched) {
    const SefamScale s = SefamParser::scaleFor(channel("Y17", 25));
    EXPECT_DOUBLE_EQ(s.apply(200), 200.0);
}

TEST(SefamReadChannel, DeframesAndScalesTogether) {
    const std::vector<uint8_t> samples(10, 110);
    const auto out = SefamParser::readChannel(file(samples, 1), channel("PRE", 1),
                                              kTestLayout);

    ASSERT_EQ(out.size(), 10u);
    for (double v : out) EXPECT_NEAR(v, 11.0, 1e-9);
}

// ── The block framing is derived, not known ─────────────────────────────────
//
// Neither number is a constant of the format. The S.Box AUTO donor card uses
// ten-second blocks with a three-byte trailer; Sefam's own SleepBox_AUTO demo
// recording, model 1200R on older firmware, uses ten-second blocks with a
// ONE-byte trailer and no block index at all. A parser that hard-codes either
// reads the other as corrupt. The checksums settle it -- under a wrong layout
// the trailer bytes are samples and the sums do not match.

TEST(SefamBlockLayout, IsRecoveredFromTheChecksums) {
    for (int seconds : {1, 5, 10, 30, 60}) {
        const auto raw = file(ramp(25 * seconds * 4), 25, -1, 1,
                              "1263R24337476", seconds);
        const auto found = SefamParser::deduceBlockLayout(raw, channel("FLW", 25));
        ASSERT_TRUE(found.has_value()) << seconds;
        EXPECT_EQ(found->seconds, seconds);
        EXPECT_EQ(found->trailer_bytes, 3);
    }
}

TEST(SefamBlockLayout, AOneByteTrailerIsRecognised) {
    // The 1200R firmware writes the checksum alone, with no block index.
    std::vector<uint8_t> raw = header();
    const auto samples = ramp(250 * 5);
    for (size_t at = 0; at < samples.size(); at += 250) {
        unsigned sum = 0;
        for (size_t i = at; i < at + 250; ++i) { raw.push_back(samples[i]); sum += samples[i]; }
        raw.push_back(static_cast<uint8_t>(sum & 0xFF));
    }

    const auto found = SefamParser::deduceBlockLayout(raw, channel("FLW", 25));
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->seconds, 10);
    EXPECT_EQ(found->trailer_bytes, 1);
    EXPECT_FALSE(found->hasIndex());

    SefamFraming f;
    EXPECT_EQ(SefamParser::deframe(raw, channel("FLW", 25), *found, &f), samples);
    EXPECT_EQ(f.blocks, 5u);
    EXPECT_EQ(f.checksum_failures, 0u);
}

TEST(SefamBlockLayout, ASingleBlockCannotSettleIt) {
    // One block verifies under any layout long enough to hold it.
    const auto raw = file(ramp(250), 25);
    EXPECT_FALSE(SefamParser::deduceBlockLayout(raw, channel("FLW", 25)).has_value());
}

TEST(SefamBlockLayout, NoAnswerFromAStubOrFromNoise) {
    EXPECT_FALSE(SefamParser::deduceBlockLayout(header(), channel("FLW", 25)).has_value());

    auto noise = header();
    for (int i = 0; i < 4000; ++i) noise.push_back(static_cast<uint8_t>(i * 37 + 11));
    EXPECT_FALSE(SefamParser::deduceBlockLayout(noise, channel("FLW", 25)).has_value());
}

#endif // CPAPDASH_WITH_SEFAM
