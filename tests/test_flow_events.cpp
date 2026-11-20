// Apneas and hypopneas from the flow (SDD-006, SDD-014), on synthetic flow with
// the events written into it. Real nights are the validation tool's, never a
// test's (SDD-006 section 6).
#include <gtest/gtest.h>

#include "cpapdash/parser/EDFParser.h"
#include "cpapdash/parser/FlowEvents.h"

#include <chrono>
#include <cmath>
#include <vector>

using namespace cpapdash::parser;

namespace {

constexpr double kHz = 25.0;
const auto kStart = std::chrono::system_clock::time_point(std::chrono::seconds(2000000000));

// Breathing at 15 a minute (4 s breaths), `amplitude` L/min peak, for `seconds`.
void breathe(std::vector<double>& flow, int seconds, double amplitude) {
    const double pi = std::acos(-1.0);
    const size_t n = static_cast<size_t>(seconds * kHz);
    for (size_t i = 0; i < n; ++i) flow.push_back(amplitude * std::sin(2 * pi * i / (4 * kHz)));
}

// No breathing: only a 1 L/min cardiac ripple at 48 a minute, as real flow
// keeps during an apnea. A perfectly flat line is not used: the breath detector
// then drops the breath before it, and the detector misses the pause (SDD-014
// section 5 records it).
void still(std::vector<double>& flow, int seconds) {
    const double pi = std::acos(-1.0);
    const size_t n = static_cast<size_t>(seconds * kHz);
    for (size_t i = 0; i < n; ++i) flow.push_back(1.0 * std::sin(2 * pi * i / (1.25 * kHz)));
}

// 180 s of breathing, then `event` (filled by the caller), then 120 s more.
std::vector<double> night(void (*event)(std::vector<double>&)) {
    std::vector<double> f;
    breathe(f, 180, 30);
    event(f);
    breathe(f, 120, 30);
    return f;
}

double secondsFromStart(const SleepEvent& e) {
    return std::chrono::duration<double>(e.timestamp - kStart).count();
}

DesatEvent desatAt(double s, double depth) {
    DesatEvent d;
    d.onset = kStart + std::chrono::milliseconds(static_cast<long long>(s * 1000));
    d.duration_seconds = 15;
    d.depth = depth;
    d.nadir = 95 - depth;
    return d;
}

}  // namespace

TEST(SleepEventSource, AnEventThatDoesNotSayIsTheMachines) {
    // SDD-014: the field is last and defaulted, so every existing construction
    // keeps its meaning.
    const SleepEvent made(EventType::OBSTRUCTIVE, kStart, 12.0);
    EXPECT_EQ(made.source, EventSource::Machine);
    SleepEvent blank;
    EXPECT_EQ(blank.source, EventSource::Machine);
}

TEST(FlowApneas, TwentySecondsOfNoFlowIsOneComputedApnea) {
    const auto flow = night([](std::vector<double>& f) { still(f, 20); });
    const auto breaths = EDFParser::detectBreaths(flow, kHz);
    const auto found = detectFlowApneas(breaths, flow, kHz, kStart);
    ASSERT_EQ(found.size(), 1u);
    EXPECT_EQ(found[0].event_type, EventType::APNEA);
    EXPECT_EQ(found[0].source, EventSource::Computed);
    EXPECT_NEAR(secondsFromStart(found[0]), 180.0, 4.0);
    EXPECT_GE(found[0].duration_seconds, 16.0);
    EXPECT_LE(found[0].duration_seconds, 24.0);
}

TEST(FlowApneas, SteadyBreathingHasNone) {
    std::vector<double> flow;
    breathe(flow, 600, 30);
    EXPECT_TRUE(detectFlowApneas(EDFParser::detectBreaths(flow, kHz), flow, kHz, kStart).empty());
}

TEST(FlowApneas, AHalvedBreathIsNotAnApnea) {
    const auto flow = night([](std::vector<double>& f) { breathe(f, 20, 15); });
    EXPECT_TRUE(detectFlowApneas(EDFParser::detectBreaths(flow, kHz), flow, kHz, kStart).empty());
}

TEST(FlowHypopneas, AFlowDropWithNoDesaturationIsNothing) {
    // Ringless: never a flow-only hypopnea (SDD-014 D5).
    const auto flow = night([](std::vector<double>& f) { breathe(f, 20, 15); });
    const auto breaths = EDFParser::detectBreaths(flow, kHz);
    EXPECT_TRUE(detectFlowHypopneas(breaths, flow, kHz, kStart, {}).empty());
}

TEST(FlowHypopneas, AFlowDropWithADesaturationAfterItIsAHypopnea) {
    const auto flow = night([](std::vector<double>& f) { breathe(f, 20, 15); });
    const auto breaths = EDFParser::detectBreaths(flow, kHz);
    const auto found = detectFlowHypopneas(breaths, flow, kHz, kStart, {desatAt(215, 4)});
    ASSERT_EQ(found.size(), 1u);
    EXPECT_EQ(found[0].event_type, EventType::HYPOPNEA);
    EXPECT_EQ(found[0].source, EventSource::Computed);
    EXPECT_NEAR(secondsFromStart(found[0]), 180.0, 4.0);
    EXPECT_GE(found[0].duration_seconds, 10.0);
}

TEST(FlowHypopneas, TheDesaturationMustBeDeepAndSoonEnough) {
    const auto flow = night([](std::vector<double>& f) { breathe(f, 20, 15); });
    const auto breaths = EDFParser::detectBreaths(flow, kHz);
    EXPECT_TRUE(detectFlowHypopneas(breaths, flow, kHz, kStart, {desatAt(215, 2)}).empty())
        << "2 points is under the 3% rule";
    EXPECT_TRUE(detectFlowHypopneas(breaths, flow, kHz, kStart, {desatAt(250, 4)}).empty())
        << "beginning 50 s after the drop ends is too late";
    EXPECT_TRUE(detectFlowHypopneas(breaths, flow, kHz, kStart, {desatAt(150, 4)}).empty())
        << "a desaturation before the drop is not its";
}

TEST(FlowHypopneas, AnApneaIsNotAlsoAHypopnea) {
    const auto flow = night([](std::vector<double>& f) { still(f, 20); });
    const auto breaths = EDFParser::detectBreaths(flow, kHz);
    EXPECT_EQ(detectFlowApneas(breaths, flow, kHz, kStart).size(), 1u);
    EXPECT_TRUE(detectFlowHypopneas(breaths, flow, kHz, kStart, {desatAt(210, 5)}).empty());
}
