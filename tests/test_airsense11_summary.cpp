#include <gtest/gtest.h>
#include "cpapdash/parser/AirSense11Summary.h"

#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

using namespace cpapdash::parser;

// A tiny protobuf writer, so the fixtures are built from the documented field
// numbers and not from the reader under test. Same shape as the protocol
// documenters' test writer. No real night: every number here is made up.
namespace {

using Bytes = std::vector<uint8_t>;

Bytes varint(uint64_t n) {
    Bytes out;
    do {
        uint8_t b = n & 0x7F;
        n >>= 7;
        out.push_back(n ? (b | 0x80) : b);
    } while (n);
    return out;
}

void append(Bytes& to, const Bytes& b) { to.insert(to.end(), b.begin(), b.end()); }

Bytes fieldVarint(uint32_t num, uint64_t v) {
    Bytes out = varint(static_cast<uint64_t>(num) << 3);
    append(out, varint(v));
    return out;
}

Bytes fieldBytes(uint32_t num, const Bytes& v) {
    Bytes out = varint((static_cast<uint64_t>(num) << 3) | 2);
    append(out, varint(v.size()));
    append(out, v);
    return out;
}

Bytes message(const std::map<uint32_t, uint64_t>& members) {
    Bytes out;
    for (const auto& [k, v] : members) append(out, fieldVarint(k, v));
    return out;
}

constexpr uint64_t kDay = 86'400'000ULL;

/// One day record as the machine writes it, in hundredths: start_ms is the
/// machine's noon. `usage` 0 is a day the machine was not used.
Bytes dayRecord(uint64_t start_ms, int usage, const std::vector<std::pair<uint64_t, int>>& sessions,
                bool with_extras = true) {
    Bytes rec;
    append(rec, fieldVarint(1, 1));
    append(rec, fieldVarint(2, start_ms));
    append(rec, fieldVarint(3, start_ms + kDay));
    append(rec, fieldVarint(4, 120));
    append(rec, fieldVarint(5, static_cast<uint64_t>(usage)));
    Bytes sess;
    for (const auto& [s, m] : sessions)
        append(sess, fieldBytes(1, message({{1, s}, {2, static_cast<uint64_t>(m)}})));
    append(rec, fieldBytes(6, sess));
    append(rec, fieldVarint(7, 90));    // AHI 0.90
    append(rec, fieldVarint(8, 70));    // AI 0.70
    append(rec, fieldVarint(9, 10));    // HI 0.10
    append(rec, fieldVarint(10, 30));   // OAI 0.30
    append(rec, fieldVarint(11, 30));   // CAI 0.30
    append(rec, fieldVarint(12, 0));    // UAI
    append(rec, fieldVarint(13, 0));    // RIN
    append(rec, fieldBytes(14, message({{2, 20}, {3, 24}, {4, 44}, {5, 68}})));     // leak L/s
    append(rec, fieldBytes(15, message({{2, 1020}, {3, 1140}, {4, 1260}})));        // TgtIPAP
    append(rec, fieldVarint(16, 250));  // CSR 2.5 min
    append(rec, fieldBytes(20, message({{2, 720}, {3, 852}, {4, 960}})));           // TgtEPAP
    append(rec, fieldBytes(21, message({{2, 800}, {3, 912}, {4, 1008}})));          // MaskPress
    append(rec, fieldBytes(22, message({{2, 48}, {3, 80}, {4, 112}})));             // TidVol L
    append(rec, fieldBytes(23, message({{2, 725}, {3, 1113}, {4, 1450}})));         // MinVent
    append(rec, fieldBytes(25, message({{2, 1440}, {3, 1920}, {4, 2960}})));        // RespRate
    if (with_extras) {
        append(rec, fieldBytes(29, message({{2, 1560}})));
        append(rec, fieldBytes(30, message({{2, 2320}})));
        append(rec, fieldBytes(32, message({{2, 760}})));
        append(rec, fieldBytes(36, message({{1, 474}, {3, 1090}})));                // BlowPress 5/95
        append(rec, fieldBytes(37, message({{1, 16}, {3, 50}})));                   // Flow 5/95
        append(rec, fieldBytes(38, message({{2, 64}})));                            // BlowFlow
    }
    append(rec, fieldVarint(39, sessions.size()));
    append(rec, fieldVarint(40, start_ms + 50'000'000));
    return fieldBytes(2, rec);
}

std::chrono::system_clock::time_point tp(uint64_t ms) {
    return std::chrono::system_clock::time_point(
        std::chrono::duration_cast<std::chrono::system_clock::duration>(
            std::chrono::milliseconds(static_cast<int64_t>(ms))));
}

// 2026-03-01 12:00:00 UTC in ms.
constexpr uint64_t kNoon = 1'772'366'400'000ULL;

}  // namespace

TEST(AirSense11Summary, ADayRecordReadsBackInStrUnits) {
    const Bytes spool = dayRecord(kNoon, 412, {{kNoon + 36'000'000, 200}, {kNoon + 50'000'000, 212}});
    ASSERT_TRUE(AirSense11SummaryParser::looksLike(spool.data(), spool.size()));
    const auto days = AirSense11SummaryParser::parse(spool.data(), spool.size());
    ASSERT_EQ(days.size(), 1u);
    const auto& d = days[0];
    EXPECT_EQ(d.day_start, tp(kNoon));
    EXPECT_EQ(d.day_end, tp(kNoon + kDay));
    EXPECT_EQ(d.usage_minutes, 412);
    EXPECT_TRUE(d.used());
    ASSERT_EQ(d.sessions.size(), 2u);
    EXPECT_EQ(d.sessions[0].start, tp(kNoon + 36'000'000));
    EXPECT_EQ(d.sessions[0].minutes, 200);
    EXPECT_EQ(d.sessions[1].minutes, 212);
    EXPECT_EQ(d.mask_events, 2);
    ASSERT_TRUE(d.ahi);
    EXPECT_DOUBLE_EQ(*d.ahi, 0.90);
    EXPECT_DOUBLE_EQ(*d.ai, 0.70);
    EXPECT_DOUBLE_EQ(*d.hi, 0.10);
    EXPECT_DOUBLE_EQ(*d.oai, 0.30);
    EXPECT_DOUBLE_EQ(*d.cai, 0.30);
    EXPECT_DOUBLE_EQ(*d.uai, 0.0);
    EXPECT_DOUBLE_EQ(*d.csr_minutes, 2.5);
    EXPECT_DOUBLE_EQ(*d.leak_lps.p50, 0.20);
    EXPECT_DOUBLE_EQ(*d.leak_lps.p70, 0.24);
    EXPECT_DOUBLE_EQ(*d.leak_lps.p95, 0.44);
    EXPECT_DOUBLE_EQ(*d.leak_lps.max, 0.68);
    EXPECT_DOUBLE_EQ(*d.target_ipap.p50, 10.20);
    EXPECT_DOUBLE_EQ(*d.target_ipap.max, 12.60);
    EXPECT_DOUBLE_EQ(*d.target_epap.p95, 8.52);
    EXPECT_DOUBLE_EQ(*d.mask_pressure.p50, 8.00);
    EXPECT_DOUBLE_EQ(*d.tidal_volume_l.p95, 0.80);
    EXPECT_DOUBLE_EQ(*d.minute_vent.max, 14.50);
    EXPECT_DOUBLE_EQ(*d.resp_rate.p50, 14.40);
    EXPECT_DOUBLE_EQ(*d.blower_pressure.p5, 4.74);
    EXPECT_DOUBLE_EQ(*d.blower_pressure.p95, 10.90);
    EXPECT_FALSE(d.blower_pressure.p50) << "the blower set has no median";
    EXPECT_DOUBLE_EQ(*d.flow_lps.p5, 0.16);
    EXPECT_DOUBLE_EQ(*d.blower_flow_lps, 0.64);
    EXPECT_DOUBLE_EQ(*d.ambient_humidity, 15.60);
    EXPECT_DOUBLE_EQ(*d.humidifier_temp, 23.20);
    EXPECT_FALSE(d.heated_tube_temp) << "not carried: not invented";
    EXPECT_DOUBLE_EQ(*d.humidifier_power, 7.60);
    ASSERT_TRUE(d.updated);
    EXPECT_EQ(*d.updated, tp(kNoon + 50'000'000));
}

TEST(AirSense11Summary, TheStrRecordIsWhatTheEdfReaderWouldReturn) {
    const Bytes spool = dayRecord(kNoon, 412, {{kNoon + 36'000'000, 200}, {kNoon + 50'000'000, 212}});
    const auto days = AirSense11SummaryParser::parse(spool.data(), spool.size());
    ASSERT_EQ(days.size(), 1u);
    const STRDailyRecord r = AirSense11SummaryParser::toStrRecord(days[0], "SER-11");
    EXPECT_EQ(r.device_id, "SER-11");
    EXPECT_EQ(r.record_date, tp(kNoon)) << "the machine's noon, as the EDF reader's noon";
    EXPECT_DOUBLE_EQ(r.duration_minutes, 412);
    EXPECT_EQ(r.mask_events, 2);
    ASSERT_EQ(r.mask_pairs.size(), 2u);
    EXPECT_EQ(r.mask_pairs[0].first, tp(kNoon + 36'000'000));
    EXPECT_EQ(r.mask_pairs[0].second, tp(kNoon + 36'000'000) + std::chrono::minutes(200));
    EXPECT_DOUBLE_EQ(r.ahi, 0.90);
    EXPECT_DOUBLE_EQ(r.oai, 0.30);
    EXPECT_DOUBLE_EQ(r.csr, 2.5);
    // Leak: the EDF reader multiplies the file's L/s by 60; the summary carries
    // L/s like the file, so the record is in L/min like the reader's.
    EXPECT_DOUBLE_EQ(r.leak_50, 12.0);
    EXPECT_DOUBLE_EQ(r.leak_70, 14.4);
    EXPECT_DOUBLE_EQ(r.leak_95, 26.4);
    EXPECT_DOUBLE_EQ(r.leak_max, 40.8);
    EXPECT_DOUBLE_EQ(r.mask_press_50, 8.00);
    EXPECT_DOUBLE_EQ(r.mask_press_95, 9.12);
    EXPECT_DOUBLE_EQ(r.mask_press_max, 10.08);
    EXPECT_DOUBLE_EQ(r.blow_press_5, 4.74);
    EXPECT_DOUBLE_EQ(r.blow_press_95, 10.90);
    EXPECT_DOUBLE_EQ(r.tid_vol_50, 0.48);
    EXPECT_DOUBLE_EQ(r.min_vent_95, 11.13);
    EXPECT_DOUBLE_EQ(r.resp_rate_max, 29.60);
    ASSERT_TRUE(r.tgt_ipap_50);
    EXPECT_DOUBLE_EQ(*r.tgt_ipap_50, 10.20);
    EXPECT_DOUBLE_EQ(*r.tgt_epap_max, 9.60);
    EXPECT_FALSE(r.tgt_vent_50) << "the summary carries no ventilation targets";
    // What the spool does not carry stays at the reader's defaults.
    EXPECT_EQ(r.mode, 0);
    EXPECT_EQ(r.family, STRDailyRecord::Family::Unknown);
    EXPECT_DOUBLE_EQ(r.pressure_setting, 0);
    EXPECT_DOUBLE_EQ(r.spo2_50, 0);
}

TEST(AirSense11Summary, ADayWithoutUsageCarriesNoIndicesAndNoStrRecord) {
    Bytes spool = dayRecord(kNoon, 0, {});
    append(spool, dayRecord(kNoon + kDay, 300, {{kNoon + kDay + 40'000'000, 300}}));
    const auto days = AirSense11SummaryParser::parse(spool.data(), spool.size());
    ASSERT_EQ(days.size(), 2u) << "oldest first, as stored";
    EXPECT_FALSE(days[0].used());
    EXPECT_FALSE(days[0].ahi) << "the zeros of a day off are not a perfect night";
    EXPECT_FALSE(days[0].csr_minutes);
    EXPECT_TRUE(days[1].used());
    EXPECT_TRUE(days[1].ahi);

    const auto recs = AirSense11SummaryParser::toStrRecords(days, "SER-11");
    ASSERT_EQ(recs.size(), 1u) << "as parseSTR skips a record without therapy";
    EXPECT_EQ(recs[0].record_date, tp(kNoon + kDay));
    EXPECT_DOUBLE_EQ(recs[0].duration_minutes, 300);
}

TEST(AirSense11Summary, UnknownFieldsAreSkippedAndACutStreamYieldsTheWholeDays) {
    // A field this reader has never heard of, in every wire type, inside and
    // outside a day: a later firmware must not break the read.
    Bytes rec_inner;
    append(rec_inner, fieldVarint(2, kNoon));
    append(rec_inner, fieldVarint(5, 100));
    append(rec_inner, fieldVarint(77, 5));                               // varint
    append(rec_inner, fieldBytes(78, {1, 2, 3}));                        // bytes
    Bytes fixed32 = varint((79ULL << 3) | 5); append(fixed32, {1, 2, 3, 4});
    append(rec_inner, fixed32);                                          // 32-bit
    Bytes fixed64 = varint((80ULL << 3) | 1); append(fixed64, {1, 2, 3, 4, 5, 6, 7, 8});
    append(rec_inner, fixed64);                                          // 64-bit
    append(rec_inner, fieldVarint(7, 150));
    Bytes spool = fieldBytes(2, rec_inner);
    append(spool, fieldVarint(9, 42));                                   // a top-level stranger
    append(spool, dayRecord(kNoon + kDay, 200, {{kNoon + kDay + 1000, 200}}));

    auto days = AirSense11SummaryParser::parse(spool.data(), spool.size());
    ASSERT_EQ(days.size(), 2u);
    EXPECT_EQ(days[0].usage_minutes, 100);
    EXPECT_DOUBLE_EQ(*days[0].ahi, 1.50);
    EXPECT_EQ(days[1].usage_minutes, 200);

    // Cut in the middle of the second day: the first survives, nothing throws.
    const Bytes cut(spool.begin(), spool.end() - 20);
    days = AirSense11SummaryParser::parse(cut.data(), cut.size());
    ASSERT_EQ(days.size(), 1u);
    EXPECT_EQ(days[0].usage_minutes, 100);

    // A varint that never ends, and a length past the end: the same.
    const Bytes endless = {0x12, 0x05, 0x10, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    EXPECT_TRUE(AirSense11SummaryParser::parse(endless.data(), endless.size()).empty());
    const Bytes overrun = {0x12, 0x7F, 0x10, 0x01};
    EXPECT_TRUE(AirSense11SummaryParser::parse(overrun.data(), overrun.size()).empty());
}

TEST(AirSense11Summary, LooksLikeIsDecidedByTheBytes) {
    const Bytes spool = dayRecord(kNoon, 412, {{kNoon + 36'000'000, 412}});
    EXPECT_TRUE(AirSense11SummaryParser::looksLike(spool.data(), spool.size()));
    EXPECT_FALSE(AirSense11SummaryParser::looksLike(nullptr, 0));
    const Bytes edf = {'0', ' ', ' ', ' ', ' ', ' ', ' ', ' ', 'X'};
    EXPECT_FALSE(AirSense11SummaryParser::looksLike(edf.data(), edf.size()));
    const Bytes o2s = {0x01, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 96, 60, 0};
    EXPECT_FALSE(AirSense11SummaryParser::looksLike(o2s.data(), o2s.size()));
    const Bytes vld = {0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    EXPECT_FALSE(AirSense11SummaryParser::looksLike(vld.data(), vld.size()));
    // A day with no start is no day.
    const Bytes no_start = fieldBytes(2, message({{5, 300}, {7, 90}}));
    EXPECT_FALSE(AirSense11SummaryParser::looksLike(no_start.data(), no_start.size()));
    EXPECT_STREQ(AirSense11SummaryParser::kExtension, ".as11");
}
