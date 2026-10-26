// One day of an STR, written into the layout of an STR the machine wrote, so a
// day the machine reported over its Bluetooth (its summary) can sit among the
// days its card carried. These tests prove the record is what this library's
// own STR reader takes back: the summary's figures read back equal, what the
// summary does not carry stays the base record's, and the record's Date and
// Crc16 are the machine's. Every number here is made up.
#include <gtest/gtest.h>

#include "cpapdash/parser/AirSense11Summary.h"
#include "cpapdash/parser/EDFFile.h"
#include "cpapdash/parser/EDFParser.h"
#include "cpapdash/parser/EDFWriter.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

using namespace cpapdash::parser;

namespace {

struct Sig {
    const char* label;
    const char* dim;
    double      pmin, pmax;
    int         dmin, dmax, spr;
};

// The signal layout an AirSense 11 writes to STR.edf: labels, units, ranges and
// samples per record as the machine's own header carries them (78 signals).
const std::vector<Sig>& as11Layout() {
    static const std::vector<Sig> l = {
        {"Date", "", 0, 24836, 0, 24836, 1},
        {"MaskOn", "MINUTES", 0, 1440, 0, 1440, 20},
        {"MaskOff", "MINUTES", 0, 1440, 0, 1440, 20},
        {"MaskEvents", "", 0, 255, 0, 255, 1},
        {"Duration", "min.", 0, 1440, 0, 1440, 1},
        {"Mode", "", 0, 16, 0, 16, 1},
        {"S.C.StartPress", "cmH2O", 4, 20, 200, 1000, 1},
        {"S.C.Press", "cmH2O", 4, 20, 200, 1000, 1},
        {"S.A.StartPress", "cmH2O", 4, 20, 200, 1000, 1},
        {"S.A.MaxPress", "cmH2O", 4, 20, 200, 1000, 1},
        {"S.A.MinPress", "cmH2O", 4, 20, 200, 1000, 1},
        {"S.AFH.StartPress", "cmH2O", 4, 20, 200, 1000, 1},
        {"S.AFH.MaxPress", "cmH2O", 4, 20, 200, 1000, 1},
        {"S.AFH.MinPress", "cmH2O", 4, 20, 200, 1000, 1},
        {"S.AS.Comfort", "", 0, 16, 0, 16, 1},
        {"S.RampEnable", "", 0, 16, 0, 16, 1},
        {"S.RampTime", "min.", 5, 45, 5, 45, 1},
        {"S.EPR.ClinEnable", "", 0, 16, 0, 16, 1},
        {"S.EPR.EPREnable", "", 0, 16, 0, 16, 1},
        {"S.EPR.Level", "cmH2O", 1, 3, 50, 150, 1},
        {"S.EPR.EPRType", "", 0, 16, 0, 16, 1},
        {"S.SmartStart", "", 0, 16, 0, 16, 1},
        {"S.PtAccess", "", 0, 16, 0, 16, 1},
        {"S.ABFilter", "", 0, 16, 0, 16, 1},
        {"S.Mask", "", 0, 16, 0, 16, 1},
        {"S.Tube", "", 0, 16, 0, 16, 1},
        {"S.ClimateControl", "", 0, 16, 0, 16, 1},
        {"S.HumEnable", "", 0, 16, 0, 16, 1},
        {"S.HumLevel", "", 1, 8, 1, 8, 1},
        {"S.TempEnable", "", 0, 16, 0, 16, 1},
        {"S.Temp", "Celsius", 15.6, 30, 156, 300, 1},
        {"HeatedTube", "", 0, 16, 0, 16, 1},
        {"Humidifier", "", 0, 16, 0, 16, 1},
        {"BlowPress.95", "cmH2O", -10, 45, -500, 2250, 1},
        {"BlowPress.5", "cmH2O", -10, 45, -500, 2250, 1},
        {"Flow.95", "L/s", -2, 3, -1000, 1500, 1},
        {"Flow.5", "L/s", -2, 3, -1000, 1500, 1},
        {"BlowFlow.50", "L/s", -4, 4, -2000, 2000, 1},
        {"AmbHumidity.50", "mg/L", 0, 100, 0, 1000, 1},
        {"HumTemp.50", "Celsius", 0, 100, 0, 1000, 1},
        {"HTubeTemp.50", "Celsius", 0, 40, 0, 400, 1},
        {"HTubePow.50", "%", 0, 100, 0, 1000, 1},
        {"HumPow.50", "%", 0, 100, 0, 1000, 1},
        {"SpO2.50", "%", 0, 100, 0, 100, 1},
        {"SpO2.95", "%", 0, 100, 0, 100, 1},
        {"SpO2.Max", "%", 0, 100, 0, 100, 1},
        {"SpO2Thresh", "min.", 0, 1440, 0, 1440, 1},
        {"MaskPress.50", "cmH2O", 0, 40, 0, 2000, 1},
        {"MaskPress.95", "cmH2O", 0, 40, 0, 2000, 1},
        {"MaskPress.Max", "cmH2O", 0, 40, 0, 2000, 1},
        {"TgtIPAP.50", "cmH2O", 0, 50, 0, 2500, 1},
        {"TgtIPAP.95", "cmH2O", 0, 50, 0, 2500, 1},
        {"TgtIPAP.Max", "cmH2O", 0, 50, 0, 2500, 1},
        {"TgtEPAP.50", "cmH2O", 0, 30, 0, 1500, 1},
        {"TgtEPAP.95", "cmH2O", 0, 30, 0, 1500, 1},
        {"TgtEPAP.Max", "cmH2O", 0, 30, 0, 1500, 1},
        {"Leak.50", "L/s", 0, 2, 0, 100, 1},
        {"Leak.95", "L/s", 0, 2, 0, 100, 1},
        {"Leak.70", "L/s", 0, 2, 0, 100, 1},
        {"Leak.Max", "L/s", 0, 2, 0, 100, 1},
        {"MinVent.50", "L/min", 0, 30, 0, 240, 1},
        {"MinVent.95", "L/min", 0, 30, 0, 240, 1},
        {"MinVent.Max", "L/min", 0, 30, 0, 240, 1},
        {"RespRate.50", "bpm", 0, 90, 0, 450, 1},
        {"RespRate.95", "bpm", 0, 90, 0, 450, 1},
        {"RespRate.Max", "bpm", 0, 90, 0, 450, 1},
        {"TidVol.50", "L", 0, 4, 0, 200, 1},
        {"TidVol.95", "L", 0, 4, 0, 200, 1},
        {"TidVol.Max", "L", 0, 4, 0, 200, 1},
        {"AHI", "", 0, 240, 0, 2400, 1},
        {"HI", "", 0, 240, 0, 2400, 1},
        {"AI", "", 0, 240, 0, 2400, 1},
        {"OAI", "", 0, 240, 0, 2400, 1},
        {"CAI", "", 0, 240, 0, 2400, 1},
        {"UAI", "", 0, 240, 0, 2400, 1},
        {"RIN", "", 0, 240, 0, 2400, 1},
        {"CSR", "", 0, 1440, 0, 1440, 1},
        {"Crc16", "", -32768, 32767, -32768, 32767, 1},
    };
    return l;
}

long long daysSinceEpoch(int y, int m, int d) {
    std::tm t{};
    t.tm_year = y - 1900;
    t.tm_mon  = m - 1;
    t.tm_mday = d;
    t.tm_hour = 12;
    return static_cast<long long>(timegm(&t) / 86400);
}

/// An STR of `days` records from 2026-10-01, every day a used card day with
/// Mode 1, S.A.MaxPress 15, EPR level 2, SpO2.50 95 and two sessions.
std::vector<uint8_t> cardStr(int days) {
    EDFWriteSpec spec;
    spec.recording       = "Startdate 01-OCT-2026 X X X SRN=12345678901 MID=46 VID=3";
    spec.start           = WallClock{2026, 10, 1, 12, 0, 0};
    spec.record_duration = 86400.0;
    const long long first = daysSinceEpoch(2026, 10, 1);
    for (const auto& s : as11Layout()) {
        EDFWriteSignal w;
        w.label = s.label; w.phys_dim = s.dim;
        w.phys_min = s.pmin; w.phys_max = s.pmax;
        w.dig_min = s.dmin; w.dig_max = s.dmax;
        w.samples_per_record = s.spr;
        for (int d = 0; d < days; d++) {
            const std::string l = s.label;
            for (int k = 0; k < s.spr; k++) {
                double v = s.pmin;
                if (l == "Date") v = static_cast<double>(first + d);
                else if (l == "MaskOn") v = k == 0 ? 600 : k == 1 ? 800 : 0;
                else if (l == "MaskOff") v = k == 0 ? 700 : k == 1 ? 1000 : 0;
                else if (l == "Duration") v = 300;
                else if (l == "MaskEvents") v = 4;
                else if (l == "Mode") v = 1;
                else if (l == "S.A.MaxPress") v = 15;
                else if (l == "S.EPR.Level") v = 2;
                else if (l == "SpO2.50") v = 95;
                else if (l == "AHI") v = 5;
                else if (l == "Leak.50") v = 0.1;
                else if (l == "Crc16") v = 0;
                w.samples.push_back(v);
            }
        }
        spec.signals.push_back(std::move(w));
    }
    return writeEDF(spec);
}

size_t headerBytes(const std::vector<uint8_t>& edf) {
    return static_cast<size_t>(std::stoi(std::string(edf.begin() + 184, edf.begin() + 192)));
}

size_t recordBytes() {
    size_t n = 0;
    for (const auto& s : as11Layout()) n += static_cast<size_t>(s.spr) * 2;
    return n;
}

std::vector<uint8_t> recordOf(const std::vector<uint8_t>& edf, size_t k) {
    const size_t at = headerBytes(edf) + k * recordBytes();
    return {edf.begin() + static_cast<long>(at), edf.begin() + static_cast<long>(at + recordBytes())};
}

void putRecord(std::vector<uint8_t>& edf, size_t k, const std::vector<uint8_t>& rec) {
    std::copy(rec.begin(), rec.end(), edf.begin() + static_cast<long>(headerBytes(edf) + k * recordBytes()));
}

uint16_t crc16(const uint8_t* p, size_t n) {
    uint16_t c = 0xFFFF;
    for (size_t i = 0; i < n; i++) {
        c = static_cast<uint16_t>(c ^ (static_cast<uint16_t>(p[i]) << 8));
        for (int b = 0; b < 8; b++)
            c = static_cast<uint16_t>((c & 0x8000) ? ((c << 1) ^ 0x1021) : (c << 1));
    }
    return c;
}

int16_t sampleAt(const std::vector<uint8_t>& rec, const std::string& label, int k = 0) {
    size_t at = 0;
    for (const auto& s : as11Layout()) {
        if (label == s.label) {
            at += static_cast<size_t>(k) * 2;
            return static_cast<int16_t>(rec[at] | (rec[at + 1] << 8));
        }
        at += static_cast<size_t>(s.spr) * 2;
    }
    return 0;
}

/// A summary day as the machine reports one: noon to noon, its figures.
AirSense11Day summaryDay(std::chrono::system_clock::time_point noon) {
    AirSense11Day d;
    d.day_start     = noon;
    d.day_end       = noon + std::chrono::hours(24);
    d.usage_minutes = 458;
    d.mask_events   = 6;
    d.sessions      = {{noon + std::chrono::minutes(735), 104},
                       {noon + std::chrono::minutes(870), 234},
                       {noon + std::chrono::minutes(1144), 120}};
    d.ahi = 0.9; d.ai = 0.7; d.hi = 0.2; d.oai = 0.4; d.cai = 0.3; d.uai = 0.0; d.rin = 0.0;
    d.csr_minutes = 2.0;
    d.leak_lps.p50 = 0.04; d.leak_lps.p70 = 0.08; d.leak_lps.p95 = 0.24; d.leak_lps.max = 0.5;
    d.mask_pressure.p50 = 8.0; d.mask_pressure.p95 = 8.8; d.mask_pressure.max = 9.6;
    d.target_ipap.p50 = 8.0; d.target_ipap.p95 = 8.8; d.target_ipap.max = 9.6;
    d.target_epap.p50 = 6.0; d.target_epap.p95 = 6.8; d.target_epap.max = 7.6;
    d.tidal_volume_l.p50 = 0.48; d.tidal_volume_l.p95 = 0.8; d.tidal_volume_l.max = 1.12;
    d.minute_vent.p50 = 7.25; d.minute_vent.p95 = 11.125; d.minute_vent.max = 14.5;
    d.resp_rate.p50 = 15.2; d.resp_rate.p95 = 19.0; d.resp_rate.max = 24.0;
    d.blower_pressure.p5 = 7.0; d.blower_pressure.p95 = 9.5;
    d.flow_lps.p5 = -0.5; d.flow_lps.p95 = 0.6;
    d.blower_flow_lps = 0.3;
    d.ambient_humidity = 9.5; d.humidifier_temp = 30.0; d.heated_tube_temp = 27.0;
    d.humidifier_power = 40.0; d.heated_tube_power = 35.0;
    return d;
}

std::chrono::system_clock::time_point localNoon(int y, int m, int d) {
    std::tm t{};
    t.tm_year = y - 1900; t.tm_mon = m - 1; t.tm_mday = d; t.tm_hour = 12; t.tm_isdst = -1;
    return std::chrono::system_clock::from_time_t(std::mktime(&t));
}

}  // namespace

TEST(STRRecordWriter, ASummaryDayReadsBackAsTheSummarysFigures) {
    // One day-off base, the summary written into it, read back by parseSTR.
    auto str = cardStr(1);
    const auto day = summaryDay(localNoon(2026, 10, 1));
    const auto rec = writeResMedSTRRecord(str.data(), str.size(), {}, daysSinceEpoch(2026, 10, 1),
                                          AirSense11SummaryParser::toStrDay(day));
    ASSERT_EQ(rec.size(), recordBytes());
    putRecord(str, 0, rec);

    const auto days = EDFParser::parseSTRFromBuffer(str.data(), str.size(), "dev");
    ASSERT_EQ(days.size(), 1u);
    const auto& r = days[0];
    EXPECT_DOUBLE_EQ(r.duration_minutes, 458);
    EXPECT_EQ(r.mask_events, 6);
    EXPECT_NEAR(r.ahi, 0.9, 0.05);
    EXPECT_NEAR(r.ai, 0.7, 0.05);
    EXPECT_NEAR(r.hi, 0.2, 0.05);
    EXPECT_NEAR(r.oai, 0.4, 0.05);
    EXPECT_NEAR(r.cai, 0.3, 0.05);
    EXPECT_NEAR(r.csr, 2.0, 0.5);
    EXPECT_NEAR(r.leak_50, 0.04 * 60, 0.02 * 60 / 2 + 1e-9);   // one digital step is 0.02 L/s
    EXPECT_NEAR(r.leak_70, 0.08 * 60, 0.6);
    EXPECT_NEAR(r.leak_95, 0.24 * 60, 0.6);
    EXPECT_NEAR(r.leak_max, 0.5 * 60, 0.6);
    EXPECT_NEAR(r.mask_press_50, 8.0, 0.01);
    EXPECT_NEAR(r.mask_press_95, 8.8, 0.01);
    EXPECT_NEAR(r.mask_press_max, 9.6, 0.01);
    ASSERT_TRUE(r.tgt_ipap_50 && r.tgt_epap_95);
    EXPECT_NEAR(*r.tgt_ipap_50, 8.0, 0.01);
    EXPECT_NEAR(*r.tgt_epap_95, 6.8, 0.01);
    EXPECT_NEAR(r.tid_vol_50, 0.48, 0.01);
    EXPECT_NEAR(r.min_vent_50, 7.25, 0.07);
    EXPECT_NEAR(r.resp_rate_50, 15.2, 0.1);
    EXPECT_NEAR(r.blow_press_5, 7.0, 0.01);
    EXPECT_NEAR(r.blow_press_95, 9.5, 0.01);

    // The sessions, as minutes from the record's noon.
    ASSERT_EQ(r.mask_pairs.size(), 3u);
    const int want_on[]  = {735, 870, 1144};
    const int want_len[] = {104, 234, 120};
    for (size_t i = 0; i < 3; i++) {
        const auto on = std::chrono::duration_cast<std::chrono::minutes>(r.mask_pairs[i].first - r.record_date);
        const auto len = std::chrono::duration_cast<std::chrono::minutes>(r.mask_pairs[i].second - r.mask_pairs[i].first);
        EXPECT_EQ(on.count(), want_on[i]);
        EXPECT_EQ(len.count(), want_len[i]);
    }

    // What the summary never carries is the day-off record's "no data".
    EXPECT_EQ(sampleAt(rec, "Mode"), -1);
    EXPECT_EQ(sampleAt(rec, "S.A.MaxPress"), -1);
    EXPECT_EQ(sampleAt(rec, "SpO2.50"), -1);
    EXPECT_EQ(sampleAt(rec, "MaskOn", 3), -1);
    EXPECT_EQ(sampleAt(rec, "MaskOn", 19), -1);
    EXPECT_EQ(r.spo2_50, 0);   // the reader's "no oximeter"
}

TEST(STRRecordWriter, OnTheMachinesOwnRecordWhatTheSummaryLacksIsKept) {
    auto str = cardStr(1);
    const auto base = recordOf(str, 0);
    auto day = summaryDay(localNoon(2026, 10, 1));
    day.target_ipap = {};          // a figure this summary did not send
    const auto rec = writeResMedSTRRecord(str.data(), str.size(), base, daysSinceEpoch(2026, 10, 1),
                                          AirSense11SummaryParser::toStrDay(day));
    ASSERT_EQ(rec.size(), recordBytes());
    putRecord(str, 0, rec);
    const auto days = EDFParser::parseSTRFromBuffer(str.data(), str.size(), "dev");
    ASSERT_EQ(days.size(), 1u);
    EXPECT_DOUBLE_EQ(days[0].duration_minutes, 458);                // the summary's
    EXPECT_EQ(days[0].mode, 1);                                     // the card's
    EXPECT_NEAR(days[0].epr_level, 2.0, 0.02);
    EXPECT_NEAR(days[0].spo2_50, 95, 0.5);
    EXPECT_EQ(sampleAt(rec, "S.A.MaxPress"), sampleAt(base, "S.A.MaxPress"));
    EXPECT_EQ(sampleAt(rec, "TgtIPAP.50"), sampleAt(base, "TgtIPAP.50"));
    // The summary's three sessions replace the card's two, the rest are -1.
    EXPECT_EQ(days[0].mask_pairs.size(), 3u);
    EXPECT_EQ(sampleAt(rec, "MaskOff", 3), -1);
}

TEST(STRRecordWriter, TheRecordCarriesItsDateAndAValidCrc) {
    const auto str = cardStr(1);
    const long long day = daysSinceEpoch(2026, 10, 24);
    const auto rec = writeResMedSTRRecord(str.data(), str.size(), {}, day,
                                          AirSense11SummaryParser::toStrDay(summaryDay(localNoon(2026, 10, 24))));
    ASSERT_EQ(rec.size(), recordBytes());
    EXPECT_EQ(sampleAt(rec, "Date"), static_cast<int16_t>(day));
    const size_t crc_at = recordBytes() - 2;
    EXPECT_EQ(static_cast<uint16_t>(rec[crc_at] | (rec[crc_at + 1] << 8)), crc16(rec.data(), crc_at));

    // Nothing to write is the machine's day off: every sample -1, Date, Crc16.
    const auto off = writeResMedSTRRecord(str.data(), str.size(), {}, day, STRDayValues{});
    ASSERT_EQ(off.size(), recordBytes());
    EXPECT_EQ(sampleAt(off, "Duration"), -1);
    EXPECT_EQ(sampleAt(off, "AHI"), -1);
    EXPECT_EQ(sampleAt(off, "Date"), static_cast<int16_t>(day));
    EXPECT_EQ(static_cast<uint16_t>(off[crc_at] | (off[crc_at + 1] << 8)), crc16(off.data(), crc_at));
}

TEST(STRRecordWriter, AValueBeyondItsRangeIsClampedNeverWrapped) {
    const auto str = cardStr(1);
    STRDayValues v;
    v.values = {{"Leak.Max", 5.0}, {"AHI", -3.0}, {"NotASignal", 1.0}, {"Date", 9.0}, {"Crc16", 7.0}};
    const auto rec = writeResMedSTRRecord(str.data(), str.size(), {}, 20000, v);
    ASSERT_EQ(rec.size(), recordBytes());
    EXPECT_EQ(sampleAt(rec, "Leak.Max"), 100);   // 2 L/s, the top of the range
    EXPECT_EQ(sampleAt(rec, "AHI"), 0);
    EXPECT_EQ(sampleAt(rec, "Date"), 20000);     // never taken from the values
    STRDayValues nan;
    nan.values = {{"AHI", std::nan("")}};
    EXPECT_EQ(sampleAt(writeResMedSTRRecord(str.data(), str.size(), {}, 20000, nan), "AHI"), -1);
}

TEST(STRRecordWriter, NotAnStrOrAWrongBaseWritesNothing) {
    const auto str = cardStr(1);
    const std::vector<uint8_t> short_base(10, 0);
    EXPECT_TRUE(writeResMedSTRRecord(str.data(), str.size(), short_base, 1, {}).empty());
    EXPECT_TRUE(writeResMedSTRRecord(nullptr, 0, {}, 1, {}).empty());

    // A BRP (1 s records) is not an STR.
    StreamedBRP brp;
    brp.start = WallClock{2026, 10, 1, 22, 0, 0};
    brp.flow_lpm.assign(50, 1.0);
    brp.press_cmh2o.assign(50, 8.0);
    const auto b = writeResMedBRP(brp);
    EXPECT_TRUE(writeResMedSTRRecord(b.data(), b.size(), {}, 1, {}).empty());
}

TEST(STRRecordWriter, TheReaderTakesTheSlotCountFromTheHeader) {
    // An AirSense 11 has 20 MaskOn/MaskOff slots a day. The second day's
    // sessions must come from the second day's slots.
    auto str = cardStr(2);
    auto day = summaryDay(localNoon(2026, 10, 2));
    putRecord(str, 1, writeResMedSTRRecord(str.data(), str.size(), recordOf(str, 1),
                                           daysSinceEpoch(2026, 10, 2),
                                           AirSense11SummaryParser::toStrDay(day)));
    const auto days = EDFParser::parseSTRFromBuffer(str.data(), str.size(), "dev");
    ASSERT_EQ(days.size(), 2u);
    ASSERT_EQ(days[0].mask_pairs.size(), 2u);   // the card's 600-700, 800-1000
    EXPECT_EQ(std::chrono::duration_cast<std::chrono::minutes>(days[0].mask_pairs[0].first - days[0].record_date).count(), 600);
    ASSERT_EQ(days[1].mask_pairs.size(), 3u);
    EXPECT_EQ(std::chrono::duration_cast<std::chrono::minutes>(days[1].mask_pairs[0].first - days[1].record_date).count(), 735);
}

TEST(STRRecordWriter, TheSummaryNamesOnlyWhatItCarries) {
    AirSense11Day off;
    off.day_start = localNoon(2026, 10, 3);
    EXPECT_TRUE(AirSense11SummaryParser::toStrDay(off).empty());

    AirSense11Day bare;
    bare.day_start     = localNoon(2026, 10, 3);
    bare.usage_minutes = 60;
    const auto v = AirSense11SummaryParser::toStrDay(bare);
    ASSERT_EQ(v.values.size(), 1u);              // Duration, nothing invented
    EXPECT_EQ(v.values[0].first, "Duration");
    EXPECT_TRUE(v.sessions.empty());

    const auto full = AirSense11SummaryParser::toStrDay(summaryDay(localNoon(2026, 10, 3)));
    for (const auto& [label, x] : full.values) {
        EXPECT_NE(label, "Mode");
        EXPECT_EQ(label.rfind("S.", 0), std::string::npos) << label;
        EXPECT_EQ(label.rfind("SpO2", 0), std::string::npos) << label;
    }
}
