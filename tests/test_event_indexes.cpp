#include <gtest/gtest.h>

#include <cpapdash/parser/EventIndexes.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace cpapdash::parser;

namespace {

// Same resolution dance as test_sleep_index.cpp: CTest runs from the BUILD
// directory while a developer running ./run_tests by hand is usually at the
// repository root, and a path relative to either one alone is a test that
// passes locally and fails in CI.
#ifndef CPAPDASH_PARSER_SOURCE_DIR
#define CPAPDASH_PARSER_SOURCE_DIR "."
#endif

std::string fixturePath(const std::string& name) {
    const std::string rel = "tests/fixtures/event_indexes/" + name;
    for (const std::string& base : {std::string(CPAPDASH_PARSER_SOURCE_DIR) + "/",
                                    std::string(""),
                                    std::string("../"),
                                    std::string("../../")}) {
        std::error_code ec;
        if (std::filesystem::exists(base + rel, ec)) return base + rel;
    }
    return {};
}

std::vector<std::string> readTable(const std::string& name) {
    const std::string path = fixturePath(name);
    std::ifstream in(path);
    EXPECT_TRUE(!path.empty() && in.good())
        << "missing fixture " << name << " (looked relative to "
        << CPAPDASH_PARSER_SOURCE_DIR << " and to the working directory)";
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        lines.push_back(line);
    }
    return lines;
}

std::vector<double> split(const std::string& line) {
    std::vector<double> out;
    std::stringstream ss(line);
    std::string cell;
    while (std::getline(ss, cell, ',')) out.push_back(std::stod(cell));
    return out;
}

// Indexes are events per hour at full precision, so the contract is compared
// with a tolerance rather than as formatted strings. Rounding is a DISPLAY
// concern; a fixture that compared "0.49" would be asserting the wrong thing.
constexpr double kEps = 1e-6;

}  // namespace

// *** THE CROSS-LANGUAGE CONTRACT. ***
// Every implementation of this rule, in any language and in SQL, must reproduce
// this table. Change the mapping or the arithmetic in EventIndexes.h and this
// fails first, then every consumer's tests fail until they follow.
TEST(EventIndexes, ReproducesTheFixtureContract) {
    const auto rows = readTable("index_vectors.csv");
    ASSERT_FALSE(rows.empty()) << "fixture is empty; the contract asserts nothing";

    int checked = 0;
    for (const auto& row : rows) {
        const auto f = split(row);
        ASSERT_EQ(f.size(), 15u) << "malformed fixture row: " << row;

        EventCounts c;
        c.obstructive  = static_cast<int>(f[0]);
        c.central      = static_cast<int>(f[1]);
        c.clear_airway = static_cast<int>(f[2]);
        c.unclassified = static_cast<int>(f[3]);
        c.hypopnea     = static_cast<int>(f[4]);
        c.arousal      = static_cast<int>(f[5]);
        // Must change nothing. This is the column that catches a numerator
        // written as count(*) over every event row.
        c.excluded_from_indexes = static_cast<int>(f[6]);
        const double hours = f[7];

        const auto r = computeIndexes(c, hours);
        EXPECT_NEAR(r.ahi, f[8],  kEps) << "AHI, row: " << row;
        EXPECT_NEAR(r.ai,  f[9],  kEps) << "AI, row: "  << row;
        EXPECT_NEAR(r.hi,  f[10], kEps) << "HI, row: "  << row;
        EXPECT_NEAR(r.oai, f[11], kEps) << "OAI, row: " << row;
        EXPECT_NEAR(r.cai, f[12], kEps) << "CAI, row: " << row;
        EXPECT_NEAR(r.uai, f[13], kEps) << "UAI, row: " << row;
        EXPECT_NEAR(r.rin, f[14], kEps) << "RIN, row: " << row;
        ++checked;
    }
    EXPECT_GE(checked, 10) << "the contract is thinner than it was; rows were removed";
}

// The mapping is the half that drifts, so each type is pinned independently.
// A numerator that swallows or drops one type cannot pass these by coincidence.

TEST(EventIndexes, ClearAirwayReachesAhiAndAiAndNothingElse) {
    EventCounts c;
    c.clear_airway = 4;
    const auto r = computeIndexes(c, 8.0);
    EXPECT_NEAR(r.ahi, 0.5, kEps);
    EXPECT_NEAR(r.ai, 0.5, kEps);
    EXPECT_NEAR(r.hi, 0.0, kEps);
    EXPECT_NEAR(r.oai, 0.0, kEps);
    EXPECT_NEAR(r.cai, 0.0, kEps) << "clear-airway must not be folded into CAI";
    EXPECT_NEAR(r.uai, 0.0, kEps);
}

TEST(EventIndexes, UnclassifiedApneaCountsTowardAhiAndAi) {
    // ResMed's bare 'Apnea'. Counted toward AHI for years and never persisted,
    // which is the defect this whole contract exists to close.
    EventCounts c;
    c.unclassified = 3;
    const auto r = computeIndexes(c, 6.0);
    EXPECT_NEAR(r.ahi, 0.5, kEps);
    EXPECT_NEAR(r.ai, 0.5, kEps);
    EXPECT_NEAR(r.uai, 0.5, kEps);
    EXPECT_NEAR(r.oai, 0.0, kEps) << "unclassified must not be folded into OAI";
}

TEST(EventIndexes, ArousalsMoveRinAloneAndNeverReachAhi) {
    EventCounts c;
    c.arousal = 9;
    const auto r = computeIndexes(c, 9.0);
    EXPECT_NEAR(r.rin, 1.0, kEps);
    EXPECT_NEAR(r.ahi, 0.0, kEps) << "RERA is not part of AHI";
    EXPECT_NEAR(r.ai, 0.0, kEps);
}

TEST(EventIndexes, HypopneasMoveAhiAndHiButNotAi) {
    EventCounts c;
    c.hypopnea = 12;
    const auto r = computeIndexes(c, 8.0);
    EXPECT_NEAR(r.ahi, 1.5, kEps);
    EXPECT_NEAR(r.hi, 1.5, kEps);
    EXPECT_NEAR(r.ai, 0.0, kEps) << "AI is apneas only";
}

TEST(EventIndexes, EventsExcludedFromIndexesReachNothing) {
    EventCounts c;
    c.excluded_from_indexes = 500;  // CSR, flow limitation, snore, large leak, OTHER
    const auto r = computeIndexes(c, 8.0);
    EXPECT_NEAR(r.ahi, 0.0, kEps);
    EXPECT_NEAR(r.ai, 0.0, kEps);
    EXPECT_NEAR(r.rin, 0.0, kEps);
}

TEST(EventIndexes, ZeroHoursReturnsZerosRatherThanDividing) {
    EventCounts c;
    c.obstructive = 4;
    c.hypopnea = 6;
    const auto r = computeIndexes(c, 0.0);
    EXPECT_NEAR(r.ahi, 0.0, kEps);
    EXPECT_FALSE(std::isnan(r.ahi));
    EXPECT_FALSE(std::isinf(r.ahi));

    const auto neg = computeIndexes(c, -1.0);
    EXPECT_NEAR(neg.ahi, 0.0, kEps);
}

TEST(EventIndexes, NumeratorsAreNamedSoSqlTranscribesRatherThanRederives) {
    EventCounts c;
    c.obstructive = 7;
    c.central = 5;
    c.clear_airway = 3;
    c.unclassified = 2;
    c.hypopnea = 11;
    c.arousal = 13;
    EXPECT_EQ(aiNumerator(c), 17);
    EXPECT_EQ(ahiNumerator(c), 28);
    EXPECT_EQ(ahiNumerator(c), aiNumerator(c) + c.hypopnea);
}

// The incomplete-night detector. STR floors, so the machine's own index bounds
// the true numerator from below; a count under that bound means events are
// missing from our copy and the computed index would read LOW.
TEST(EventIndexes, DetectsTheNightWhoseRawDataIsIncomplete) {
    // 20250721 on the reference card: STR says AHI 2.2 over 9.733h, which
    // implies at least 21.41 events. Our copy holds 20 -- the card is missing
    // 66 minutes of therapy and, with it, one obstructive and one hypopnea.
    EXPECT_FALSE(eventsLookComplete(20, 2.2, 9.733));
    EXPECT_TRUE(eventsLookComplete(22, 2.2, 9.733));

    // A night whose index already agrees with STR can never be flagged. That is
    // arithmetic, not luck: floor(n/hours) == str_index implies n >= the bound.
    EXPECT_TRUE(eventsLookComplete(11, 1.2, 5.467));   // 20250722, agrees
    EXPECT_TRUE(eventsLookComplete(14, 4.5, 3.067));   // 20250729, agrees
}

TEST(EventIndexes, CompletenessCheckIsInertWithoutSomethingToCheckAgainst) {
    // No STR index, or no hours, means no lower bound exists. Do not invent one
    // and do not fall back on a night we cannot judge.
    EXPECT_TRUE(eventsLookComplete(0, 0.0, 8.0));
    EXPECT_TRUE(eventsLookComplete(0, 2.2, 0.0));
}
