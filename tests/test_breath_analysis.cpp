#include <gtest/gtest.h>
#include "cpapdash/parser/EDFParser.h"

#include <cmath>
#include <vector>

using namespace cpapdash::parser;

namespace {

constexpr double RATE = 25.0;  // ResMed BRP flow, Hz

// One breath as a pair of half-sines: inspiration positive for ti seconds,
// expiration negative for te seconds. amp is peak flow in L/min.
void appendSineBreath(std::vector<double>& out, double ti, double te,
                      double amp = 30.0, double rate = RATE) {
    const int n_i = static_cast<int>(ti * rate);
    const int n_e = static_cast<int>(te * rate);
    for (int i = 0; i < n_i; ++i) {
        out.push_back(amp * std::sin(M_PI * (i + 0.5) / n_i));
    }
    for (int i = 0; i < n_e; ++i) {
        out.push_back(-amp * std::sin(M_PI * (i + 0.5) / n_e));
    }
}

std::vector<double> sineTrain(int n_breaths, double ti = 1.6, double te = 2.4,
                              double amp = 30.0, double rate = RATE) {
    std::vector<double> flow;
    for (int b = 0; b < n_breaths; ++b) appendSineBreath(flow, ti, te, amp, rate);
    return flow;
}

double mean(const std::vector<double>& v) {
    double s = 0.0;
    for (double x : v) s += x;
    return s / v.size();
}

}  // namespace

// ---------------------------------------------------------------------------
// D1: the zero-crossing parity fix
// ---------------------------------------------------------------------------

// The bug this whole change exists for. A file that opens mid-expiration used
// to pair its crossings half a breath out, and the tidal-volume sanity filter
// then discarded most of the mispaired cycles, so the file yielded almost
// nothing instead of anything visibly wrong.
TEST(BreathAnalysis, OpeningInExpirationYieldsTheSameBreathsAsOpeningInInspiration) {
    const auto opens_inspiring = sineTrain(20);

    // Same signal, started one full inspiration in, so it opens in expiration.
    const int n_i = static_cast<int>(1.6 * RATE);
    std::vector<double> opens_expiring(opens_inspiring.begin() + n_i,
                                       opens_inspiring.end());

    const auto a = EDFParser::detectBreaths(opens_inspiring, RATE);
    const auto b = EDFParser::detectBreaths(opens_expiring, RATE);

    ASSERT_GE(a.size(), 15u);
    // Within one breath: the shifted signal drops the partial cycle it opens on.
    EXPECT_NEAR(static_cast<double>(b.size()), static_cast<double>(a.size()), 1.0);

    EXPECT_NEAR(a[1].inspiratory_time, b[0].inspiratory_time, 0.1);
    EXPECT_NEAR(a[1].expiratory_time, b[0].expiratory_time, 0.1);
}

// A sine starting exactly at 0.0 used to yield zero breaths, because
// clean_flow[0] > 0 is false and the file was declared to be in expiration.
// The Philips fork's fixture generator carries a +0.1 rad phase offset purely
// to dodge this.
TEST(BreathAnalysis, ASineStartingAtExactlyZeroStillYieldsBreaths) {
    std::vector<double> flow;
    const double period = 4.0;
    for (int i = 0; i < static_cast<int>(period * RATE * 12); ++i) {
        flow.push_back(30.0 * std::sin(2 * M_PI * i / (period * RATE)));
    }

    const auto breaths = EDFParser::detectBreaths(flow, RATE);
    EXPECT_GE(breaths.size(), 9u);
}

// Mispairing swapped these two, and the swap was invisible in aggregate because
// Ti + Te is unchanged by it.
TEST(BreathAnalysis, InspiratoryAndExpiratoryTimesAreNotSwapped) {
    const auto flow = sineTrain(12, /*ti=*/1.0, /*te=*/2.0);
    const auto breaths = EDFParser::detectBreaths(flow, RATE);

    ASSERT_GE(breaths.size(), 8u);
    for (const auto& b : breaths) {
        EXPECT_NEAR(b.inspiratory_time, 1.0, 0.12);
        EXPECT_NEAR(b.expiratory_time, 2.0, 0.12);
        EXPECT_LT(b.inspiratory_time, b.expiratory_time);
    }
}

TEST(BreathAnalysis, AFlatLineYieldsNoBreathsAndDoesNotCrash) {
    const std::vector<double> flat(RATE * 60, 0.0);
    EXPECT_TRUE(EDFParser::detectBreaths(flat, RATE).empty());
}

TEST(BreathAnalysis, AnEmptySeriesYieldsNoBreaths) {
    EXPECT_TRUE(EDFParser::detectBreaths({}, RATE).empty());
}

// ---------------------------------------------------------------------------
// Flow limitation
//
// A shape-based flattening index was built here and then dropped: measured
// against ResMed's own FlowLim channel over 175 sessions and 49,256 paired
// minutes of real card data it came out at Spearman -0.003, with the machine's
// highest flow-limitation minutes scoring no higher than its lowest. So the
// only thing left to assert is that the existing scalar still behaves.
// ---------------------------------------------------------------------------

// flow_limitation keeps its old meaning and its old range.
TEST(BreathAnalysis, TheOriginalFlowLimitationScalarIsUnchangedInRange) {
    const auto breaths = EDFParser::detectBreaths(sineTrain(12), RATE);
    ASSERT_GE(breaths.size(), 8u);
    for (const auto& b : breaths) {
        EXPECT_GE(b.flow_limitation, 0.0);
        EXPECT_LE(b.flow_limitation, 1.0);
    }
}

// ---------------------------------------------------------------------------
// D2: one detection pass, bucketed by minute
// ---------------------------------------------------------------------------

// The whole-file pass must not lose or duplicate the breath that sits across a
// minute boundary. Per-minute detection truncated it and the tidal-volume
// filter then dropped it from both minutes.
TEST(BreathAnalysis, ABreathStraddlingAMinuteBoundaryIsCountedExactlyOnce) {
    // 4 s per breath at 25 Hz is 100 samples, and 1500 samples per minute, so
    // breath 15 starts at sample 1500 exactly. Shift by half a breath to put a
    // breath across the boundary instead of on it.
    std::vector<double> flow(static_cast<int>(2.0 * RATE), 0.0);
    for (int i = 0; i < 40; ++i) appendSineBreath(flow, 1.6, 2.4);

    const auto breaths = EDFParser::detectBreaths(flow, RATE);
    ASSERT_GE(breaths.size(), 30u);

    const int samples_per_minute = static_cast<int>(RATE * 60);
    int straddlers = 0;
    for (const auto& b : breaths) {
        if (b.start_idx / samples_per_minute != b.end_idx / samples_per_minute) {
            ++straddlers;
        }
    }
    ASSERT_GT(straddlers, 0) << "fixture does not actually straddle a boundary";

    // Onsets are strictly increasing and unique, so bucketing by onset assigns
    // every breath to exactly one minute.
    for (size_t i = 1; i < breaths.size(); ++i) {
        EXPECT_GT(breaths[i].start_idx, breaths[i - 1].start_idx);
    }
}

// Detection over the whole file and detection over a clean slice agree when
// there is no straddling breath to disagree about.
TEST(BreathAnalysis, WholeFileDetectionMatchesASliceWithNoStraddlingBreath) {
    const auto flow = sineTrain(15);  // 15 x 4 s = 60 s exactly
    const auto whole = EDFParser::detectBreaths(flow, RATE);

    const std::vector<double> first_half(flow.begin(),
                                         flow.begin() + flow.size() / 3);
    const auto sliced = EDFParser::detectBreaths(first_half, RATE);

    ASSERT_FALSE(sliced.empty());
    for (size_t i = 0; i < sliced.size(); ++i) {
        EXPECT_NEAR(whole[i].inspiratory_time, sliced[i].inspiratory_time, 0.1);
        EXPECT_NEAR(whole[i].tidal_volume, sliced[i].tidal_volume, 1.0);
    }
}
