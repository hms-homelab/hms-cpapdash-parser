#pragma once

#include <string>
#include <cpapdash/parser/VLDParser.h>

namespace cpapdash::parser {

/**
 * Reads a Wellue / Viatom "O2 Ring" CSV export into the same OximetrySession
 * the .vld path produces, so everything downstream is identical for both.
 *
 * THE ONE READER. hms-cpap and hms-cpapdash-api each grew their own, and the
 * two drifted exactly where a format definition always drifts -- in the corners
 * one consumer hit and the other had not yet. The API learned to resolve an
 * ambiguous numeric date from the night the client filed it under (ticket 84)
 * and to strip the comma out of `"Jun 19, 2026"` before sniffing; hms-cpap
 * learned to compute its metrics through calculateMetrics instead of a
 * hand-rolled proxy. Neither knew what the other knew. This is the union.
 *
 * Dialects accepted, all three seen in the wild:
 *
 *     06:53:07 Apr 12 2026        24-hour, month name, unquoted
 *     "11:20:29PM Jun 19, 2026"   12-hour AM/PM, quoted, comma after the day
 *     07:53:04 23/08/2026         numeric, order resolved as described below
 *
 * NUMERIC DATES are the hard part. The Wellue phone app writes the date in the
 * PHONE's locale, so "05/08/2026" is the 5th of August or the 8th of May
 * depending on whose phone exported it, and NOTHING IN THE FILE SAYS WHICH.
 * Resolved by, in priority order:
 *
 *   1. any component > 12 -- only a day can be that, whatever the locale;
 *   2. `day_hint` (YYYYMMDD), the night the caller is filing this under, which
 *      it got from the user's own choice rather than from a guess;
 *   3. consecutive-day progression across the file -- whichever of the pair
 *      marches forward by one at a date change is the day;
 *   4. a YYYYMMDD stamp in `filename`, since the export names itself after its
 *      first sample;
 *   5. an AM/PM clock, which implies a US locale, so month-first;
 *   6. day-first, which is every numeric-slash locale except the US.
 *
 * Both hints are optional and neither can fail a parse. A file whose dates
 * carry a month NAME never reaches rules 2-6; splitNumericDate declines it.
 *
 * TIMEZONE-FREE, and this file must stay that way. A ring records a wall clock
 * with no zone attached, so timestamps are read through timegm and carried as
 * if UTC -- the same clock writeO2RingCsv renders. Whose evening that wall
 * clock actually was is the CALLER's question: hms-cpapdash-api answers it per
 * account at the database layer. Reaching for localtime here would silently
 * re-file every night by the offset of whatever machine did the parsing.
 */
struct O2RingCsvRead {
    OximetrySession session;
    /// Which way an ambiguous numeric date was read. Meaningless for a
    /// month-name file; reported so a caller can surface the choice.
    bool date_order_day_first = true;
};

O2RingCsvRead readO2RingCsv(const std::string& content,
                            const std::string& filename = "",
                            const std::string& day_hint = "");

/**
 * Writes an OximetrySession back out as the CSV a Wellue / Viatom ring's own
 * app exports, which is the file SleepHQ accepts for oximetry.
 *
 * Lives here rather than in either consumer because hms-cpap and
 * hms-cpapdash-api both need to produce this exact file, and two copies of a
 * format definition drift.
 *
 * Real exports come in two dialects and a device picks both its header text and
 * its timestamp style together:
 *
 *     Time,Oxygen Level,Pulse Rate,Motion,O2 Reminder,PR Reminder
 *     00:16:55 Jun 26 2026,95,86,29,0,0            (Checkme O2 Max)
 *
 *     Time,SpO2(%),Pulse Rate(bpm),Motion,SpO2 Reminder,PR Reminder,
 *     "11:20:29PM Jun 19, 2026",89,60,0,0,0,       (O2Ring S, trailing comma)
 *
 * This writes the FIRST and only the first. A writer that can emit two dialects
 * has a bug waiting in the branch nobody exercises; readers stay permissive
 * because files in the wild are not.
 *
 * Timestamps are rendered as UTC, because that is the clock the readers parse
 * them in (timegm). Getting that wrong moves the whole night.
 */
std::string writeO2RingCsv(const OximetrySession& session);

/**
 * The name the ring's own app would give this file:
 *   <device>_<YYYYMMDDHHMMSS>.csv   e.g. "O2Ring S_20260619232029.csv"
 * `device` falls back to "O2Ring" when empty, and is sanitised: the value can
 * come from a database and it lands in a filename and a multipart part name.
 */
std::string o2RingCsvFilename(const OximetrySession& session,
                              const std::string& device = "");

}  // namespace cpapdash::parser
