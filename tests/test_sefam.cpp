#ifdef CPAPDASH_WITH_SEFAM

// End to end over the fixtures in tests/fixtures/sefam.
//
// They are synthetic, but they are written to the format a real 242-session
// donor card actually uses, and they exist to exercise what one card cannot: a
// corrupt checksum, channels that disagree on the recording length, an identity
// that does not match the INI. The card is the accuracy check; these are the
// refusal checks.

#include <gtest/gtest.h>

#include "cpapdash/parser/ISessionParser.h"
#include "cpapdash/parser/SefamParser.h"

#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

using namespace cpapdash::parser;
namespace fs = std::filesystem;

namespace {

#ifndef CPAPDASH_PARSER_SOURCE_DIR
#define CPAPDASH_PARSER_SOURCE_DIR "."
#endif

std::string fixturePath(const std::string& name) {
    const std::string rel = "tests/fixtures/sefam/" + name;
    for (const std::string& base : {std::string(CPAPDASH_PARSER_SOURCE_DIR) + "/",
                                    std::string(""),
                                    std::string("../"),
                                    std::string("../../")}) {
        std::error_code ec;
        if (fs::exists(base + rel, ec)) return base + rel;
    }
    return {};
}

const char* kCardSession = "card/1263R/24337476/DATA_0";
const char* kCardRoot = "card";

double avgOf(const std::vector<BreathingSummary>& rows,
             std::optional<double> BreathingSummary::*field) {
    double sum = 0;
    int n = 0;
    for (const auto& r : rows) if ((r.*field).has_value()) { sum += *(r.*field); ++n; }
    return n ? sum / n : 0;
}

double avgOf(const std::vector<BreathingSummary>& rows,
             double BreathingSummary::*field) {
    double sum = 0;
    for (const auto& r : rows) sum += r.*field;
    return rows.empty() ? 0 : sum / static_cast<double>(rows.size());
}

} // namespace

// ── Detection ───────────────────────────────────────────────────────────────

TEST(SefamDetect, SessionManifestNamesTheBrand) {
    EXPECT_EQ(detectManufacturer(std::vector<std::string>{"DATA_0.INI"}),
              DeviceManufacturer::SEFAM);
    EXPECT_EQ(detectManufacturer(std::vector<std::string>{
                  "1263R/24337476/DATA_0/DATA_0.INI"}),
              DeviceManufacturer::SEFAM);
    EXPECT_EQ(detectManufacturer(std::vector<std::string>{"data_12/data_12.ini"}),
              DeviceManufacturer::SEFAM);
}

TEST(SefamDetect, AnOrdinaryIniIsNotASefamCard) {
    EXPECT_NE(detectManufacturer(std::vector<std::string>{"settings.ini"}),
              DeviceManufacturer::SEFAM);
    EXPECT_NE(detectManufacturer(std::vector<std::string>{"Report/Report.ini"}),
              DeviceManufacturer::SEFAM);
    EXPECT_NE(detectManufacturer(std::vector<std::string>{"DATA_X.INI"}),
              DeviceManufacturer::SEFAM);
    EXPECT_NE(detectManufacturer(std::vector<std::string>{"Report/DATA_0.INI"}),
              DeviceManufacturer::SEFAM);
}

TEST(SefamDetect, FoundFromTheCardRootAndFromTheSessionFolder) {
    const auto root = fixturePath(kCardRoot);
    const auto session = fixturePath(kCardSession);
    ASSERT_FALSE(root.empty());
    ASSERT_FALSE(session.empty());

    // The session sits two levels down, under <model>/<serial>/.
    EXPECT_EQ(detectManufacturer(root), DeviceManufacturer::SEFAM);
    EXPECT_EQ(detectManufacturer(session), DeviceManufacturer::SEFAM);
}

TEST(SefamDetect, FactoryBuildsTheParser) {
    auto parser = createParser(DeviceManufacturer::SEFAM);
    ASSERT_NE(parser, nullptr);
    EXPECT_EQ(parser->manufacturer(), DeviceManufacturer::SEFAM);
}

// ── A session ───────────────────────────────────────────────────────────────

TEST(SefamParse, ReadsTheIdentityFromBothTheIniAndTheData) {
    const auto dir = fixturePath(kCardSession);
    ASSERT_FALSE(dir.empty()) << "run tests/fixtures/sefam/gen/make_fixtures.py";

    SefamParser parser;
    auto s = parser.parseSession(dir, "dev-1", "");
    ASSERT_NE(s, nullptr);

    EXPECT_EQ(s->manufacturer, DeviceManufacturer::SEFAM);
    EXPECT_EQ(s->serial_number, "1263R24337476");
    // The caller passed no name, so the model string out of the INI stands in.
    EXPECT_EQ(s->device_name, "S.Box_AUTO");

    const auto& h = parser.lastNotes().header;
    ASSERT_TRUE(h.valid);
    EXPECT_EQ(h.record_type, "#02");
    EXPECT_EQ(h.identity, "1263R24337476");
}

TEST(SefamParse, EveryBlockChecksumVerifies) {
    const auto dir = fixturePath(kCardSession);
    ASSERT_FALSE(dir.empty());

    SefamParser parser;
    auto s = parser.parseSession(dir, "dev-1", "");
    ASSERT_NE(s, nullptr);

    const auto& framing = parser.lastNotes().framing;
    ASSERT_FALSE(framing.empty());
    for (const auto& [name, f] : framing) {
        EXPECT_GT(f.blocks, 0u) << name;
        EXPECT_EQ(f.checksum_failures, 0u) << name;
        EXPECT_EQ(f.index_breaks, 0u) << name;
    }
}

TEST(SefamParse, DurationComesFromTheSamplesNotTheIni) {
    const auto dir = fixturePath(kCardSession);
    ASSERT_FALSE(dir.empty());

    SefamParser parser;
    auto s = parser.parseSession(dir, "dev-1", "");
    ASSERT_NE(s, nullptr);

    // The INI declares an eight-hour recording, as every session on the donor
    // card does. The channels hold two minutes, and two minutes is the therapy.
    ASSERT_TRUE(s->duration_seconds.has_value());
    EXPECT_EQ(*s->duration_seconds, 120);
    ASSERT_TRUE(s->session_end.has_value());
    EXPECT_EQ(*s->session_end, *s->session_start + std::chrono::seconds(120));
}

TEST(SefamParse, StartTimeComesFromTheIni) {
    const auto dir = fixturePath(kCardSession);
    ASSERT_FALSE(dir.empty());

    SefamParser parser;
    auto s = parser.parseSession(dir, "dev-1", "");
    ASSERT_NE(s, nullptr);
    ASSERT_TRUE(s->session_start.has_value());

    const std::time_t t = std::chrono::system_clock::to_time_t(*s->session_start);
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
}

TEST(SefamParse, PressureAndLeakLandInTheirUnits) {
    const auto dir = fixturePath(kCardSession);
    ASSERT_FALSE(dir.empty());

    SefamParser parser;
    auto s = parser.parseSession(dir, "dev-1", "");
    ASSERT_NE(s, nullptr);

    ASSERT_EQ(s->breathing_summary.size(), 2u);  // 120 s, one row a minute
    EXPECT_TRUE(s->has_summary);

    // Codes near 110 and 122, read as tenths.
    EXPECT_NEAR(avgOf(s->breathing_summary, &BreathingSummary::avg_pressure), 11.0, 0.1);
    EXPECT_NEAR(avgOf(s->breathing_summary, &BreathingSummary::therapy_pressure), 11.0, 0.1);
    EXPECT_NEAR(avgOf(s->breathing_summary, &BreathingSummary::leak_rate), 12.2, 0.1);

    // Flow is centred on nothing, as breathing is.
    EXPECT_NEAR(avgOf(s->breathing_summary, &BreathingSummary::avg_flow_rate), 0.0, 1.0);
}

TEST(SefamParse, LeakIsAlsoKeptAtTheRateTheMachineWroteIt) {
    const auto dir = fixturePath(kCardSession);
    ASSERT_FALSE(dir.empty());

    SefamParser parser;
    auto s = parser.parseSession(dir, "dev-1", "");
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->native_samples.leak.size(), 120u);
}

// DET is a bitfield of concurrent flags, and bit 2 is the apnea flag -- the only
// bit whose long runs coincide with the airflow actually stopping, established
// on the donor card (SefamParser_Events.cpp).
TEST(SefamParse, ApneasComeFromDetBitTwo) {
    const auto dir = fixturePath(kCardSession);
    ASSERT_FALSE(dir.empty());

    SefamParser parser;
    auto s = parser.parseSession(dir, "dev-1", "");
    ASSERT_NE(s, nullptr);

    // Two runs long enough to count, and a 2 s burst that is not an event: bit 2
    // fires in sub-second bursts as well, and the ten-second floor is what
    // separates a detection from an apnea.
    ASSERT_EQ(s->events.size(), 2u);
    EXPECT_TRUE(s->has_events);
    EXPECT_NEAR(s->events[0].duration_seconds, 14.0, 0.1);
    EXPECT_NEAR(s->events[1].duration_seconds, 11.0, 0.1);

    // APNEA, never OBSTRUCTIVE or CENTRAL: that airflow stopped is measured,
    // the mechanism is not.
    for (const auto& e : s->events) {
        EXPECT_EQ(e.event_type, EventType::APNEA);
        ASSERT_TRUE(e.details.has_value());
        EXPECT_EQ(*e.details, "Sefam apnea (DET bit 2)");
    }
}

// The index that comes out is an APNEA index, not an AHI: no hypopneas are
// detected, because the only candidate bit does not meet the criterion. A
// consumer must not grade this against AHI severity thresholds.
TEST(SefamParse, TheIndexIsApneaOnly) {
    const auto dir = fixturePath(kCardSession);
    ASSERT_FALSE(dir.empty());

    SefamParser parser;
    auto s = parser.parseSession(dir, "dev-1", "");
    ASSERT_NE(s, nullptr);
    ASSERT_TRUE(s->metrics.has_value());

    EXPECT_EQ(s->metrics->total_events, 2);
    EXPECT_EQ(s->metrics->unclassified_apneas, 2);
    EXPECT_EQ(s->metrics->hypopneas, 0);
    EXPECT_EQ(s->metrics->obstructive_apneas, 0);
    EXPECT_EQ(s->metrics->central_apneas, 0);

    // Two apneas in two minutes of recording.
    EXPECT_NEAR(s->metrics->ahi, 60.0, 1.0);
}

TEST(SefamParse, EveryDetBitIsStillReported) {
    const auto dir = fixturePath(kCardSession);
    ASSERT_FALSE(dir.empty());

    SefamParser parser;
    auto s = parser.parseSession(dir, "dev-1", "");
    ASSERT_NE(s, nullptr);

    // The seven bits we have not identified are reported rather than dropped, so
    // the next round of identification has somewhere to start.
    const auto& share = parser.lastNotes().det_bit_share;
    ASSERT_TRUE(share.count(1));
    ASSERT_TRUE(share.count(2));
    ASSERT_TRUE(share.count(3));
    EXPECT_NEAR(share.at(1), 2.0 / 3.0, 0.02);
    EXPECT_LT(share.at(3), 0.01);
    EXPECT_GT(share.at(3), 0.0);
}

TEST(SefamParse, StubChannelsAreNamedAndProduceNoVitals) {
    const auto dir = fixturePath(kCardSession);
    ASSERT_FALSE(dir.empty());

    SefamParser parser;
    auto s = parser.parseSession(dir, "dev-1", "");
    ASSERT_NE(s, nullptr);

    EXPECT_TRUE(s->vitals.empty());
    const auto& stubs = parser.lastNotes().stub_channels;
    EXPECT_EQ(stubs.size(), 3u);  // SPO, HRT, POS
}

// ── Oximetry, unmapped and undeclared channels ──────────────────────────────

TEST(SefamParse, OximetryBecomesVitals) {
    const auto dir = fixturePath("oximetry/DATA_1");
    ASSERT_FALSE(dir.empty());

    SefamParser parser;
    auto s = parser.parseSession(dir, "dev-2", "Bedroom");
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->device_name, "Bedroom");

    ASSERT_EQ(s->vitals.size(), 120u);  // 1 Hz over two minutes
    ASSERT_TRUE(s->vitals[0].spo2.has_value());
    ASSERT_TRUE(s->vitals[0].heart_rate.has_value());
    EXPECT_NEAR(*s->vitals[0].spo2, 96.0, 2.5);
    EXPECT_NEAR(*s->vitals[0].heart_rate, 60.0, 6.0);
}

TEST(SefamParse, UnmappedAndUndeclaredChannelsAreReported) {
    const auto dir = fixturePath("oximetry/DATA_1");
    ASSERT_FALSE(dir.empty());

    SefamParser parser;
    auto s = parser.parseSession(dir, "dev-2", "Bedroom");
    ASSERT_NE(s, nullptr);

    const auto& notes = parser.lastNotes();
    ASSERT_EQ(notes.unmapped_channels.size(), 1u);
    EXPECT_EQ(notes.unmapped_channels[0], "THO");
    ASSERT_EQ(notes.undeclared_files.size(), 1u);
    EXPECT_EQ(notes.undeclared_files[0], "Y17");
}

// Two sessions on the donor card number their blocks from 2, with every checksum
// correct. This fixture does the same.
TEST(SefamParse, ASessionNumberingBlocksFromTwoIsStillRead) {
    const auto dir = fixturePath("oximetry/DATA_1");
    ASSERT_FALSE(dir.empty());

    SefamParser parser;
    auto s = parser.parseSession(dir, "dev-2", "");
    ASSERT_NE(s, nullptr);
    for (const auto& [name, f] : parser.lastNotes().framing)
        EXPECT_EQ(f.index_breaks, 0u) << name;
}

// ── Refusals ────────────────────────────────────────────────────────────────

TEST(SefamParse, ACardWithNothingRecordedIsRefused) {
    const auto dir = fixturePath("stubs/DATA_2");
    ASSERT_FALSE(dir.empty());

    SefamParser parser;
    EXPECT_EQ(parser.parseSession(dir, "dev-3", ""), nullptr);
}

// The block checksums are the whole reason to trust a reading of this format. A
// failure means the framing was misread, and everything after it in that channel
// is suspect.
TEST(SefamParse, ABadBlockChecksumIsRefused) {
    const auto dir = fixturePath("badsum/DATA_3");
    ASSERT_FALSE(dir.empty());

    SefamParser parser;
    EXPECT_EQ(parser.parseSession(dir, "dev-4", ""), nullptr);
}

TEST(SefamParse, ChannelsThatDisagreeOnTheRecordingLengthAreRefused) {
    const auto dir = fixturePath("spans/DATA_4");
    ASSERT_FALSE(dir.empty());

    SefamParser parser;
    EXPECT_EQ(parser.parseSession(dir, "dev-5", ""), nullptr);
}

// The file header names the device, and so does the INI. When they disagree the
// INI and the data do not belong together, and neither can be trusted about the
// other.
TEST(SefamParse, DataAndIniNamingDifferentDevicesIsRefused) {
    const auto dir = fixturePath("mismatch/DATA_5");
    ASSERT_FALSE(dir.empty());

    SefamParser parser;
    EXPECT_EQ(parser.parseSession(dir, "dev-6", ""), nullptr);
}

TEST(SefamParse, AFolderWithNoManifestIsNotASession) {
    SefamParser parser;
    EXPECT_EQ(parser.parseSession(fixturePath("card"), "dev-7", ""), nullptr);
    EXPECT_EQ(parser.parseSession("/no/such/place", "dev-7", ""), nullptr);
}

#endif // CPAPDASH_WITH_SEFAM
