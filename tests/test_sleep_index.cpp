#include <gtest/gtest.h>

#include "cpapdash/parser/SleepIndex.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

using namespace cpapdash::parser;

namespace {

// Where the contract tables live.
//
// CMake passes the source directory in, because ctest runs the binary from the
// BUILD directory while a developer running ./run_tests by hand is usually at
// the repository root. A path relative to either one alone is a test that
// passes locally and fails in CI, which is exactly what happened.
#ifndef CPAPDASH_PARSER_SOURCE_DIR
#define CPAPDASH_PARSER_SOURCE_DIR "."
#endif

std::string fixturePath(const std::string& name) {
    const std::string rel = "tests/fixtures/sleep_index/" + name;
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

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string field;
    std::istringstream in(s);
    while (std::getline(in, field, sep)) out.push_back(field);
    // getline drops a trailing empty field, and the index table ends in one
    // whenever a night does not score.
    if (!s.empty() && s.back() == sep) out.emplace_back();
    return out;
}

std::optional<double> optDouble(const std::string& field) {
    if (field.empty()) return std::nullopt;
    return std::stod(field);
}

}  // namespace

// ---------------------------------------------------------------------------
// The fixture IS the specification. Both tables below are read by the Dart
// implementation in cpapdash-app as well; if a change makes one of these fail,
// the answer is never to edit the expectation in place.
// ---------------------------------------------------------------------------

TEST(SleepIndex, MatchesTheIndexFixture) {
    const auto lines = readTable("index_vectors.csv");
    ASSERT_FALSE(lines.empty());

    int checked = 0;
    for (const auto& line : lines) {
        const auto f = split(line, ',');
        ASSERT_EQ(f.size(), 5u) << "malformed vector: " << line;

        const auto got = nightlyIndex(optDouble(f[0]), optDouble(f[1]), optDouble(f[2]));

        if (f[3].empty()) {
            EXPECT_FALSE(got.has_value()) << "expected no score: " << line;
            EXPECT_TRUE(f[4].empty()) << "a night that does not score has no band: " << line;
        } else {
            ASSERT_TRUE(got.has_value()) << "expected a score: " << line;
            EXPECT_EQ(*got, std::stoi(f[3])) << line;
            EXPECT_STREQ(bandKey(bandFor(*got)), f[4].c_str()) << line;
        }
        ++checked;
    }
    EXPECT_GE(checked, 20) << "the index fixture lost vectors";
}

TEST(SleepIndex, MatchesTheStreakFixture) {
    const auto lines = readTable("streak_vectors.csv");
    ASSERT_FALSE(lines.empty());

    for (const auto& line : lines) {
        const auto f = split(line, ';');
        ASSERT_EQ(f.size(), 4u) << "malformed vector: " << line;
        const std::string& name = f[0];

        std::vector<NightUsage> nights;
        for (const auto& entry : split(f[1], ',')) {
            if (entry.empty()) continue;
            const auto colon = entry.find(':');
            ASSERT_NE(colon, std::string::npos) << "malformed night: " << entry;
            nights.push_back({entry.substr(0, colon), std::stod(entry.substr(colon + 1))});
        }

        EXPECT_EQ(currentStreak(nights), std::stoi(f[2])) << name;
        EXPECT_EQ(bestStreak(nights), std::stoi(f[3])) << name;
    }
}

// ---------------------------------------------------------------------------
// Behaviour the CSV tables cannot express
// ---------------------------------------------------------------------------

TEST(SleepIndex, NonFiniteInputsAreAbsent) {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();

    // NaN leak drops out and the night scores over usage + AHI alone, exactly
    // as an absent leak does.
    EXPECT_EQ(nightlyIndex(3.5, 17.5, nan), nightlyIndex(3.5, 17.5, std::nullopt));
    EXPECT_EQ(nightlyIndex(inf, 17.5, 32.0), nightlyIndex(std::nullopt, 17.5, 32.0));
    EXPECT_FALSE(nightlyIndex(nan, nan, nan).has_value());
}

TEST(SleepIndex, BandBoundaries) {
    EXPECT_EQ(bandFor(100), IndexBand::Excellent);
    EXPECT_EQ(bandFor(85), IndexBand::Excellent);
    EXPECT_EQ(bandFor(84), IndexBand::Good);
    EXPECT_EQ(bandFor(70), IndexBand::Good);
    EXPECT_EQ(bandFor(69), IndexBand::Fair);
    EXPECT_EQ(bandFor(50), IndexBand::Fair);
    EXPECT_EQ(bandFor(49), IndexBand::NeedsAttention);
    EXPECT_EQ(bandFor(0), IndexBand::NeedsAttention);
}

TEST(SleepIndex, TrailingAverageWindowsEntriesNotScores) {
    // Eight nights, newest first, the fourth of which did not score.
    const std::vector<std::optional<int>> nights = {90, 80, 70, std::nullopt, 60, 50, 40, 0};

    // Seven entries, six of which score: (90+80+70+60+50+40)/6.
    const auto seven = trailingAverage(nights);
    ASSERT_TRUE(seven.has_value());
    EXPECT_DOUBLE_EQ(*seven, 390.0 / 6.0);

    // The unscorable night consumed a slot: three entries, three scores.
    const auto three = trailingAverage(nights, 3);
    ASSERT_TRUE(three.has_value());
    EXPECT_DOUBLE_EQ(*three, 80.0);

    const auto four = trailingAverage(nights, 4);
    ASSERT_TRUE(four.has_value());
    EXPECT_DOUBLE_EQ(*four, 80.0) << "the fourth night does not score and must not count as zero";
}

TEST(SleepIndex, TrailingAverageDegenerateCases) {
    EXPECT_FALSE(trailingAverage({}).has_value());
    EXPECT_FALSE(trailingAverage({std::nullopt, std::nullopt}).has_value());
    EXPECT_FALSE(trailingAverage({90, 80}, 0).has_value());

    const auto shorter = trailingAverage({90, 80});  // fewer nights than the window
    ASSERT_TRUE(shorter.has_value());
    EXPECT_DOUBLE_EQ(*shorter, 85.0);
}

TEST(SleepIndex, Milestones) {
    for (const int m : {7, 30, 100, 365}) {
        EXPECT_EQ(milestoneFor(m), std::optional<int>(m));
    }
    EXPECT_FALSE(milestoneFor(0).has_value());
    EXPECT_FALSE(milestoneFor(6).has_value());
    EXPECT_FALSE(milestoneFor(8).has_value());
    EXPECT_FALSE(milestoneFor(366).has_value());
}

TEST(SleepIndex, ImpossibleDatesDoNotCount) {
    // 31 April is arithmetically 1 May, which is exactly how a naive date
    // conversion turns a bad row into a streak that never happened.
    const std::vector<NightUsage> nights = {{"20260501", 8.0}, {"20260431", 8.0}};
    EXPECT_EQ(currentStreak(nights), 1);
    EXPECT_EQ(bestStreak(nights), 1);
}
