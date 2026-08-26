#pragma once

#include <optional>
#include <string>
#include <vector>

namespace cpapdash::parser {

/**
 * SDD-019: the nightly therapy-quality index, and the consistency streak that
 * goes with it.
 *
 * Pure arithmetic. No I/O, no dates beyond an eight character YYYYMMDD string,
 * nothing that needs a database. It lives here rather than in a consumer
 * because hms-cpap, hms-cpapdash-api and cpapdash-app would otherwise each own
 * a copy of the constants below and there would be nothing keeping them equal.
 *
 * The index reflects THERAPY QUALITY. It is not a health grade and not medical
 * advice.
 *
 * The Dart implementation in cpapdash-app is a second copy of this file's
 * arithmetic, held to it by the CSV tables in tests/fixtures/sleep_index.
 * Change a constant
 * here and that fixture has to be regenerated, at which point the app's own
 * tests fail until it follows. That is the whole point.
 */

// -- Tunable weights and cutoffs (SDD-006 approved 40/40/20). If real data
//    argues otherwise these move, and nothing else has to. --
inline constexpr double kUsageWeight = 40.0;
inline constexpr double kAhiWeight   = 40.0;
inline constexpr double kLeakWeight  = 20.0;

inline constexpr double kUsageTargetHours = 7.0;   // full usage credit at or above
inline constexpr double kAhiFloor         = 5.0;   // <= 5 AHI: full credit
inline constexpr double kAhiCeiling       = 30.0;  // >= 30 AHI: zero credit
inline constexpr double kLeakFloor        = 24.0;  // <= 24 L/min leak95: full credit
inline constexpr double kLeakCeiling      = 40.0;  // >= 40 L/min: zero credit

inline constexpr double kComplianceHours = 4.0;    // a "compliant" night, for streaks

/// Streak lengths that earn a callout.
inline constexpr int kStreakMilestones[] = {7, 30, 100, 365};

enum class IndexBand { Excellent, Good, Fair, NeedsAttention };

/// Band for an index in [0, 100]: >= 85 excellent, >= 70 good, >= 50 fair.
IndexBand bandFor(int index);

/// Stable lower-case identifier for a band ("excellent", "needs_attention", ...).
/// This is the wire and fixture form. Display strings are the caller's problem,
/// because they are localised and this library is not.
const char* bandKey(IndexBand band);

/**
 * Nightly index in [0, 100], or nullopt when none of the three inputs is
 * present.
 *
 * Each present input contributes its weight; absent ones drop out and the
 * remaining weights renormalise, so a machine that reports no leak scores out
 * of 80 rather than being punished for its silence. Values that are not finite
 * are treated as absent, as is a negative leak, which is a sentinel and not a
 * measurement.
 */
std::optional<int> nightlyIndex(std::optional<double> usage_hours,
                                std::optional<double> ahi,
                                std::optional<double> leak_95);

/**
 * Mean of the scorable entries among the first [nights], newest first, or
 * nullopt when none of them scores.
 *
 * The window is the first [nights] ENTRIES, not the first [nights] that score:
 * an unscorable night consumes a slot and is then skipped rather than counted
 * as zero. A week with two unscorable nights averages the other five.
 */
std::optional<double> trailingAverage(const std::vector<std::optional<int>>& newest_first,
                                      int nights = 7);

/// One night's usage, for the streak functions. Date is YYYYMMDD.
struct NightUsage {
    std::string date;
    double usage_hours = 0.0;
};

/**
 * Consecutive calendar nights ending at the newest entry, each with usage at or
 * above [threshold_hours].
 *
 * Returns 0 when the newest night itself is not compliant, which is deliberate:
 * a streak the user has already broken is not a streak. Anything that is not
 * "exactly one day older and compliant" ends the run, which includes a gap in
 * the dates, a repeated date, and a date that does not parse.
 *
 * That a repeat ends the run here while bestStreak() skips over it is inherited
 * from the Dart original, not an accident of this port. Both are ports; if the
 * asymmetry ever matters it should be changed on both sides at once, with the
 * fixture regenerated.
 *
 * [newest_first] must be ordered newest first.
 */
int currentStreak(const std::vector<NightUsage>& newest_first,
                  double threshold_hours = kComplianceHours);

/// Longest run of consecutive compliant nights anywhere in the history.
/// Order-independent; unparseable dates are ignored.
int bestStreak(const std::vector<NightUsage>& nights,
               double threshold_hours = kComplianceHours);

/// The milestone just reached, when [streak] is exactly one of them.
std::optional<int> milestoneFor(int streak);

}  // namespace cpapdash::parser
