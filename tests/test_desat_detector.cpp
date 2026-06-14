#include <gtest/gtest.h>
#include "cpapdash/parser/DesatDetector.h"
#include "cpapdash/parser/Models.h"

using namespace cpapdash::parser;

namespace {

// Build a 1 Hz vitals series from a list of SpO2 values starting at t0.
// A negative value marks an invalid/absent SpO2 sample.
std::vector<VitalSample> makeSeries(const std::vector<double>& spo2,
                                    std::chrono::system_clock::time_point t0) {
    std::vector<VitalSample> v;
    v.reserve(spo2.size());
    for (size_t i = 0; i < spo2.size(); ++i) {
        VitalSample s(t0 + std::chrono::seconds(static_cast<int>(i)));
        if (spo2[i] >= 0) s.spo2 = spo2[i];
        v.push_back(s);
    }
    return v;
}

std::vector<double> flat(double value, int n) { return std::vector<double>(n, value); }

void append(std::vector<double>& dst, const std::vector<double>& src) {
    dst.insert(dst.end(), src.begin(), src.end());
}

}  // namespace

TEST(DesatDetector, EmptyAndAllInvalid) {
    auto t0 = std::chrono::system_clock::now();
    EXPECT_TRUE(detectDesaturations({}).empty());
    EXPECT_TRUE(detectDesaturations(makeSeries({-1, -1, -1, -1}, t0)).empty());
}

TEST(DesatDetector, SingleSustainedDrop) {
    auto t0 = std::chrono::system_clock::now();
    // 60s baseline at 97, then a 4% drop to 93 held 12s, then recovery.
    std::vector<double> s;
    append(s, flat(97, 60));
    append(s, flat(93, 12));   // sustained desaturation (>= 8s, drop 4%)
    append(s, flat(97, 30));

    auto events = detectDesaturations(makeSeries(s, t0));
    ASSERT_EQ(events.size(), 1u);
    EXPECT_NEAR(events[0].nadir, 93.0, 1e-6);
    EXPECT_NEAR(events[0].depth, 4.0, 1e-6);            // baseline 97 - nadir 93
    EXPECT_GE(events[0].duration_seconds, 8.0);
}

TEST(DesatDetector, ShortBlipIgnored) {
    auto t0 = std::chrono::system_clock::now();
    // A 4% dip lasting only 3s is below the 8s minimum.
    std::vector<double> s;
    append(s, flat(97, 60));
    append(s, flat(93, 3));
    append(s, flat(97, 30));
    EXPECT_TRUE(detectDesaturations(makeSeries(s, t0)).empty());
}

TEST(DesatDetector, GradualDeclineTracksBaseline) {
    auto t0 = std::chrono::system_clock::now();
    // Slow 1%/30s decline never drops >= 3% below the rolling baseline.
    std::vector<double> s;
    for (int i = 0; i < 300; ++i) s.push_back(97.0 - i / 60.0);  // ~ -5% over 5 min
    EXPECT_TRUE(detectDesaturations(makeSeries(s, t0)).empty());
}

TEST(DesatDetector, BackToBackEvents) {
    auto t0 = std::chrono::system_clock::now();
    std::vector<double> s;
    append(s, flat(97, 60));
    append(s, flat(92, 10));   // event 1
    append(s, flat(97, 20));   // recover
    append(s, flat(91, 10));   // event 2
    append(s, flat(97, 20));

    auto events = detectDesaturations(makeSeries(s, t0));
    ASSERT_EQ(events.size(), 2u);
    EXPECT_NEAR(events[0].nadir, 92.0, 1e-6);
    EXPECT_NEAR(events[1].nadir, 91.0, 1e-6);
}

TEST(DesatDetector, OpenEventClosedAtEnd) {
    auto t0 = std::chrono::system_clock::now();
    // Drop that never recovers before the record ends.
    std::vector<double> s;
    append(s, flat(97, 60));
    append(s, flat(90, 15));   // still desaturated at end-of-record
    auto events = detectDesaturations(makeSeries(s, t0));
    ASSERT_EQ(events.size(), 1u);
    EXPECT_NEAR(events[0].nadir, 90.0, 1e-6);
    EXPECT_GE(events[0].duration_seconds, 8.0);
}

// calculateMetrics() should surface desat count + ODI without polluting AHI.
TEST(DesatDetector, MetricsOdiAndAhiIsolation) {
    ParsedSession session;
    session.duration_seconds = 3600;  // 1 hour

    auto t0 = std::chrono::system_clock::now();
    std::vector<double> s;
    append(s, flat(97, 60));
    append(s, flat(92, 12));
    append(s, flat(97, 60));
    session.vitals = makeSeries(s, t0);

    // One respiratory event; desaturations must NOT be counted into AHI.
    session.events.push_back(SleepEvent(EventType::OBSTRUCTIVE, t0, 15.0));

    session.calculateMetrics();
    ASSERT_TRUE(session.metrics.has_value());
    const auto& m = session.metrics.value();

    EXPECT_EQ(session.desaturations.size(), 1u);
    EXPECT_EQ(m.spo2_drops.value_or(-1), 1);
    ASSERT_TRUE(m.odi.has_value());
    EXPECT_NEAR(m.odi.value(), 1.0, 1e-6);          // 1 desat / 1 hour
    EXPECT_EQ(m.total_events, 1);                   // desat excluded
    EXPECT_NEAR(m.ahi, 1.0, 1e-6);                  // 1 event / 1 hour
}
