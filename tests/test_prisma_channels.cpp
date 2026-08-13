// Löwenstein channel binding: exact spellings only, from an explicit table.
//
// Two Prisma firmwares in the field disagree on nearly half their channel
// names. Binding used to fall back to a SUBSTRING search, which is silently
// dangerous on labels this short: a SMART max has no exact "MV", so the
// minute-ventilation read matched **rRMV** — a 0-255 relative percentage — and
// it would have been stored as minute ventilation in L/min. A wrong therapy
// number is worse than a missing one.
//
// These build WMEDF files carrying each firmware's real channel layout rather
// than shipping patient data. Values are chosen so a mis-binding cannot pass:
// each channel holds a distinct constant.
#include <gtest/gtest.h>

#include "cpapdash/parser/ISessionParser.h"
#include "cpapdash/parser/Models.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace cpapdash::parser;
namespace fs = std::filesystem;

namespace {

struct Chan {
    std::string label;
    double value;     // constant physical value written to every sample
    int samples_per_record = 1;
};

void pad(std::string& out, const std::string& v, size_t width) {
    std::string s = v.substr(0, width);
    s.resize(width, ' ');
    out += s;
}

// A minimal WMEDF: 8-bit samples, one second per record, constant per channel.
// Digital range is chosen so `digital == physical`, keeping the assertions
// readable.
std::vector<uint8_t> makeWmedf(const std::vector<Chan>& chans, int records) {
    const int ns = static_cast<int>(chans.size());
    const int header_bytes = 256 + ns * (16 + 80 + 8 + 8 + 8 + 8 + 8 + 80 + 8 + 32);

    std::string h;
    pad(h, "1", 8);                                   // version
    pad(h, "Patient Name", 80);
    pad(h, "Recording start at 20.07.2026 21:40:10", 80);
    pad(h, "20.07.26", 8);
    pad(h, "21.40.10", 8);
    pad(h, std::to_string(header_bytes), 8);
    pad(h, "", 44);                                   // reserved
    pad(h, std::to_string(records), 8);
    pad(h, "1", 8);                                   // record duration (s)
    pad(h, std::to_string(ns), 4);

    for (const auto& c : chans) pad(h, c.label, 16);
    for (int i = 0; i < ns; ++i)  pad(h, "transducer", 80);
    for (int i = 0; i < ns; ++i)  pad(h, "hPa", 8);
    for (int i = 0; i < ns; ++i)  pad(h, "0", 8);      // phys min
    for (int i = 0; i < ns; ++i)  pad(h, "255", 8);    // phys max
    for (int i = 0; i < ns; ++i)  pad(h, "0", 8);      // dig min
    for (int i = 0; i < ns; ++i)  pad(h, "255", 8);    // dig max
    for (int i = 0; i < ns; ++i)  pad(h, "", 80);      // prefiltering
    for (const auto& c : chans) pad(h, std::to_string(c.samples_per_record), 8);
    for (int i = 0; i < ns; ++i)  pad(h, "#1", 32);    // "#1" => 8-bit samples

    std::vector<uint8_t> out(h.begin(), h.end());
    for (int r = 0; r < records; ++r)
        for (const auto& c : chans)
            for (int s = 0; s < c.samples_per_record; ++s)
                out.push_back(static_cast<uint8_t>(c.value));
    return out;
}

// The two layouts observed in production, verbatim.
std::vector<Chan> prismaLayout() {
    return {
        {"Pressure", 8, 5}, {"EEPAPsoll", 9}, {"IPAPsoll", 10}, {"EPAPsoll", 7},
        {"RespFlow", 20, 10}, {"rAMV", 30}, {"BreathVolume", 0},
        {"BreathFrequency", 0}, {"LeakFlowBreath", 12}, {"ObstructLevel", 40},
        {"SpO2", 0}, {"HeartFrequency", 0}, {"SPRstatus", 3},
        {"InspExpirRel", 0}, {"MV", 0}, {"rMVFluctuation", 5},
        {"TotalLeakage", 60}, {"RSBI", 70},
    };
}

std::vector<Chan> smartMaxLayout() {
    return {
        {"RespFlow", 20, 5}, {"LeakFlowBreath", 12}, {"ObstructLevel", 40},
        {"Pressure", 8, 2}, {"CPAPPressure", 6}, {"PressureMeasured", 11, 2},
        {"FlowFull", 25, 5}, {"rRMV", 99}, {"SPRStatus", 3},
        {"IPAP", 10}, {"EPAP", 7}, {"rMVFluctuation", 5},
    };
}

// Write one sequence into a temp dir and parse it the way the apps do.
std::unique_ptr<ParsedSession> parseLayout(const std::vector<Chan>& chans,
                                           int records = 180) {
    auto dir = fs::temp_directory_path() /
               ("prisma_chan_" + std::to_string(::rand()));
    fs::create_directories(dir);
    auto buf = makeWmedf(chans, records);
    {
        std::ofstream f(dir / "signal_000001.wmedf", std::ios::binary);
        f.write(reinterpret_cast<const char*>(buf.data()),
                static_cast<std::streamsize>(buf.size()));
    }
    auto parser = createParser(DeviceManufacturer::LOWENSTEIN);
    auto s = parser ? parser->parseSession(dir.string(), "dev", "Prisma") : nullptr;
    std::error_code ec;
    fs::remove_all(dir, ec);
    return s;
}

int countSet(const ParsedSession& s, std::optional<double> BreathingSummary::*f) {
    int n = 0;
    for (const auto& b : s.breathing_summary) n += (b.*f).has_value();
    return n;
}

} // namespace

// ── The bug that made this table necessary ──────────────────────────────────

// A SMART max has no "MV" channel. Under the old substring fallback the read
// bound rRMV, whose 0-255 relative percentage would have been served as minute
// ventilation in L/min. Nothing may bind it now.
TEST(PrismaChannels, MinuteVentilationDoesNotBindToRrmvOnSmartMax) {
    auto s = parseLayout(smartMaxLayout());
    ASSERT_NE(s, nullptr);
    ASSERT_FALSE(s->breathing_summary.empty());

    EXPECT_EQ(countSet(*s, &BreathingSummary::minute_ventilation), 0)
        << "rRMV is a relative percentage and must never be read as MV";
}

// The same trap in the other direction: rMVFluctuation also contains "MV" and
// is present on BOTH firmwares.
TEST(PrismaChannels, MinuteVentilationDoesNotBindToFluctuation) {
    auto s = parseLayout(prismaLayout());
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(countSet(*s, &BreathingSummary::minute_ventilation), 0)
        << "the Prisma layout's MV channel is all zeros; nothing else may stand in";
}

// ── What each firmware can actually give ────────────────────────────────────

// SMART max reports the pressure measured at the mask. It went unread while
// avg_mask_pressure sat empty (hms-cpap issue 15).
TEST(PrismaChannels, SmartMaxYieldsMeasuredMaskPressure) {
    auto s = parseLayout(smartMaxLayout());
    ASSERT_NE(s, nullptr);
    ASSERT_FALSE(s->breathing_summary.empty());

    EXPECT_EQ(countSet(*s, &BreathingSummary::mask_pressure),
              static_cast<int>(s->breathing_summary.size()));
    // 11 hPa -> cmH2O, and NOT the 8 hPa therapy pressure beside it.
    ASSERT_TRUE(s->breathing_summary.front().mask_pressure.has_value());
    EXPECT_NEAR(*s->breathing_summary.front().mask_pressure, 11.0 * 1.01972, 0.05);
}

// SMART max drops the "soll" suffix, so EPAP went unread and EPR came out
// empty on exactly the machine the issue was filed about.
TEST(PrismaChannels, SmartMaxYieldsEprFromUnsuffixedEpap) {
    auto s = parseLayout(smartMaxLayout());
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(countSet(*s, &BreathingSummary::epr_pressure),
              static_cast<int>(s->breathing_summary.size()));
    ASSERT_TRUE(s->breathing_summary.front().epr_pressure.has_value());
    EXPECT_NEAR(*s->breathing_summary.front().epr_pressure, 7.0 * 1.01972, 0.05);
}

// The suffixed spelling must keep working; this is the older firmware.
TEST(PrismaChannels, PrismaStillYieldsEprFromSuffixedEpap) {
    auto s = parseLayout(prismaLayout());
    ASSERT_NE(s, nullptr);
    ASSERT_TRUE(s->breathing_summary.front().epr_pressure.has_value());
    EXPECT_NEAR(*s->breathing_summary.front().epr_pressure, 7.0 * 1.01972, 0.05);
}

// The Prisma layout has no measured-pressure channel at all. Absent must stay
// absent rather than borrowing the setpoint.
TEST(PrismaChannels, PrismaHasNoMaskPressureAndInventsNone) {
    auto s = parseLayout(prismaLayout());
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(countSet(*s, &BreathingSummary::mask_pressure), 0);
}

// Both firmwares report leak and obstruction, and both are the values that
// were being computed and then discarded downstream.
TEST(PrismaChannels, BothFirmwaresYieldLeakAndFlowLimitation) {
    for (const auto& layout : {prismaLayout(), smartMaxLayout()}) {
        auto s = parseLayout(layout);
        ASSERT_NE(s, nullptr);
        EXPECT_GT(countSet(*s, &BreathingSummary::leak_rate), 0);
        EXPECT_GT(countSet(*s, &BreathingSummary::flow_limitation), 0);
    }
}

// A channel the machine declares but never writes is not data. Reporting 0%
// SpO2 as a reading would be worse than reporting nothing.
TEST(PrismaChannels, DeclaredButAllZeroChannelsProduceNothing) {
    auto s = parseLayout(prismaLayout());
    ASSERT_NE(s, nullptr);
    EXPECT_TRUE(s->vitals.empty())
        << "SpO2 and HeartFrequency are present in the header but all zero";
    EXPECT_EQ(countSet(*s, &BreathingSummary::respiratory_rate), 0);
    EXPECT_EQ(countSet(*s, &BreathingSummary::tidal_volume), 0);
}

// An unknown spelling must yield nothing rather than latch onto a neighbour.
TEST(PrismaChannels, AnUnknownSpellingBindsNothing) {
    std::vector<Chan> odd = {
        {"Pressure", 8, 2}, {"RespFlow", 20, 5},
        {"LeakageFlowPerBreath", 12},   // not a spelling we accept
        {"PressureAtMask", 11},         // nor this
    };
    auto s = parseLayout(odd);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(countSet(*s, &BreathingSummary::leak_rate), 0);
    EXPECT_EQ(countSet(*s, &BreathingSummary::mask_pressure), 0);
    // ...while the channels it does recognise still come through.
    EXPECT_FALSE(s->breathing_summary.empty());
}
