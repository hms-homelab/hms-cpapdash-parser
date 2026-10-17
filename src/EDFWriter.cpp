#include "cpapdash/parser/EDFWriter.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace cpapdash::parser {

namespace {

// A header field: the text, space-padded to its width, never longer.
void putField(std::vector<uint8_t>& out, const std::string& text, size_t width) {
    const size_t n = std::min(text.size(), width);
    out.insert(out.end(), text.begin(), text.begin() + static_cast<long>(n));
    out.insert(out.end(), width - n, static_cast<uint8_t>(' '));
}

std::string numberField(double v) {
    char buf[32];
    // Whole numbers print without a point, as EDF writers do; others with as
    // many decimals as fit the field, trimmed of trailing zeros.
    if (std::fabs(v - std::round(v)) < 1e-9) {
        std::snprintf(buf, sizeof(buf), "%.0f", v);
    } else {
        std::snprintf(buf, sizeof(buf), "%.6g", v);
    }
    return buf;
}

std::string intField(long long v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%lld", v);
    return buf;
}

std::string two(int v) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%02d", v);
    return buf;
}

const char* const kMonths[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                               "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};

size_t wholeRecords(const EDFWriteSignal& s) {
    if (s.samples_per_record <= 0) return 0;
    return s.samples.size() / static_cast<size_t>(s.samples_per_record);
}

}  // namespace

std::vector<uint8_t> writeEDF(const EDFWriteSpec& spec) {
    std::vector<uint8_t> out;
    if (spec.signals.empty() || spec.record_duration <= 0) return out;
    size_t records = SIZE_MAX;
    for (const auto& s : spec.signals) records = std::min(records, wholeRecords(s));
    if (records == 0 || records == SIZE_MAX) return out;

    const size_t ns           = spec.signals.size();
    const size_t header_bytes = 256 + 256 * ns;
    out.reserve(header_bytes);

    // ── main header (256) ──
    putField(out, "0", 8);
    putField(out, spec.patient, 80);
    putField(out, spec.recording, 80);
    putField(out, two(spec.start.day) + "." + two(spec.start.month) + "." + two(spec.start.year % 100), 8);
    putField(out, two(spec.start.hour) + "." + two(spec.start.minute) + "." + two(spec.start.second), 8);
    putField(out, intField(static_cast<long long>(header_bytes)), 8);
    putField(out, "", 44);                       // plain EDF, as a ResMed's files are
    putField(out, intField(static_cast<long long>(records)), 8);
    putField(out, numberField(spec.record_duration), 8);
    putField(out, intField(static_cast<long long>(ns)), 4);

    // ── signal headers (256 each, field by field across all signals) ──
    for (const auto& s : spec.signals) putField(out, s.label, 16);
    for (size_t i = 0; i < ns; i++) putField(out, "", 80);          // transducer
    for (const auto& s : spec.signals) putField(out, s.phys_dim, 8);
    for (const auto& s : spec.signals) putField(out, numberField(s.phys_min), 8);
    for (const auto& s : spec.signals) putField(out, numberField(s.phys_max), 8);
    for (const auto& s : spec.signals) putField(out, intField(s.dig_min), 8);
    for (const auto& s : spec.signals) putField(out, intField(s.dig_max), 8);
    for (size_t i = 0; i < ns; i++) putField(out, "", 80);          // prefiltering
    for (const auto& s : spec.signals) putField(out, intField(s.samples_per_record), 8);
    for (size_t i = 0; i < ns; i++) putField(out, "", 32);          // reserved

    // ── data records: for each record, each signal's samples, int16 LE ──
    for (size_t r = 0; r < records; r++) {
        for (const auto& s : spec.signals) {
            const double span_phys = s.phys_max - s.phys_min;
            const double span_dig  = static_cast<double>(s.dig_max - s.dig_min);
            const size_t base      = r * static_cast<size_t>(s.samples_per_record);
            for (int k = 0; k < s.samples_per_record; k++) {
                double v = s.samples[base + static_cast<size_t>(k)];
                if (!(v == v)) v = 0.0;                                   // NaN reads as zero
                v = std::clamp(v, s.phys_min, s.phys_max);
                long d = s.dig_min;
                if (span_phys > 0 && span_dig > 0)
                    d = std::lround(s.dig_min + (v - s.phys_min) * span_dig / span_phys);
                d = std::clamp(d, static_cast<long>(s.dig_min), static_cast<long>(s.dig_max));
                const auto u = static_cast<uint16_t>(static_cast<int16_t>(d));
                out.push_back(static_cast<uint8_t>(u & 0xFF));
                out.push_back(static_cast<uint8_t>(u >> 8));
            }
        }
    }
    return out;
}

std::string resmedRecordingField(const WallClock& start, const std::string& serial,
                                 int model_id, int vid) {
    const int m = std::clamp(start.month, 1, 12);
    std::string s = "Startdate " + two(start.day) + "-" + kMonths[m - 1] + "-" +
                    intField(start.year) + " X X X";
    if (!serial.empty()) s += " SRN=" + serial;
    if (model_id > 0) s += " MID=" + intField(model_id);
    if (vid > 0) s += " VID=" + intField(vid);
    return s;
}

std::string resmedFileName(const WallClock& start, const std::string& kind) {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d_%02d%02d%02d_%s.edf", start.year, start.month,
                  start.day, start.hour, start.minute, start.second, kind.c_str());
    return buf;
}

std::vector<uint8_t> writeResMedBRP(const StreamedBRP& brp) {
    const size_t n = std::min(brp.flow_lpm.size(), brp.press_cmh2o.size());
    EDFWriteSpec spec;
    spec.recording       = resmedRecordingField(brp.start, brp.serial);
    spec.start           = brp.start;
    spec.record_duration = 1.0;
    EDFWriteSignal flow;
    flow.label              = "Flow.40ms";
    flow.phys_dim           = "L/min";
    flow.phys_min           = -300.0;   // 0.0092 L/min a step, the machine's own 0.01
    flow.phys_max           = 300.0;
    flow.samples_per_record = 25;
    flow.samples.assign(brp.flow_lpm.begin(), brp.flow_lpm.begin() + static_cast<long>(n));
    EDFWriteSignal press;
    press.label              = "Press.40ms";
    press.phys_dim           = "cmH2O";
    press.phys_min           = 0.0;
    press.phys_max           = 40.0;
    press.samples_per_record = 25;
    press.samples.assign(brp.press_cmh2o.begin(), brp.press_cmh2o.begin() + static_cast<long>(n));
    spec.signals = {flow, press};
    return writeEDF(spec);
}

std::vector<uint8_t> writeResMedPLD(const StreamedPLD& pld) {
    struct Chan {
        const char*                label;
        const char*                dim;
        double                     lo, hi;
        const std::vector<double>* v;
    };
    // In the machine's order: the three pressures first (the reader's prefix
    // logic relies on nothing but the labels, the order is for a human).
    const Chan chans[] = {
        {"MaskPress.2s", "cmH2O", 0.0, 40.0, &pld.mask_press_cmh2o},
        {"Press.2s", "cmH2O", 0.0, 40.0, &pld.press_cmh2o},
        {"EprPress.2s", "cmH2O", 0.0, 40.0, &pld.epr_press_cmh2o},
        {"Leak.2s", "L/s", 0.0, 4.0, &pld.leak_lps},
        {"RespRate.2s", "bpm", 0.0, 60.0, &pld.resp_rate},
        {"TidVol.2s", "L", 0.0, 4.0, &pld.tidal_volume_l},
        {"MinVent.2s", "L/min", 0.0, 60.0, &pld.minute_vent_lpm},
        {"Snore.2s", "", 0.0, 10.0, &pld.snore},
        {"FlowLim.2s", "", 0.0, 1.0, &pld.flow_limitation},
    };
    size_t n = SIZE_MAX;
    for (const auto& c : chans)
        if (!c.v->empty()) n = std::min(n, c.v->size());
    if (n == SIZE_MAX) return {};

    EDFWriteSpec spec;
    spec.recording       = resmedRecordingField(pld.start, pld.serial);
    spec.start           = pld.start;
    spec.record_duration = 2.0;
    for (const auto& c : chans) {
        if (c.v->empty()) continue;
        EDFWriteSignal s;
        s.label              = c.label;
        s.phys_dim           = c.dim;
        s.phys_min           = c.lo;
        s.phys_max           = c.hi;
        s.samples_per_record = 1;
        s.samples.assign(c.v->begin(), c.v->begin() + static_cast<long>(n));
        spec.signals.push_back(std::move(s));
    }
    return writeEDF(spec);
}

std::vector<uint8_t> writeResMedSAD(const StreamedSAD& sad) {
    const size_t n = std::max(sad.spo2_pct.size(), sad.pulse_bpm.size());
    if (n == 0) return {};
    auto padded = [n](const std::vector<double>& v) {
        std::vector<double> out(n, 0.0);
        for (size_t i = 0; i < v.size(); i++) out[i] = std::isnan(v[i]) ? 0.0 : v[i];
        return out;
    };
    EDFWriteSpec spec;
    spec.recording       = resmedRecordingField(sad.start, sad.serial);
    spec.start           = sad.start;
    spec.record_duration = 1.0;
    EDFWriteSignal spo2;
    spo2.label              = "SpO2";
    spo2.phys_dim           = "%";
    spo2.phys_min           = 0.0;
    spo2.phys_max           = 100.0;
    spo2.samples_per_record = 1;
    spo2.samples            = padded(sad.spo2_pct);
    EDFWriteSignal pulse;
    pulse.label              = "Pulse";
    pulse.phys_dim           = "bpm";
    pulse.phys_min           = 0.0;
    pulse.phys_max           = 300.0;
    pulse.samples_per_record = 1;
    pulse.samples            = padded(sad.pulse_bpm);
    spec.signals = {spo2, pulse};
    return writeEDF(spec);
}

}  // namespace cpapdash::parser
