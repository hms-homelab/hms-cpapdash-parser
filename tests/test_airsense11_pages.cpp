// The AirSense 11's stored pages (SDD-012).
//
// The fixtures are pages captured from the bench AirSense 11 on 2026-11-17
// (the two 6.25 Hz pages cut to their first four records). The expected
// values come from the reference decoder that was matched against the same
// machine's card (r = 0.999, 7 of 7 events; SDD-012 FORMAT.md). The synthetic
// cases cover what the bench has not shown: a one-sample chunk, negative
// values, and every way a blob can be wrong.

#include <gtest/gtest.h>
#include "cpapdash/parser/AirSense11Pages.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numeric>

namespace cpapdash::parser {

namespace fs = std::filesystem;

static const std::string FIXTURES = "tests/fixtures/airsense11_pages";

#ifndef CPAPDASH_PARSER_SOURCE_DIR
#define CPAPDASH_PARSER_SOURCE_DIR "."
#endif

static std::vector<uint8_t> fixture(const std::string& name) {
    const std::string rel = FIXTURES + "/" + name;
    for (const std::string& base : {std::string(CPAPDASH_PARSER_SOURCE_DIR) + "/",
                                    std::string(""), std::string("../"), std::string("../../")}) {
        std::error_code ec;
        if (fs::exists(base + rel, ec)) {
            std::ifstream f(base + rel, std::ios::binary);
            return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
        }
    }
    return {};
}

struct Totals {
    size_t samples = 0;
    int64_t sum = 0;
    int32_t min = INT32_MAX, max = INT32_MIN;
};

static Totals totals(const AirSense11SignalPage& p) {
    Totals t;
    for (const auto& c : p.chunks) {
        t.samples += c.raw.size();
        for (int32_t v : c.raw) {
            t.sum += v;
            t.min = std::min(t.min, v);
            t.max = std::max(t.max, v);
        }
    }
    return t;
}

// ── the bench pages ─────────────────────────────────────────────────────────

TEST(AirSense11Pages, LeakPageDecodesEveryRecord) {
    auto b = fixture("Leak0p5Hz.bin");
    ASSERT_FALSE(b.empty()) << "fixture not found";
    auto p = AirSense11PagesParser::parseSignalPage(b.data(), b.size());
    EXPECT_EQ(p.skipped, 0) << p.first_error;
    ASSERT_EQ(p.chunks.size(), 19u);
    const auto& c = p.chunks.front();
    EXPECT_EQ(c.signal, AirSense11PageSignal::Leak);
    EXPECT_EQ(c.interval_ms, 2000);
    EXPECT_EQ(c.start_ms, 1793595319335LL);
    EXPECT_EQ(c.end_ms, 1793595399335LL);
    ASSERT_EQ(c.raw.size(), 41u);
    EXPECT_EQ((std::vector<int32_t>(c.raw.begin(), c.raw.begin() + 6)),
              (std::vector<int32_t>{1, 0, 0, 0, 0, 0}));
    EXPECT_EQ(p.chunks.back().end_ms, 1793907761949LL);
    auto t = totals(p);
    EXPECT_EQ(t.samples, 11218u);
    EXPECT_EQ(t.sum, 964);
    EXPECT_EQ(t.min, 0);
    EXPECT_EQ(t.max, 9);
}

TEST(AirSense11Pages, InspiratoryPressurePageDecodesEveryRecord) {
    auto b = fixture("InspiratoryPressure0p5Hz.bin");
    ASSERT_FALSE(b.empty()) << "fixture not found";
    auto p = AirSense11PagesParser::parseSignalPage(b.data(), b.size());
    EXPECT_EQ(p.skipped, 0) << p.first_error;
    ASSERT_EQ(p.chunks.size(), 19u);
    EXPECT_EQ(p.chunks.front().signal, AirSense11PageSignal::InspiratoryPressure);
    EXPECT_EQ(p.chunks.front().raw.front(), 20);
    EXPECT_DOUBLE_EQ(p.chunks.front().value(0), 4.0);   // 20 counts x 0.2 cmH2O
    auto t = totals(p);
    EXPECT_EQ(t.samples, 11218u);
    EXPECT_EQ(t.sum, 375038);
    EXPECT_EQ(t.min, 20);
    EXPECT_EQ(t.max, 59);
}

TEST(AirSense11Pages, FlowPageHasTheLongerHeaderAndK3) {
    auto b = fixture("RespiratoryFlow6p25Hz_first4.bin");
    ASSERT_FALSE(b.empty()) << "fixture not found";
    auto p = AirSense11PagesParser::parseSignalPage(b.data(), b.size());
    EXPECT_EQ(p.skipped, 0) << p.first_error;
    ASSERT_EQ(p.chunks.size(), 4u);
    const auto& c = p.chunks.front();
    EXPECT_EQ(c.signal, AirSense11PageSignal::Flow);
    EXPECT_EQ(c.interval_ms, 160);
    EXPECT_EQ(c.start_ms, 1793595317496LL);
    EXPECT_EQ(c.end_ms, 1793595364696LL);
    ASSERT_EQ(c.raw.size(), 296u);
    EXPECT_EQ((std::vector<int32_t>(c.raw.begin(), c.raw.begin() + 6)),
              (std::vector<int32_t>{8, 27, 31, 30, 27, 24}));
    EXPECT_EQ(c.raw.back(), -18);
    EXPECT_EQ(c.timeOf(c.raw.size() - 1), c.end_ms);
    EXPECT_NEAR(c.value(1), 0.54, 1e-12);                // 27 counts x 0.02 L/s
    auto t = totals(p);
    EXPECT_EQ(t.samples, 595u);
    EXPECT_EQ(t.sum, 331);
    EXPECT_EQ(t.min, -38);
    EXPECT_EQ(t.max, 150);
    EXPECT_EQ(p.chunks.back().end_ms, 1793772971606LL);
}

TEST(AirSense11Pages, MaskPressurePage) {
    auto b = fixture("MaskPressure6p25Hz_first4.bin");
    ASSERT_FALSE(b.empty()) << "fixture not found";
    auto p = AirSense11PagesParser::parseSignalPage(b.data(), b.size());
    EXPECT_EQ(p.skipped, 0) << p.first_error;
    ASSERT_EQ(p.chunks.size(), 4u);
    const auto& c = p.chunks.front();
    EXPECT_EQ(c.signal, AirSense11PageSignal::MaskPressure);
    ASSERT_EQ(c.raw.size(), 437u);
    EXPECT_EQ((std::vector<int32_t>(c.raw.begin(), c.raw.begin() + 6)),
              (std::vector<int32_t>{7, 20, 20, 21, 21, 21}));
    EXPECT_EQ(c.raw.back(), 20);
    auto t = totals(p);
    EXPECT_EQ(t.samples, 595u);
    EXPECT_EQ(t.sum, 11737);
    EXPECT_EQ(t.min, 0);
    EXPECT_EQ(t.max, 22);
}

TEST(AirSense11Pages, APageCutInsideARecordYieldsTheWholeOnes) {
    auto b = fixture("Leak0p5Hz.bin");
    ASSERT_FALSE(b.empty()) << "fixture not found";
    auto p = AirSense11PagesParser::parseSignalPage(b.data(), b.size() - 10);
    EXPECT_EQ(p.chunks.size(), 18u);
    EXPECT_EQ(p.skipped, 0);
}

TEST(AirSense11Pages, EventsPage) {
    auto b = fixture("TherapyEvents-RespiratoryEvents.bin");
    ASSERT_FALSE(b.empty()) << "fixture not found";
    auto ev = AirSense11PagesParser::parseEventsPage(b.data(), b.size());
    const std::vector<std::pair<int, int64_t>> want = {
        {3, 1793778395375LL}, {3, 1793778434574LL}, {3, 1793782701777LL}, {4, 1793783799777LL},
        {3, 1793783853078LL}, {4, 1793789615480LL}, {3, 1793792286882LL}, {3, 1793850319914LL}};
    const std::vector<int64_t> dur = {11000, 13000, 12000, 14000, 11000, 10000, 11000, 10000};
    ASSERT_EQ(ev.size(), want.size());
    for (size_t i = 0; i < ev.size(); i++) {
        EXPECT_EQ(ev[i].type, want[i].first) << i;
        EXPECT_EQ(ev[i].onset_ms, want[i].second) << i;
        EXPECT_EQ(ev[i].field3_ms, want[i].second) << i;
        EXPECT_EQ(ev[i].duration_ms, dur[i]) << i;
    }
    EXPECT_STREQ(AirSense11PagesParser::eventName(3), "Central Apnea");
    EXPECT_STREQ(AirSense11PagesParser::eventName(4), "Obstructive Apnea");
    EXPECT_STREQ(AirSense11PagesParser::eventName(7), "");
}

// ── synthetic blobs ─────────────────────────────────────────────────────────

namespace {

void putVarint(std::vector<uint8_t>& o, uint64_t v) {
    while (v >= 0x80) { o.push_back(static_cast<uint8_t>(v | 0x80)); v >>= 7; }
    o.push_back(static_cast<uint8_t>(v));
}

void putBytes(std::vector<uint8_t>& o, uint32_t field, const std::vector<uint8_t>& b) {
    putVarint(o, (static_cast<uint64_t>(field) << 3) | 2);
    putVarint(o, b.size());
    o.insert(o.end(), b.begin(), b.end());
}

void putInt(std::vector<uint8_t>& o, uint32_t field, uint64_t v) {
    putVarint(o, static_cast<uint64_t>(field) << 3);
    putVarint(o, v);
}

// An RC03 blob as the machine writes one: an 11-byte header, two int16
// samples, Rice codes of the second-order residuals, 1-padding.
std::vector<uint8_t> encodeRc03(const std::vector<int32_t>& x, uint8_t d = 16) {
    int k = -1;
    for (uint8_t v = d; v > 1; v >>= 1) k++;
    std::vector<uint8_t> o = {11, 'R', 'C', '0', '3', 0x04, 0x01, 0x00, 0xF4, 0x03, d, 0x02};
    auto i16 = [&o](int32_t v) {
        o.push_back(static_cast<uint8_t>(v & 0xFF));
        o.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    };
    if (!x.empty()) i16(x[0]);
    if (x.size() > 1) i16(x[1]);
    std::vector<int> bits;
    for (size_t n = 2; n < x.size(); n++) {
        const int64_t res = x[n] - (2 * static_cast<int64_t>(x[n - 1]) - x[n - 2]);
        const uint64_t u = res >= 0 ? static_cast<uint64_t>(res) * 2 : static_cast<uint64_t>(-res) * 2 - 1;
        for (uint64_t q = u >> k; q; q--) bits.push_back(1);
        bits.push_back(0);
        for (int b = k - 1; b >= 0; b--) bits.push_back(static_cast<int>((u >> b) & 1));
    }
    while (bits.size() % 8) bits.push_back(1);
    for (size_t i = 0; i < bits.size(); i += 8) {
        uint8_t byte = 0;
        for (int j = 0; j < 8; j++) byte = static_cast<uint8_t>((byte << 1) | bits[i + j]);
        o.push_back(byte);
    }
    return o;
}

std::vector<uint8_t> record(uint32_t signal, int64_t iv, int64_t start, int64_t end,
                            const std::vector<uint8_t>& blob) {
    std::vector<uint8_t> inner, outer, page;
    putInt(inner, 1, static_cast<uint64_t>(iv));
    putInt(inner, 2, static_cast<uint64_t>(start));
    putInt(inner, 3, static_cast<uint64_t>(end));
    putBytes(inner, 4, blob);
    putInt(outer, 1, 1);
    putBytes(outer, 2, inner);
    putBytes(page, signal, outer);
    return page;
}

}  // namespace

TEST(AirSense11Pages, RoundTripsNegativeValuesAndLargeSteps) {
    const std::vector<int32_t> x = {-5, -3, 0, 40, -60, -61, 300, -300, 7, 7, 7};
    for (uint8_t d : {uint8_t(4), uint8_t(16)}) {
        auto blob = encodeRc03(x, d);
        std::string err;
        auto got = AirSense11PagesParser::decodeRc03(blob.data(), blob.size(), x.size(), &err);
        ASSERT_TRUE(got) << err;
        EXPECT_EQ(*got, x);
    }
}

TEST(AirSense11Pages, OneAndTwoSampleChunks) {
    for (const std::vector<int32_t>& x : {std::vector<int32_t>{12}, std::vector<int32_t>{12, -4}}) {
        auto blob = encodeRc03(x);
        auto got = AirSense11PagesParser::decodeRc03(blob.data(), blob.size(), x.size());
        ASSERT_TRUE(got);
        EXPECT_EQ(*got, x);
    }
    // One sample with the second slot still written.
    auto blob = encodeRc03({12, 0});
    auto got = AirSense11PagesParser::decodeRc03(blob.data(), blob.size(), 1);
    ASSERT_TRUE(got);
    EXPECT_EQ(*got, std::vector<int32_t>{12});
}

TEST(AirSense11Pages, RefusesABlobThatIsNotRc03) {
    auto blob = encodeRc03({1, 2, 3});
    blob[2] = 'X';
    std::string err;
    EXPECT_FALSE(AirSense11PagesParser::decodeRc03(blob.data(), blob.size(), 3, &err));
    EXPECT_EQ(err, "not an RC03 blob");
}

TEST(AirSense11Pages, RefusesARiceByteThatIsNotAPowerOfTwo) {
    auto blob = encodeRc03({1, 2, 3});
    blob[10] = 12;
    std::string err;
    EXPECT_FALSE(AirSense11PagesParser::decodeRc03(blob.data(), blob.size(), 3, &err));
    EXPECT_EQ(err, "RC03 Rice byte is not a power of two");
}

TEST(AirSense11Pages, RefusesTheWrongSampleCount) {
    const std::vector<int32_t> x = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    auto blob = encodeRc03(x);
    std::string err;
    // One too many: the bitstream runs out.
    EXPECT_FALSE(AirSense11PagesParser::decodeRc03(blob.data(), blob.size(), x.size() + 1, &err));
    // One too few: a 0 is left in the padding.
    EXPECT_FALSE(AirSense11PagesParser::decodeRc03(blob.data(), blob.size(), x.size() - 1, &err));
    // Far too many: refused before anything is allocated.
    EXPECT_FALSE(AirSense11PagesParser::decodeRc03(blob.data(), blob.size(), size_t(1) << 40, &err));
    EXPECT_EQ(err, "RC03 bitstream too short for the sample count");
}

TEST(AirSense11Pages, RefusesBytesAfterTheLastSample) {
    auto blob = encodeRc03({1, 2, 3});
    blob.push_back(0xFF);
    std::string err;
    EXPECT_FALSE(AirSense11PagesParser::decodeRc03(blob.data(), blob.size(), 3, &err));
    EXPECT_EQ(err, "RC03 blob has bytes after its last sample");
}

TEST(AirSense11Pages, ABadRecordIsSkippedAndTheRestAreRead) {
    const std::vector<int32_t> a = {10, 10, 11, 12}, b = {3, 3, 3};
    std::vector<uint8_t> page = record(20, 2000, 1000, 7000, encodeRc03(a, 4));
    auto bad = record(20, 2000, 9000, 10000, encodeRc03(b, 4));   // 1000 ms span, 2000 ms grid
    page.insert(page.end(), bad.begin(), bad.end());
    auto good = record(21, 2000, 9000, 13000, encodeRc03(b, 4));
    page.insert(page.end(), good.begin(), good.end());

    auto p = AirSense11PagesParser::parseSignalPage(page.data(), page.size());
    EXPECT_EQ(p.skipped, 1);
    EXPECT_EQ(p.first_error, "record span is not a whole number of intervals");
    ASSERT_EQ(p.chunks.size(), 2u);
    EXPECT_EQ(p.chunks[0].raw, a);
    EXPECT_EQ(p.chunks[1].signal, AirSense11PageSignal::InspiratoryPressure);
    EXPECT_EQ(p.chunks[1].raw, b);
}

TEST(AirSense11Pages, AnUnknownSignalIsStillDecodedInCounts) {
    auto page = record(30, 1000, 0, 2000, encodeRc03({4, 5, 6}, 4));
    auto p = AirSense11PagesParser::parseSignalPage(page.data(), page.size());
    ASSERT_EQ(p.chunks.size(), 1u);
    EXPECT_EQ(p.chunks[0].signal, AirSense11PageSignal::Unknown);
    EXPECT_DOUBLE_EQ(p.chunks[0].value(2), 6.0);
    EXPECT_STREQ(AirSense11PagesParser::unit(AirSense11PageSignal::Unknown), "");
}

}  // namespace cpapdash::parser
