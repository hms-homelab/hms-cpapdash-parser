#include "cpapdash/parser/SleepIndex.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace cpapdash::parser {

namespace {

double clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

bool usable(const std::optional<double>& v) {
    return v.has_value() && std::isfinite(*v);
}

/**
 * Days since 1970-01-01 for a proleptic Gregorian date.
 *
 * Howard Hinnant's days_from_civil, public domain. C++20 has this in <chrono>;
 * this library is C++17 and the alternative is timegm(), which is not portable
 * to the Windows build and drags in a timezone database question that a date
 * with no time of day has no business asking.
 */
long daysFromCivil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);             // [0, 399]
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;   // [0, 365]
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;            // [0, 146096]
    return static_cast<long>(era) * 146097L + static_cast<long>(doe) - 719468L;
}

/// YYYYMMDD to a day number, or nullopt when it is not eight digits or not a
/// real calendar date. The Dart original throws on both; a library cannot.
std::optional<long> parseYmd(const std::string& ymd) {
    if (ymd.size() < 8) return std::nullopt;
    for (int i = 0; i < 8; ++i) {
        if (ymd[i] < '0' || ymd[i] > '9') return std::nullopt;
    }
    const int year  = std::stoi(ymd.substr(0, 4));
    const int month = std::stoi(ymd.substr(4, 2));
    const int day   = std::stoi(ymd.substr(6, 2));
    if (month < 1 || month > 12 || day < 1 || day > 31) return std::nullopt;

    // daysFromCivil is linear in the day, so 31 April silently becomes 1 May
    // rather than failing. Compare against the first of the following month,
    // which is the only check that actually catches it.
    const long n = daysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
    const long next_month = (month == 12) ? daysFromCivil(year + 1, 1, 1)
                                          : daysFromCivil(year, static_cast<unsigned>(month + 1), 1);
    if (n >= next_month) return std::nullopt;
    return n;
}

}  // namespace

IndexBand bandFor(int index) {
    if (index >= 85) return IndexBand::Excellent;
    if (index >= 70) return IndexBand::Good;
    if (index >= 50) return IndexBand::Fair;
    return IndexBand::NeedsAttention;
}

const char* bandKey(IndexBand band) {
    switch (band) {
        case IndexBand::Excellent:      return "excellent";
        case IndexBand::Good:           return "good";
        case IndexBand::Fair:           return "fair";
        case IndexBand::NeedsAttention: return "needs_attention";
    }
    return "needs_attention";
}

std::optional<int> nightlyIndex(std::optional<double> usage_hours,
                                std::optional<double> ahi,
                                std::optional<double> leak_95) {
    double weight_sum = 0.0;
    double acc = 0.0;

    if (usable(usage_hours)) {
        weight_sum += kUsageWeight;
        acc += kUsageWeight * clamp01(*usage_hours / kUsageTargetHours);
    }
    if (usable(ahi)) {
        weight_sum += kAhiWeight;
        acc += kAhiWeight * clamp01((kAhiCeiling - *ahi) / (kAhiCeiling - kAhiFloor));
    }
    // A negative leak is a sentinel, not a measurement, and is dropped rather
    // than clamped to full credit.
    if (usable(leak_95) && *leak_95 >= 0.0) {
        weight_sum += kLeakWeight;
        acc += kLeakWeight * clamp01((kLeakCeiling - *leak_95) / (kLeakCeiling - kLeakFloor));
    }

    if (weight_sum == 0.0) return std::nullopt;
    return static_cast<int>(std::lround(acc / weight_sum * 100.0));
}

std::optional<double> trailingAverage(const std::vector<std::optional<int>>& newest_first,
                                      int nights) {
    if (nights <= 0) return std::nullopt;

    const size_t window = std::min(newest_first.size(), static_cast<size_t>(nights));
    long sum = 0;
    int count = 0;
    for (size_t i = 0; i < window; ++i) {
        if (newest_first[i].has_value()) {
            sum += *newest_first[i];
            ++count;
        }
    }
    if (count == 0) return std::nullopt;
    return static_cast<double>(sum) / count;
}

int currentStreak(const std::vector<NightUsage>& newest_first, double threshold_hours) {
    int streak = 0;
    long prev = 0;

    for (const auto& night : newest_first) {
        const auto day = parseYmd(night.date);
        if (!day) break;

        if (streak == 0) {
            if (night.usage_hours < threshold_hours) break;  // newest night already broke it
            streak = 1;
            prev = *day;
            continue;
        }
        if (prev - *day == 1 && night.usage_hours >= threshold_hours) {
            ++streak;
            prev = *day;
        } else {
            break;
        }
    }
    return streak;
}

int bestStreak(const std::vector<NightUsage>& nights, double threshold_hours) {
    std::vector<long> days;
    days.reserve(nights.size());
    for (const auto& night : nights) {
        if (night.usage_hours < threshold_hours) continue;
        if (const auto day = parseYmd(night.date)) days.push_back(*day);
    }
    std::sort(days.begin(), days.end());

    int best = 0;
    int run = 0;
    bool have_prev = false;
    long prev = 0;
    for (const long day : days) {
        if (have_prev && day == prev) continue;  // same night twice
        run = (have_prev && day - prev == 1) ? run + 1 : 1;
        best = std::max(best, run);
        prev = day;
        have_prev = true;
    }
    return best;
}

std::optional<int> milestoneFor(int streak) {
    for (const int milestone : kStreakMilestones) {
        if (streak == milestone) return milestone;
    }
    return std::nullopt;
}

}  // namespace cpapdash::parser
