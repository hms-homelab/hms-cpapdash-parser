#include <gtest/gtest.h>
#include "cpapdash/parser/AirSense11Settings.h"

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace cpapdash::parser;

// A tiny protobuf writer, so the fixtures are built from the documented field
// numbers and not from the reader under test. No real machine: every number
// here is made up.
namespace {

using Bytes = std::vector<uint8_t>;
using Clock = std::chrono::system_clock;

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

constexpr uint64_t kT0 = 1'780'000'000'000ULL;   // an applied time, ms

struct Profile {
    uint32_t slot;
    std::map<uint32_t, uint64_t> fields;   // field 1 is the mode
};

/// One settings record: applied at `applied_ms`, `active` the active slot.
Bytes record(uint64_t applied_ms, uint32_t active, const std::vector<Profile>& profiles,
             const std::vector<std::pair<uint32_t, std::map<uint32_t, uint64_t>>>& features = {}) {
    Bytes rec;
    Bytes attrs = fieldVarint(1, applied_ms);
    append(attrs, fieldVarint(4, 0));
    append(rec, fieldBytes(1, attrs));
    Bytes act = fieldVarint(1, active);
    append(act, fieldVarint(2, 3));
    append(act, fieldVarint(2, 5));
    append(rec, fieldBytes(2, act));
    Bytes ther;
    for (const auto& p : profiles) append(ther, fieldBytes(p.slot, message(p.fields)));
    append(rec, fieldBytes(3, ther));
    Bytes feat;
    for (const auto& [n, f] : features) append(feat, fieldBytes(n, message(f)));
    if (!feat.empty()) append(rec, fieldBytes(4, feat));
    return fieldBytes(3, rec);
}

std::optional<double> valueOf(const AirSense11SettingsRecord& r, const std::string& label) {
    for (const auto& [k, v] : r.values.values)
        if (k == label) return v;
    return std::nullopt;
}

Clock::time_point at(uint64_t ms) {
    return Clock::time_point(std::chrono::duration_cast<Clock::duration>(std::chrono::milliseconds(ms)));
}

const Profile kAutoSet{1, {{1, 2}, {2, 500}, {3, 700}, {4, 1500}}};
const Profile kVAuto{9, {{1, 7}, {2, 450}, {3, 1800}, {4, 600}, {5, 250}, {6, 1900}, {7, 400}, {8, 3}, {9, 4}}};
const Profile kAsv{7, {{1, 8}, {2, 400}, {3, 900}, {4, 700}, {5, 250}}};

}  // namespace

TEST(AirSense11Settings, LooksLikeASettingsSpoolOnly) {
    const Bytes spool = record(kT0, 1, {kAutoSet});
    EXPECT_TRUE(AirSense11SettingsParser::looksLike(spool.data(), spool.size()));

    // A Summary spool's records are field 2 at the top: not this.
    Bytes summary = fieldBytes(2, message({{1, 1}, {2, kT0}, {3, kT0 + 86'400'000ULL}, {5, 400}}));
    EXPECT_FALSE(AirSense11SettingsParser::looksLike(summary.data(), summary.size()));

    const Bytes junk = {0xFF, 0xFF, 0xFF};
    EXPECT_FALSE(AirSense11SettingsParser::looksLike(junk.data(), junk.size()));
    EXPECT_FALSE(AirSense11SettingsParser::looksLike(nullptr, 0));
}

TEST(AirSense11Settings, AutoSetRecordAsTheStrColumns) {
    const Bytes spool = record(kT0, 1, {kAutoSet, kAsv},
                               {{2, {{1, 2}, {2, 1}, {3, 2}, {4, 200}}},
                                {3, {{1, 2}, {2, 4500}}},
                                {5, {{1, 3}, {2, 3}, {3, 1}}},
                                {6, {{1, 1}, {2, 2}, {3, 5}, {4, 3}, {5, 2840}}}});
    const auto recs = AirSense11SettingsParser::parse(spool.data(), spool.size());
    ASSERT_EQ(recs.size(), 1u);
    const auto& r = recs[0];
    EXPECT_EQ(r.applied, at(kT0));
    EXPECT_EQ(r.active_profile, 1);
    EXPECT_EQ(r.active_mode, 2);

    EXPECT_EQ(valueOf(r, "Mode"), 1.0);   // AutoSet is the STR's Mode 1
    EXPECT_DOUBLE_EQ(*valueOf(r, "S.A.StartPress"), 5.0);
    EXPECT_DOUBLE_EQ(*valueOf(r, "S.A.MinPress"), 7.0);
    EXPECT_DOUBLE_EQ(*valueOf(r, "S.A.MaxPress"), 15.0);
    // Every profile the record carries is written, active or not, as the card does.
    EXPECT_DOUBLE_EQ(*valueOf(r, "S.AV.EPAP"), 9.0);
    EXPECT_DOUBLE_EQ(*valueOf(r, "S.AV.MinPS"), 2.5);

    EXPECT_EQ(valueOf(r, "S.EPR.ClinEnable"), 2.0);
    EXPECT_EQ(valueOf(r, "S.EPR.EPREnable"), 1.0);
    EXPECT_EQ(valueOf(r, "S.EPR.EPRType"), 2.0);
    EXPECT_DOUBLE_EQ(*valueOf(r, "S.EPR.Level"), 2.0);
    EXPECT_EQ(valueOf(r, "S.RampEnable"), 2.0);
    EXPECT_DOUBLE_EQ(*valueOf(r, "S.RampTime"), 45.0);
    EXPECT_EQ(valueOf(r, "S.Mask"), 3.0);
    EXPECT_EQ(valueOf(r, "S.Tube"), 3.0);
    EXPECT_EQ(valueOf(r, "S.ABFilter"), 1.0);
    EXPECT_EQ(valueOf(r, "S.ClimateControl"), 1.0);
    EXPECT_EQ(valueOf(r, "S.HumEnable"), 2.0);
    EXPECT_EQ(valueOf(r, "S.HumLevel"), 5.0);
    EXPECT_EQ(valueOf(r, "S.TempEnable"), 3.0);
    EXPECT_DOUBLE_EQ(*valueOf(r, "S.Temp"), 28.4);
}

TEST(AirSense11Settings, VAutoActiveIsMode8WithTimesInSeconds) {
    const Bytes spool = record(kT0, 9, {kAutoSet, kVAuto});
    const auto recs = AirSense11SettingsParser::parse(spool.data(), spool.size());
    ASSERT_EQ(recs.size(), 1u);
    const auto& r = recs[0];
    EXPECT_EQ(r.active_mode, 7);
    EXPECT_EQ(valueOf(r, "Mode"), 8.0);
    EXPECT_DOUBLE_EQ(*valueOf(r, "S.VA.StartPress"), 4.5);
    EXPECT_DOUBLE_EQ(*valueOf(r, "S.VA.MaxIPAP"), 18.0);
    EXPECT_DOUBLE_EQ(*valueOf(r, "S.VA.MinEPAP"), 6.0);
    EXPECT_DOUBLE_EQ(*valueOf(r, "S.VA.PS"), 2.5);
    EXPECT_DOUBLE_EQ(*valueOf(r, "S.VA.TiMax"), 1.9);
    EXPECT_DOUBLE_EQ(*valueOf(r, "S.VA.TiMin"), 0.4);
    EXPECT_EQ(valueOf(r, "S.VA.Trigger"), 3.0);
    EXPECT_EQ(valueOf(r, "S.VA.Cycle"), 4.0);
}

TEST(AirSense11Settings, ModeLeftOutWhenItsStrCodeIsUnknown) {
    // ASV active: its STR Mode code was never seen on a card.
    const Bytes spool = record(kT0, 7, {kAutoSet, kAsv});
    const auto recs = AirSense11SettingsParser::parse(spool.data(), spool.size());
    ASSERT_EQ(recs.size(), 1u);
    EXPECT_EQ(recs[0].active_mode, 8);
    EXPECT_FALSE(valueOf(recs[0], "Mode").has_value());
    EXPECT_TRUE(valueOf(recs[0], "S.A.MinPress").has_value());   // the rest is still written
}

TEST(AirSense11Settings, RecordsInOrderAndACutStreamKeepsTheWholeOnes) {
    Bytes spool = record(kT0, 1, {kAutoSet});
    Profile changed = kAutoSet;
    changed.fields[3] = 400;
    append(spool, record(kT0 + 60'000, 1, {changed}));
    const auto recs = AirSense11SettingsParser::parse(spool.data(), spool.size());
    ASSERT_EQ(recs.size(), 2u);
    EXPECT_DOUBLE_EQ(*valueOf(recs[0], "S.A.MinPress"), 7.0);
    EXPECT_DOUBLE_EQ(*valueOf(recs[1], "S.A.MinPress"), 4.0);

    const Bytes cut(spool.begin(), spool.end() - 5);
    const auto partial = AirSense11SettingsParser::parse(cut.data(), cut.size());
    ASSERT_EQ(partial.size(), 1u);
    EXPECT_EQ(partial[0].applied, at(kT0));
}

TEST(AirSense11Settings, ADayTakesTheLastChangeBeforeItEnds) {
    // Day D runs noon to noon. A change in D's evening is already in D's
    // record; a change after D's end belongs to the next day.
    const uint64_t day_end = kT0 + 10ULL * 3'600'000ULL;
    Bytes spool = record(kT0, 1, {kAutoSet});                              // before D
    Profile evening = kAutoSet;
    evening.fields[3] = 400;
    append(spool, record(kT0 + 6ULL * 3'600'000ULL, 1, {evening}));        // D's evening
    Profile later = kAutoSet;
    later.fields[3] = 900;
    append(spool, record(day_end + 1, 1, {later}));                         // after D
    const auto recs = AirSense11SettingsParser::parse(spool.data(), spool.size());
    ASSERT_EQ(recs.size(), 3u);

    const auto* d = AirSense11SettingsParser::inForce(recs, at(day_end));
    ASSERT_NE(d, nullptr);
    EXPECT_DOUBLE_EQ(*valueOf(*d, "S.A.MinPress"), 4.0);

    // A day that ended before the first record: the history does not reach it.
    EXPECT_EQ(AirSense11SettingsParser::inForce(recs, at(kT0)), nullptr);
    EXPECT_EQ(AirSense11SettingsParser::inForce({}, at(day_end)), nullptr);
}
