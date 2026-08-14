#pragma once

#include <string>
#include <cpapdash/parser/VLDParser.h>

namespace cpapdash::parser {

/**
 * Writes an OximetrySession back out as the CSV a Wellue / Viatom ring's own
 * app exports, which is the file SleepHQ accepts for oximetry.
 *
 * Lives here rather than in either consumer because hms-cpap and
 * hms-cpapdash-api both need to produce this exact file, and two copies of a
 * format definition drift. The reader that defines the format is per-consumer
 * (each has its own upload path); this is the one writer.
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
