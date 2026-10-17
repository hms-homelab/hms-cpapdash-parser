#pragma once
//
// EDF files written by us, in the layout the readers in this library already
// take. A night that reaches us as a live stream rather than as card files (an
// AirSense 11 over its own Bluetooth) is laid out as the BRP and PLD files the
// card would have carried, so everything from the parser on is untouched.
//
// Two layers. writeEDF() lays out any plain EDF: the 256-byte main header, one
// 256-byte signal header per signal, then the data records as little-endian
// int16, physical values quantised into each signal's digital range exactly
// as EDFFile::readSignal() scales them back. writeResMedBRP() and
// writeResMedPLD() name and scale the signals the way parseBRPFile() and
// parsePLDFile() look for them, with the units those readers assume:
//
//   BRP, 1 s records:  Flow.40ms (L/min, 25 per record), Press.40ms (cmH2O, 25)
//   PLD, 2 s records:  MaskPress.2s, Press.2s, EprPress.2s (cmH2O), Leak.2s
//                      (L/s: the reader multiplies by 60), RespRate.2s (/min),
//                      TidVol.2s (L: the reader multiplies by 1000), MinVent.2s
//                      (L/min), Snore.2s, FlowLim.2s (0..1), one per record
//
// The recording field carries "SRN=<serial>" the way the machine writes it, so
// parseDeviceInfo() reads the machine's serial from a streamed file as from a
// card's.
//
#include <cstdint>
#include <string>
#include <vector>

namespace cpapdash::parser {

/// One signal to write: its header fields and its physical samples.
struct EDFWriteSignal {
    std::string label;                 // up to 16 characters
    std::string phys_dim;              // up to 8
    double      phys_min = -1.0;
    double      phys_max = 1.0;
    int         dig_min  = -32768;
    int         dig_max  = 32767;
    int         samples_per_record = 1;
    std::vector<double> samples;       // physical values; clamped to [phys_min, phys_max]
};

/// The wall clock a machine writes into its files: ITS local time, as the
/// card's file names and EDF headers carry it.
struct WallClock {
    int year = 2000, month = 1, day = 1, hour = 0, minute = 0, second = 0;
};

struct EDFWriteSpec {
    std::string patient   = "X X X X";   // up to 80
    std::string recording;               // up to 80: resmedRecordingField() for a ResMed layout
    WallClock   start;
    double      record_duration = 1.0;   // seconds per data record
    std::vector<EDFWriteSignal> signals;
};

/// Lay out the file. The record count is the smallest whole number of records
/// any signal can fill; trailing samples that do not fill a record are
/// dropped, never padded, so a file never claims time it has no data for.
/// Empty when there is no signal or no whole record.
std::vector<uint8_t> writeEDF(const EDFWriteSpec& spec);

/// "Startdate 17-OCT-2026 X X X SRN=<serial> MID=<model> VID=<vid>", the
/// recording field a ResMed writes (MID and VID left out when 0).
std::string resmedRecordingField(const WallClock& start, const std::string& serial,
                                 int model_id = 0, int vid = 0);

/// "20261017_062013_BRP.edf": the name a ResMed gives the file, from the
/// machine's wall clock; kind is "BRP", "PLD", "SAD", "EVE" or "CSL".
std::string resmedFileName(const WallClock& start, const std::string& kind);

/// A streamed night's breathing, 25 Hz each, in the units the BRP reader
/// expects. The two vectors are trimmed to the shorter.
struct StreamedBRP {
    WallClock   start;
    std::string serial;
    std::vector<double> flow_lpm;       // L/min
    std::vector<double> press_cmh2o;    // cmH2O
};
std::vector<uint8_t> writeResMedBRP(const StreamedBRP& brp);

/// A streamed night's slow channels, one value per 2 s record, in the units
/// the PLD reader expects. Vectors are trimmed to the shortest non-empty one;
/// an empty vector leaves that signal out of the file.
struct StreamedPLD {
    WallClock   start;
    std::string serial;
    std::vector<double> mask_press_cmh2o;
    std::vector<double> press_cmh2o;        // the therapy (inspiratory) pressure
    std::vector<double> epr_press_cmh2o;    // the expiratory pressure
    std::vector<double> leak_lps;           // L/s
    std::vector<double> resp_rate;          // breaths per minute
    std::vector<double> tidal_volume_l;     // litres
    std::vector<double> minute_vent_lpm;    // L/min
    std::vector<double> snore;              // the machine's index
    std::vector<double> flow_limitation;    // 0..1
};
std::vector<uint8_t> writeResMedPLD(const StreamedPLD& pld);

}  // namespace cpapdash::parser
