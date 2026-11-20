#pragma once

#include <chrono>
#include <ctime>

namespace cpapdash::parser {

/// The wall clock a machine writes into its files: ITS local time, as the
/// card's file names and EDF headers carry it.
struct WallClock {
    int year = 2000, month = 1, day = 1, hour = 0, minute = 0, second = 0;
};

// SDD-013: the machine's wall clock as a label, on request.
//
// A card stamps its files with its own wall clock and no zone. By default the
// parser reads those numbers through the host's std::mktime, so a time point it
// returns is "the machine's wall clock read in the host's zone", and the host's
// daylight saving can move or fold it. A LABEL is the same six numbers counted
// as if they were UTC: no zone, nothing to move. A caller asks for labels with a
// WallClockLabels scope; without one, every parser runs exactly the code it ran
// before.

/// How this thread's parses turn a machine's wall clock into a time point.
enum class ClockFrame {
    HostLocal,  ///< std::mktime / localtime in the host's zone (the default)
    Label       ///< the wall clock counted as UTC, no zone (SDD-013)
};

/// This thread's frame.
ClockFrame clockFrame();

/**
 * While an object of this type lives, every parse on THIS thread returns
 * labels: session, file, minute, breath, event and STR day times are the
 * machine's wall clock counted as UTC, to be read back with gmtime (or
 * wallClockOf). The previous frame comes back when it ends, so scopes nest.
 * Other threads are not affected.
 */
class WallClockLabels {
public:
    WallClockLabels();
    ~WallClockLabels();
    WallClockLabels(const WallClockLabels&) = delete;
    WallClockLabels& operator=(const WallClockLabels&) = delete;

private:
    ClockFrame previous_;
};

/** The label of a wall clock, whatever this thread's frame. Pure arithmetic. */
std::chrono::system_clock::time_point wallClockLabel(const WallClock& w);

/** A label's six numbers back (sub-seconds dropped). */
WallClock wallClockOf(std::chrono::system_clock::time_point label);

/**
 * The parser's one conversion of a machine's wall clock, in this thread's
 * frame. HostLocal: std::mktime(&t), unchanged (it normalises `t` and returns
 * -1 when it cannot). Label: the fields counted as UTC, with an out-of-range
 * month, day, hour, minute or second normalised as mktime would; `t` is not
 * written back. tm_isdst is not read.
 */
std::time_t wallClockToTimeT(std::tm& t);

/**
 * The way back, in this thread's frame. HostLocal: localtime, unchanged.
 * Label: the label's fields (tm_wday, tm_yday and tm_isdst are left 0).
 */
std::tm wallClockFromTimeT(std::time_t t);

}  // namespace cpapdash::parser
