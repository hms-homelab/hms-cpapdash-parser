#include <gtest/gtest.h>
#include "cpapdash/parser/EDFParser.h"
#include <cstring>
#include <string>
#include <vector>

using namespace cpapdash::parser;

namespace {

// Build an STR.edf carrying exactly the signals named, one daily record.
//
// The point of this helper is the SIGNAL SET: SDD-064 establishes what kind of
// machine wrote a file from which settings signals it contains, so a test needs to
// be able to say "a file with these labels and no others".
std::vector<uint8_t> buildSTR(const std::vector<std::string>& labels,
                              const std::vector<double>& values) {
    const int ns = (int)labels.size();
    const int header_bytes = 256 + 256 * ns;
    const int total = header_bytes + ns * 2;   // 1 record, 1 sample per signal
    std::vector<uint8_t> buf(total, ' ');

    auto put = [&](int off, int len, const std::string& v) {
        for (int i = 0; i < len; i++)
            buf[off + i] = (i < (int)v.size()) ? v[i] : ' ';
    };

    put(0, 8, "0");
    put(8, 80, "TestPatient");
    put(88, 80, "Startdate 01-MAR-2026 SRN=12345 MID=46 VID=6");
    put(168, 8, "01.03.26");
    put(176, 8, "12.00.00");
    put(184, 8, std::to_string(header_bytes));
    put(192, 44, "");
    put(236, 8, "1");                       // one daily record
    put(244, 8, "86400");                   // STR is one record per day
    put(252, 4, std::to_string(ns));

    const int base = 256;
    for (int i = 0; i < ns; i++) put(base + i * 16, 16, labels[i]);
    int off = base + ns * 16;
    for (int i = 0; i < ns; i++) put(off + i * 80, 80, "");
    off += ns * 80;
    for (int i = 0; i < ns; i++) put(off + i * 8, 8, "cmH2O");
    off += ns * 8;
    for (int i = 0; i < ns; i++) put(off + i * 8, 8, "0");        // phys min
    off += ns * 8;
    for (int i = 0; i < ns; i++) put(off + i * 8, 8, "3276");     // phys max
    off += ns * 8;
    for (int i = 0; i < ns; i++) put(off + i * 8, 8, "0");        // dig min
    off += ns * 8;
    for (int i = 0; i < ns; i++) put(off + i * 8, 8, "32760");    // dig max
    off += ns * 8;
    for (int i = 0; i < ns; i++) put(off + i * 80, 80, "");
    off += ns * 80;
    for (int i = 0; i < ns; i++) put(off + i * 8, 8, "1");        // samples/record
    off += ns * 8;
    for (int i = 0; i < ns; i++) put(off + i * 32, 32, "");

    // phys 0..3276 over dig 0..32760 => digital = value * 10.
    // The range has to hold Duration in MINUTES (480 for an eight-hour night); an
    // earlier version used a x100 scale and 480 overflowed int16 into a negative,
    // so every record looked like it had no therapy and the parser returned none.
    for (int i = 0; i < ns; i++) {
        int16_t d = (int16_t)((i < (int)values.size() ? values[i] : 0.0) * 10.0);
        buf[header_bytes + i * 2]     = (uint8_t)(d & 0xFF);
        buf[header_bytes + i * 2 + 1] = (uint8_t)((d >> 8) & 0xFF);
    }
    return buf;
}

// Every STR needs these for a record to be considered therapy at all.
const std::vector<std::string> kBase = {"Duration", "MaskEvents", "Mode"};

} // namespace

// A bi-level is identified by S.VA.* / S.S.*, NOT by its mode number and NOT by
// its model code. Ken Narod's AirCurve and five AutoSet machines all report
// MID=46, so the model cannot separate them.
TEST(STRFamily, BiLevelIsRecognisedFromItsSignals) {
    auto data = buildSTR(
        {"Duration", "MaskEvents", "Mode", "S.VA.MaxIPAP", "S.VA.MinEPAP",
         "S.VA.PS", "S.S.IPAP", "S.S.EPAP"},
        {480, 1, 8, 14, 5, 3, 8, 5});

    auto recs = EDFParser::parseSTRFromBuffer(data.data(), data.size(), "dev");
    ASSERT_FALSE(recs.empty());
    const auto& r = recs[0];

    EXPECT_EQ(r.family, STRDailyRecord::Family::BiLevel);
    // The mode is left EXACTLY as the machine reported it. Rewriting it would
    // silently change what every already-stored row means.
    EXPECT_EQ(r.mode, 8);

    ASSERT_TRUE(r.bl_max_ipap.has_value()); EXPECT_NEAR(*r.bl_max_ipap, 14.0, 0.01);
    ASSERT_TRUE(r.bl_min_epap.has_value()); EXPECT_NEAR(*r.bl_min_epap, 5.0, 0.01);
    ASSERT_TRUE(r.bl_ps.has_value());       EXPECT_NEAR(*r.bl_ps, 3.0, 0.01);
    ASSERT_TRUE(r.bl_ipap.has_value());     EXPECT_NEAR(*r.bl_ipap, 8.0, 0.01);
    ASSERT_TRUE(r.bl_epap.has_value());     EXPECT_NEAR(*r.bl_epap, 5.0, 0.01);
}

// THE REGRESSION THAT MATTERS: 3679 of 4421 fleet nights are CPAP/APAP. A change
// to how therapy pressure is modelled goes wrong by disturbing the 24 users who
// are fine in order to fix the one who is not.
TEST(STRFamily, AnAutoSetIsUnaffectedAndCarriesNoBiLevelFields) {
    auto data = buildSTR(
        {"Duration", "MaskEvents", "Mode", "S.AS.MaxPress", "S.AS.MinPress", "S.C.Press"},
        {480, 1, 1, 14, 6, 10});

    auto recs = EDFParser::parseSTRFromBuffer(data.data(), data.size(), "dev");
    ASSERT_FALSE(recs.empty());
    const auto& r = recs[0];

    EXPECT_EQ(r.family, STRDailyRecord::Family::AutoSet);
    EXPECT_EQ(r.mode, 1);
    EXPECT_FALSE(r.bl_max_ipap.has_value());
    EXPECT_FALSE(r.bl_min_epap.has_value());
    EXPECT_FALSE(r.bl_ps.has_value());
    EXPECT_FALSE(r.bl_ipap.has_value());
    EXPECT_FALSE(r.bl_epap.has_value());
}

// ASV must keep working. It is the family whose enum we had been applying to
// everyone, so it is the one most at risk of being broken by the fix.
TEST(STRFamily, AsvStillClassifiesAsAsv) {
    auto data = buildSTR(
        {"Duration", "MaskEvents", "Mode", "S.AV.EPAP", "S.AA.MinEPAP", "S.AA.MaxEPAP"},
        {480, 1, 8, 6, 5, 12});

    auto recs = EDFParser::parseSTRFromBuffer(data.data(), data.size(), "dev");
    ASSERT_FALSE(recs.empty());
    EXPECT_EQ(recs[0].family, STRDailyRecord::Family::Asv);
    EXPECT_FALSE(recs[0].bl_ps.has_value());
}

// Mode 8 means DIFFERENT THINGS on different machines. This is the whole defect:
// a bi-level reporting 8 was read through the ASV enum and shown to its owner as
// "ASVAuto", a treatment for central apnea he is not on.
TEST(STRFamily, TheSameModeNumberMeansDifferentThingsPerFamily) {
    auto bilevel = buildSTR(
        {"Duration", "MaskEvents", "Mode", "S.VA.PS"}, {480, 1, 8, 3});
    auto asv = buildSTR(
        {"Duration", "MaskEvents", "Mode", "S.AV.EPAP"}, {480, 1, 8, 6});

    auto rb = EDFParser::parseSTRFromBuffer(bilevel.data(), bilevel.size(), "dev");
    auto ra = EDFParser::parseSTRFromBuffer(asv.data(), asv.size(), "dev");
    ASSERT_FALSE(rb.empty()); ASSERT_FALSE(ra.empty());

    EXPECT_EQ(rb[0].mode, ra[0].mode);              // identical number
    EXPECT_NE(rb[0].family, ra[0].family);          // different therapy
    EXPECT_EQ(rb[0].family, STRDailyRecord::Family::BiLevel);
    EXPECT_EQ(ra[0].family, STRDailyRecord::Family::Asv);
}

// A fixed-pressure CPAP has only S.C.*, and a file we cannot place must say so
// rather than be guessed into a family.
TEST(STRFamily, PlainCpapAndUnknownAreDistinguished) {
    auto cpap = buildSTR({"Duration", "MaskEvents", "Mode", "S.C.Press"}, {480, 1, 0, 10});
    auto bare = buildSTR({"Duration", "MaskEvents", "Mode"}, {480, 1, 0});

    auto rc = EDFParser::parseSTRFromBuffer(cpap.data(), cpap.size(), "dev");
    auto ru = EDFParser::parseSTRFromBuffer(bare.data(), bare.size(), "dev");
    ASSERT_FALSE(rc.empty()); ASSERT_FALSE(ru.empty());

    EXPECT_EQ(rc[0].family, STRDailyRecord::Family::Cpap);
    EXPECT_EQ(ru[0].family, STRDailyRecord::Family::Unknown);
}
