// SDD-013: the machine's wall clock as a label, on request.

#include "cpapdash/parser/WallClock.h"

#include <cstdint>

namespace cpapdash::parser {

namespace {

constexpr int64_t kDay = 86400;

// The default is the host's zone, so a thread that never opens a scope runs the
// code it always ran.
thread_local ClockFrame t_frame = ClockFrame::HostLocal;

int64_t floorDiv(int64_t a, int64_t b) {
    return a / b - ((a % b != 0) && ((a < 0) != (b < 0)));
}

// Days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant's
// days_from_civil), and back.
int64_t daysFromCivil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

void civilFromDays(int64_t z, int& y, int& m, int& d) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    y = static_cast<int>(static_cast<int64_t>(yoe) + era * 400 + (m <= 2));
}

// The fields counted as UTC. The month is folded into the year first; the day,
// hour, minute and second may overflow and simply add, as mktime normalises them.
int64_t labelSeconds(int64_t year, int64_t month0, int64_t mday, int64_t hour, int64_t minute,
                     int64_t second) {
    year += floorDiv(month0, 12);
    month0 -= floorDiv(month0, 12) * 12;
    const int64_t days = daysFromCivil(year, static_cast<unsigned>(month0 + 1), 1) + (mday - 1);
    return days * kDay + hour * 3600 + minute * 60 + second;
}

std::tm labelFields(int64_t seconds) {
    const int64_t days = floorDiv(seconds, kDay);
    const int64_t tod = seconds - days * kDay;
    int y = 0, m = 0, d = 0;
    civilFromDays(days, y, m, d);
    std::tm out{};
    out.tm_year = y - 1900;
    out.tm_mon = m - 1;
    out.tm_mday = d;
    out.tm_hour = static_cast<int>(tod / 3600);
    out.tm_min = static_cast<int>((tod % 3600) / 60);
    out.tm_sec = static_cast<int>(tod % 60);
    return out;
}

}  // namespace

ClockFrame clockFrame() { return t_frame; }

WallClockLabels::WallClockLabels() : previous_(t_frame) { t_frame = ClockFrame::Label; }

WallClockLabels::~WallClockLabels() { t_frame = previous_; }

std::chrono::system_clock::time_point wallClockLabel(const WallClock& w) {
    return std::chrono::system_clock::time_point(std::chrono::seconds(
        labelSeconds(w.year, w.month - 1, w.day, w.hour, w.minute, w.second)));
}

WallClock wallClockOf(std::chrono::system_clock::time_point label) {
    const auto s =
        std::chrono::floor<std::chrono::seconds>(label.time_since_epoch()).count();
    const std::tm t = labelFields(static_cast<int64_t>(s));
    return WallClock{t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec};
}

std::time_t wallClockToTimeT(std::tm& t) {
    if (t_frame == ClockFrame::HostLocal) return std::mktime(&t);
    return static_cast<std::time_t>(labelSeconds(t.tm_year + 1900, t.tm_mon, t.tm_mday, t.tm_hour,
                                                 t.tm_min, t.tm_sec));
}

std::tm wallClockFromTimeT(std::time_t t) {
    if (t_frame == ClockFrame::HostLocal) {
        std::tm lt{};
#if defined(_WIN32)
        localtime_s(&lt, &t);
#else
        localtime_r(&t, &lt);
#endif
        return lt;
    }
    return labelFields(static_cast<int64_t>(t));
}

}  // namespace cpapdash::parser
