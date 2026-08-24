#include "cpapdash/parser/OximetryCsv.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <sstream>
#include <string>
#include <vector>

namespace cpapdash::parser {

namespace {

// "06:53:07 Apr 12 2026" — the Checkme dialect, rendered as UTC.
//
// UTC, not local, because the CSV readers parse these back through timegm. A
// ring writes wall-clock with no zone and the reader picked a clock; the writer
// has to pick the same one or write-then-parse moves the night by the local
// offset. That cost four hours the first time round, caught by a round-trip
// test rather than by review.
std::string stampUtc(std::chrono::system_clock::time_point tp) {
    static const char* kMon[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                 "Jul","Aug","Sep","Oct","Nov","Dec"};
    std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
#ifdef _WIN32
    ::gmtime_s(&tm, &t);
#else
    ::gmtime_r(&t, &tm);
#endif
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d %s %d %d",
                  tm.tm_hour, tm.tm_min, tm.tm_sec,
                  kMon[tm.tm_mon < 0 || tm.tm_mon > 11 ? 0 : tm.tm_mon],
                  tm.tm_mday, tm.tm_year + 1900);
    return buf;
}

}  // namespace

std::string writeO2RingCsv(const OximetrySession& session) {
    std::ostringstream out;

    // Header copied from a real Checkme O2 Max export. Readers ignore it and
    // read positionally, but SleepHQ may not, so it is copied rather than
    // invented.
    out << "Time,Oxygen Level,Pulse Rate,Motion,O2 Reminder,PR Reminder\r\n";

    for (const auto& s : session.samples) {
        // A sample the ring could not read is written back as the SENTINEL it
        // came in as, never skipped.
        //
        // Skipping would silently compress the night: twenty minutes with the
        // ring off the finger would come back as a shorter session rather than
        // a session with a gap, and the interval auto-detection on re-import
        // would then measure the wrong cadence off the healed-over timestamps.
        //
        // 255 for both: the readers test `spo2 > 0 && spo2 <= 100` and
        // `hr > 0 && hr < 255`, so 255 fails both and round trips back to the
        // same 0xFF this sample already holds.
        const bool sp_ok = s.spo2 != 0xFF && s.invalid_flag == 0;
        const bool hr_ok = s.heart_rate != 0xFF;

        out << stampUtc(s.timestamp) << ','
            << (sp_ok ? static_cast<int>(s.spo2) : 255) << ','
            << (hr_ok ? static_cast<int>(s.heart_rate) : 255) << ','
            << static_cast<int>(s.motion)
            << ",0,0\r\n";   // the reminder columns: always present, never ours
    }

    return out.str();
}

std::string o2RingCsvFilename(const OximetrySession& session,
                              const std::string& device) {
    std::time_t t = std::chrono::system_clock::to_time_t(session.start_time);
    // Same clock as the rows, so the filename and the first timestamp agree.
    std::tm tm{};
#ifdef _WIN32
    ::gmtime_s(&tm, &t);
#else
    ::gmtime_r(&t, &tm);
#endif
    char stamp[24];
    std::snprintf(stamp, sizeof(stamp), "%04d%02d%02d%02d%02d%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);

    std::string name = device.empty() ? "O2Ring" : device;
    for (char& c : name) {
        if (c == '/' || c == '\\' || c == ',' || c == '"' || c == '\n' || c == '\r') c = '_';
    }
    return name + "_" + stamp + ".csv";
}

// ── The reader ───────────────────────────────────────────────────────────────

namespace {

// Split a CSV line honoring double-quoted fields, so a quoted timestamp that
// contains a comma -- "11:20:29PM Jun 19, 2026" -- stays one field. Quotes are
// stripped from the output.
std::vector<std::string> splitCsv(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    bool inq = false;
    for (char c : line) {
        if (c == '"') inq = !inq;
        else if (c == ',' && !inq) { out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    out.push_back(cur);
    return out;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// Portable "read this struct tm AS UTC". Uniquely named: Drogon exports its own
// timegm on Windows, and a plain one here collided at link time in hms-cpap.
std::time_t timegmUtc(std::tm* tm) {
#ifdef _WIN32
    return _mkgmtime(tm);
#else
    return timegm(tm);
#endif
}

enum class DateOrder { DayFirst, MonthFirst };

// Split "23/08/2026" (or 23-08-2026) into its three numbers. False for anything
// that is not exactly three numeric components, which is how a month-name date
// ("Apr 12 2026") declines the whole ambiguity question.
bool splitNumericDate(const std::string& s, int& a, int& b, int& year) {
    char sep1 = 0, sep2 = 0, extra = 0;
    int n = std::sscanf(s.c_str(), "%d%c%d%c%d%c", &a, &sep1, &b, &sep2, &year, &extra);
    if (n != 5) return false;                        // 6 means trailing junk
    if ((sep1 != '/' && sep1 != '-') || sep1 != sep2) return false;
    return true;
}

// Pull a YYYYMMDD out of an export filename, e.g.
// "WearO2 3719_20260823075304.csv" -> 2026, 8, 23.
bool filenameDateStamp(const std::string& filename, int& year, int& month, int& day) {
    for (size_t i = 0; i < filename.size();) {
        if (!std::isdigit((unsigned char)filename[i])) { ++i; continue; }
        size_t j = i;
        while (j < filename.size() && std::isdigit((unsigned char)filename[j])) ++j;
        size_t len = j - i;
        if (len == 8 || len == 14) {
            std::string d = filename.substr(i, 8);
            int y = std::atoi(d.substr(0, 4).c_str());
            int m = std::atoi(d.substr(4, 2).c_str());
            int dd = std::atoi(d.substr(6, 2).c_str());
            if (y >= 2000 && m >= 1 && m <= 12 && dd >= 1 && dd <= 31) {
                year = y; month = m; day = dd;
                return true;
            }
        }
        i = j;
    }
    return false;
}

// Split a Wellue time field into its clock token and its date remainder. The
// comma in "Jun 19, 2026" becomes a space first, so both the detector and the
// parser see the same shape -- the API had this and hms-cpap did not, which is
// precisely the kind of gap two copies of a reader accumulate.
bool splitClockAndDate(const std::string& field, std::string& clock, std::string& rest) {
    std::string t = trim(field);
    for (auto& c : t) if (c == ',') c = ' ';
    size_t sp = t.find(' ');
    if (sp == std::string::npos) return false;
    clock = t.substr(0, sp);
    rest = trim(t.substr(sp + 1));
    return true;
}

bool clockHasAmPm(const std::string& clock) {
    if (clock.size() < 2) return false;
    char c1 = (char)std::toupper((unsigned char)clock[clock.size() - 2]);
    char c2 = (char)std::toupper((unsigned char)clock.back());
    return c2 == 'M' && (c1 == 'A' || c1 == 'P');
}

// Parse one time field to UTC. See the header for the dialects and for why UTC.
bool parseTimestamp(const std::string& field, DateOrder order,
                    std::chrono::system_clock::time_point& out) {
    static const char* months[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                   "Jul","Aug","Sep","Oct","Nov","Dec"};
    std::string clock, rest;
    if (!splitClockAndDate(field, clock, rest)) return false;

    int ampm = 0;  // 0 none, 1 AM, 2 PM
    if (clockHasAmPm(clock)) {
        ampm = std::toupper((unsigned char)clock[clock.size() - 2]) == 'A' ? 1 : 2;
        clock = clock.substr(0, clock.size() - 2);
    }

    int hh = 0, mm = 0, ssec = 0;
    if (std::sscanf(clock.c_str(), "%d:%d:%d", &hh, &mm, &ssec) < 3) return false;
    if (ampm == 2 && hh < 12) hh += 12;       // PM
    if (ampm == 1 && hh == 12) hh = 0;        // 12 AM -> 00

    int day = 0, year = 0, month = 0;
    int na = 0, nb = 0, nyear = 0;
    if (splitNumericDate(rest, na, nb, nyear)) {
        // A component above 12 can only be the day, whatever the locale says.
        if (na > 12)      { day = na; month = nb; }
        else if (nb > 12) { month = na; day = nb; }
        else if (order == DateOrder::DayFirst) { day = na; month = nb; }
        else                                   { month = na; day = nb; }
        year = nyear;
    } else {
        char mon[16] = {};
        if (std::sscanf(rest.c_str(), "%15s %d %d", mon, &day, &year) < 3) return false;
        for (int i = 0; i < 12; ++i)
            if (std::strncmp(mon, months[i], 3) == 0) { month = i + 1; break; }
    }
    if (month < 1 || month > 12 || day < 1 || day > 31 || year < 2000) return false;

    std::tm tm{};
    tm.tm_year = year - 1900; tm.tm_mon = month - 1; tm.tm_mday = day;
    tm.tm_hour = hh; tm.tm_min = mm; tm.tm_sec = ssec;
    out = std::chrono::system_clock::from_time_t(timegmUtc(&tm));
    return true;
}

// Decide how to read this file's numeric dates. Header has the priority order.
DateOrder detectDateOrder(const std::string& content,
                          const std::string& day_hint,
                          const std::string& filename) {
    std::istringstream ss(content);
    std::string line;
    std::getline(ss, line);  // header

    bool have_first = false, have_prev = false, ampm_clock = false;
    int first_a = 0, first_b = 0, first_year = 0;
    int prev_a = 0, prev_b = 0;

    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (trim(line).empty()) continue;
        auto cols = splitCsv(line);
        if (cols.empty()) continue;

        std::string clock, rest;
        if (!splitClockAndDate(cols[0], clock, rest)) continue;

        int a = 0, b = 0, y = 0;
        // A month-name row tells us nothing about numeric order. Keep scanning
        // rather than bailing: a file can carry a stray unparseable first row
        // and still be perfectly readable after it.
        if (!splitNumericDate(rest, a, b, y)) continue;

        // 1. Only a day can exceed 12. The first definitive row settles the file.
        if (a > 12) return DateOrder::DayFirst;
        if (b > 12) return DateOrder::MonthFirst;

        if (clockHasAmPm(clock)) ampm_clock = true;

        if (!have_first) { first_a = a; first_b = b; first_year = y; have_first = true; }
        // 3. Whichever of the pair marches forward by one at a date change is the day.
        if (have_prev && (a != prev_a || b != prev_b)) {
            if (b == prev_b && a == prev_a + 1) return DateOrder::DayFirst;
            if (a == prev_a && b == prev_b + 1) return DateOrder::MonthFirst;
        }
        prev_a = a; prev_b = b; have_prev = true;
    }

    // 2. The night the caller is filing this under. Checked before the weaker
    //    heuristics because it came from the user's own choice, not a guess.
    if (have_first && day_hint.size() == 8) {
        int uy = std::atoi(day_hint.substr(0, 4).c_str());
        int um = std::atoi(day_hint.substr(4, 2).c_str());
        int ud = std::atoi(day_hint.substr(6, 2).c_str());
        if (uy == first_year) {
            if (ud == first_a && um == first_b) return DateOrder::DayFirst;
            if (um == first_a && ud == first_b) return DateOrder::MonthFirst;
        }
    }

    // 4. The export stamps YYYYMMDD into its own filename.
    int fy = 0, fm = 0, fd = 0;
    if (have_first && filenameDateStamp(filename, fy, fm, fd) && fy == first_year) {
        if (fd == first_a && fm == first_b) return DateOrder::DayFirst;
        if (fm == first_a && fd == first_b) return DateOrder::MonthFirst;
    }

    // 5/6.
    if (ampm_clock) return DateOrder::MonthFirst;
    return DateOrder::DayFirst;
}

}  // namespace

O2RingCsvRead readO2RingCsv(const std::string& content,
                            const std::string& filename,
                            const std::string& day_hint) {
    O2RingCsvRead out;
    out.session.filename = filename;

    const DateOrder order = detectDateOrder(content, day_hint, filename);
    out.date_order_day_first = (order == DateOrder::DayFirst);

    std::istringstream ss(content);
    std::string line;
    std::getline(ss, line);  // header

    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (trim(line).empty()) continue;
        auto cols = splitCsv(line);
        if (cols.size() < 3) continue;

        std::chrono::system_clock::time_point ts;
        if (!parseTimestamp(cols[0], order, ts)) continue;
        int s = std::atoi(trim(cols[1]).c_str());
        int h = std::atoi(trim(cols[2]).c_str());
        int mo = cols.size() > 3 ? std::atoi(trim(cols[3]).c_str()) : 0;

        // Sentinel readings (SpO2 255, HR 65535) and out-of-range values are
        // invalid. Mapped to 0xFF so OximetrySample::valid() excludes them, and
        // so a 16-bit HR sentinel cannot wrap into a plausible-looking uint8_t.
        const bool sp_ok = s > 0 && s <= 100;
        const bool hr_ok = h > 0 && h < 255;

        OximetrySample sample;
        sample.timestamp = ts;
        sample.spo2 = sp_ok ? (uint8_t)s : 0xFF;
        sample.heart_rate = hr_ok ? (uint8_t)h : 0xFF;
        sample.invalid_flag = sp_ok ? 0 : 1;
        sample.motion = (mo >= 0 && mo <= 255) ? (uint8_t)mo : 0;
        sample.vibration = 0;
        out.session.samples.push_back(sample);
    }

    if (out.session.samples.empty()) return out;

    // The base sample interval is the SMALLEST positive gap, not the first one:
    // a per-second export with a dropped row would otherwise read as 2s and
    // double the night's duration.
    long long interval = 0;
    for (size_t i = 1; i < out.session.samples.size(); ++i) {
        long long d = std::chrono::duration_cast<std::chrono::seconds>(
                          out.session.samples[i].timestamp -
                          out.session.samples[i - 1].timestamp).count();
        if (d > 0 && (interval == 0 || d < interval)) interval = d;
    }
    out.session.sample_interval = interval > 0 ? (double)interval : 1.0;

    out.session.start_time = out.session.samples.front().timestamp;
    out.session.end_time = out.session.samples.back().timestamp;
    out.session.duration_seconds =
        (int)(out.session.samples.size() * out.session.sample_interval);

    // Through calculateMetrics, the same call the .vld path makes, so a night
    // does not get different numbers for having arrived as text.
    out.session.metrics =
        VLDParser::calculateMetrics(out.session.samples, out.session.sample_interval);

    return out;
}

}  // namespace cpapdash::parser
