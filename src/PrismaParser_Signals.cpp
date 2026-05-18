#ifdef CPAPDASH_WITH_LOWENSTEIN

#include "cpapdash/parser/PrismaParser.h"
#include "cpapdash/parser/EDFFile.h"
#include <iostream>
#include <algorithm>
#include <numeric>
#include <cmath>

namespace cpapdash::parser {

namespace {

constexpr double HPA_TO_CMH2O = 1.0197;
constexpr int SAMPLES_PER_MINUTE = 60;  // 1s record duration

double percentile(std::vector<double>& data, double p) {
    if (data.empty()) return 0;
    std::sort(data.begin(), data.end());
    double idx = p / 100.0 * (data.size() - 1);
    size_t lo = static_cast<size_t>(idx);
    size_t hi = std::min(lo + 1, data.size() - 1);
    double frac = idx - lo;
    return data[lo] * (1.0 - frac) + data[hi] * frac;
}

struct SignalData {
    std::vector<double> pressure;
    std::vector<double> resp_flow;
    std::vector<double> leak;
    std::vector<double> breath_freq;
    std::vector<double> breath_vol;
    std::vector<double> minute_vent;
    std::vector<double> ie_ratio;
    std::vector<double> obstruct_level;
    std::vector<double> spo2;
    std::vector<double> heart_rate;
    std::vector<double> epap;
    std::vector<double> ipap;
    std::vector<double> eepap;
};

} // anonymous namespace

bool PrismaParser::parseSignalWmedf(const std::string& filepath, ParsedSession& session) {
    EDFFile edf;
    if (!edf.open(filepath)) {
        std::cerr << "PrismaParser: cannot open WMEDF: " << filepath << std::endl;
        return false;
    }

    session.session_start = edf.getStartTime();
    session.data_records = edf.actual_records;

    if (edf.record_duration > 0 && edf.actual_records > 0) {
        int total_seconds = static_cast<int>(edf.actual_records * edf.record_duration);
        session.duration_seconds = total_seconds;
        session.session_end = *session.session_start +
            std::chrono::seconds(total_seconds);
    }

    // Read all available signals
    SignalData sig;
    auto readChannel = [&](const std::string& label, std::vector<double>& out) {
        int idx = edf.findSignalExact(label);
        if (idx < 0) idx = edf.findSignal(label);
        if (idx >= 0) edf.readSignal(idx, out);
    };

    readChannel("Pressure", sig.pressure);
    readChannel("RespFlow", sig.resp_flow);
    readChannel("LeakFlowBreath", sig.leak);
    readChannel("BreathFrequency", sig.breath_freq);
    readChannel("BreathVolume", sig.breath_vol);
    readChannel("MV", sig.minute_vent);
    readChannel("InspExpirRel", sig.ie_ratio);
    readChannel("ObstructLevel", sig.obstruct_level);
    readChannel("SpO2", sig.spo2);
    readChannel("HeartFrequency", sig.heart_rate);
    readChannel("EPAPsoll", sig.epap);
    readChannel("IPAPsoll", sig.ipap);
    readChannel("EEPAPsoll", sig.eepap);

    // Aggregate into per-minute BreathingSummary
    // Most signals are 1 sample/second (except Pressure at 5/s and RespFlow at 10/s)
    // Use the lowest-rate signals for minute boundaries
    int num_records = edf.actual_records;
    int num_minutes = (num_records + SAMPLES_PER_MINUTE - 1) / SAMPLES_PER_MINUTE;

    for (int m = 0; m < num_minutes; ++m) {
        int rec_start = m * SAMPLES_PER_MINUTE;
        int rec_end = std::min(rec_start + SAMPLES_PER_MINUTE, num_records);
        if (rec_start >= rec_end) break;

        auto ts = *session.session_start + std::chrono::seconds(rec_start);
        BreathingSummary bs(ts);

        // Pressure (may be at higher sample rate - use per-record values)
        if (!sig.pressure.empty()) {
            int pressure_spr = 1;
            int pidx = edf.findSignalExact("Pressure");
            if (pidx < 0) pidx = edf.findSignal("Pressure");
            if (pidx >= 0) pressure_spr = edf.signals[pidx].samples_per_record;

            int ps = rec_start * pressure_spr;
            int pe = std::min(rec_end * pressure_spr, static_cast<int>(sig.pressure.size()));
            if (ps < pe) {
                double sum = 0, mn = 1e9, mx = -1e9;
                for (int i = ps; i < pe; ++i) {
                    double v = sig.pressure[i] * HPA_TO_CMH2O;
                    sum += v;
                    mn = std::min(mn, v);
                    mx = std::max(mx, v);
                }
                bs.avg_pressure = sum / (pe - ps);
                bs.min_pressure = mn;
                bs.max_pressure = mx;
            }
        }

        // Flow (may be at higher sample rate)
        if (!sig.resp_flow.empty()) {
            int flow_spr = 1;
            int fidx = edf.findSignalExact("RespFlow");
            if (fidx < 0) fidx = edf.findSignal("RespFlow");
            if (fidx >= 0) flow_spr = edf.signals[fidx].samples_per_record;

            int fs = rec_start * flow_spr;
            int fe = std::min(rec_end * flow_spr, static_cast<int>(sig.resp_flow.size()));
            if (fs < fe) {
                double sum = 0, mn = 1e9, mx = -1e9;
                for (int i = fs; i < fe; ++i) {
                    sum += sig.resp_flow[i];
                    mn = std::min(mn, sig.resp_flow[i]);
                    mx = std::max(mx, sig.resp_flow[i]);
                }
                bs.avg_flow_rate = sum / (fe - fs);
                bs.min_flow_rate = mn;
                bs.max_flow_rate = mx;
            }
        }

        // 1-sample-per-record signals: simple average over the minute
        auto avgRange = [](const std::vector<double>& v, int s, int e) -> std::optional<double> {
            if (v.empty() || s >= static_cast<int>(v.size())) return std::nullopt;
            e = std::min(e, static_cast<int>(v.size()));
            if (s >= e) return std::nullopt;
            double sum = 0;
            int count = 0;
            for (int i = s; i < e; ++i) {
                if (v[i] != 0) { sum += v[i]; ++count; }
            }
            return count > 0 ? std::optional<double>(sum / count) : std::nullopt;
        };

        bs.leak_rate = avgRange(sig.leak, rec_start, rec_end);
        bs.respiratory_rate = avgRange(sig.breath_freq, rec_start, rec_end);
        bs.tidal_volume = avgRange(sig.breath_vol, rec_start, rec_end);
        bs.minute_ventilation = avgRange(sig.minute_vent, rec_start, rec_end);
        bs.ie_ratio = avgRange(sig.ie_ratio, rec_start, rec_end);

        // Obstruction level: 0-100% -> 0-1 score
        auto ol = avgRange(sig.obstruct_level, rec_start, rec_end);
        if (ol) bs.flow_limitation = *ol / 100.0;

        // EPAP as mask/epr pressure
        auto ep = avgRange(sig.epap, rec_start, rec_end);
        if (ep) bs.epr_pressure = *ep * HPA_TO_CMH2O;

        session.breathing_summary.push_back(std::move(bs));
    }

    // Vitals: SpO2 and HeartFrequency at 1 sample/second -> per-second VitalSamples
    if (!sig.spo2.empty() || !sig.heart_rate.empty()) {
        int n = std::max(sig.spo2.size(), sig.heart_rate.size());
        for (int i = 0; i < n; ++i) {
            auto ts = *session.session_start + std::chrono::seconds(i);
            VitalSample vs(ts);
            if (i < static_cast<int>(sig.spo2.size()) && sig.spo2[i] > 0) {
                vs.spo2 = sig.spo2[i];
            }
            if (i < static_cast<int>(sig.heart_rate.size()) && sig.heart_rate[i] > 0) {
                vs.heart_rate = static_cast<int>(sig.heart_rate[i]);
            }
            if (vs.spo2 || vs.heart_rate) {
                session.vitals.push_back(std::move(vs));
            }
        }
    }

    return true;
}

} // namespace cpapdash::parser

#endif // CPAPDASH_WITH_LOWENSTEIN
