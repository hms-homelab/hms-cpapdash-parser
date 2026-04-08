#include "cpapdash/parser/EDFParser.h"
#include <iostream>
#include <cmath>

namespace cpapdash::parser {

std::vector<STRDailyRecord> EDFParser::parseSTRFile(
    const std::string& filepath,
    const std::string& device_id) {

    EDFFile edf;
    if (!edf.open(filepath)) {
        std::cerr << "STR: Failed to open " << filepath << std::endl;
        return {};
    }

    return parseSTRInternal(edf, device_id);
}

std::vector<STRDailyRecord> EDFParser::parseSTRFromBuffer(
    const uint8_t* data, size_t len,
    const std::string& device_id) {

    EDFFile edf;
    if (!edf.open(data, len)) {
        std::cerr << "STR: Failed to parse buffer" << std::endl;
        return {};
    }

    return parseSTRInternal(edf, device_id);
}

std::vector<STRDailyRecord> EDFParser::parseSTRInternal(
    EDFFile& edf,
    const std::string& device_id) {

    std::vector<STRDailyRecord> records;

    if (static_cast<int>(edf.record_duration) != 86400) {
        std::cerr << "STR: Unexpected record_duration=" << edf.record_duration
                  << " (expected 86400)" << std::endl;
        return records;
    }

    // Helper: read signal into vector, return empty on failure
    auto readSig = [&](const std::string& label) -> std::vector<double> {
        int idx = edf.findSignalExact(label);
        if (idx < 0) return {};
        std::vector<double> data;
        edf.readSignal(idx, data);
        return data;
    };

    // Read all signals we care about
    auto duration_data     = readSig("Duration");
    auto patient_hours_data = readSig("PatientHours");
    auto mask_events_data  = readSig("MaskEvents");
    auto ahi_data          = readSig("AHI");
    auto hi_data           = readSig("HI");
    auto ai_data           = readSig("AI");
    auto oai_data          = readSig("OAI");
    auto cai_data          = readSig("CAI");
    auto uai_data          = readSig("UAI");
    auto rin_data          = readSig("RIN");
    auto csr_data          = readSig("CSR");

    auto blow_press_95_data = readSig("BlowPress.95");
    auto blow_press_5_data  = readSig("BlowPress.5");
    auto mask_press_50_data = readSig("MaskPress.50");
    auto mask_press_95_data = readSig("MaskPress.95");
    auto mask_press_max_data = readSig("MaskPress.Max");

    auto leak_50_data  = readSig("Leak.50");
    auto leak_95_data  = readSig("Leak.95");
    auto leak_70_data  = readSig("Leak.70");
    auto leak_max_data = readSig("Leak.Max");

    auto spo2_50_data  = readSig("SpO2.50");
    auto spo2_95_data  = readSig("SpO2.95");
    auto spo2_max_data = readSig("SpO2.Max");

    auto resp_rate_50_data  = readSig("RespRate.50");
    auto resp_rate_95_data  = readSig("RespRate.95");
    auto resp_rate_max_data = readSig("RespRate.Max");
    auto tid_vol_50_data    = readSig("TidVol.50");
    auto tid_vol_95_data    = readSig("TidVol.95");
    auto tid_vol_max_data   = readSig("TidVol.Max");
    auto min_vent_50_data   = readSig("MinVent.50");
    auto min_vent_95_data   = readSig("MinVent.95");
    auto min_vent_max_data  = readSig("MinVent.Max");

    auto mode_data          = readSig("Mode");
    auto epr_level_data     = readSig("S.EPR.Level");
    auto press_setting_data = readSig("S.C.Press");
    auto max_press_data     = readSig("S.AS.MaxPress");
    auto min_press_data     = readSig("S.AS.MinPress");

    auto fault_device_data = readSig("Fault.Device");
    auto fault_alarm_data  = readSig("Fault.Alarm");

    // ASV settings (Mode=7: ASV Fixed EPAP, Mode=8: ASV Variable EPAP)
    auto asv_start_press_data = readSig("S.AV.StartPress");
    auto asv_epap_data        = readSig("S.AV.EPAP");
    auto asv_max_ps_data      = readSig("S.AV.MaxPS");
    auto asv_min_ps_data      = readSig("S.AV.MinPS");

    // ASVAuto settings (Mode=8 only)
    auto asvauto_min_epap_data = readSig("S.AA.MinEPAP");
    auto asvauto_max_epap_data = readSig("S.AA.MaxEPAP");

    // Target percentiles (ASV daily targets)
    auto tgt_ipap_50_data  = readSig("TgtIPAP.50");
    auto tgt_ipap_95_data  = readSig("TgtIPAP.95");
    auto tgt_ipap_max_data = readSig("TgtIPAP.Max");
    auto tgt_epap_50_data  = readSig("TgtEPAP.50");
    auto tgt_epap_95_data  = readSig("TgtEPAP.95");
    auto tgt_epap_max_data = readSig("TgtEPAP.Max");
    auto tgt_vent_50_data  = readSig("TgtVent.50");
    auto tgt_vent_95_data  = readSig("TgtVent.95");
    auto tgt_vent_max_data = readSig("TgtVent.Max");

    // MaskOn/MaskOff have 10 samples per record
    int mask_on_idx  = edf.findSignalExact("MaskOn");
    int mask_off_idx = edf.findSignalExact("MaskOff");
    std::vector<double> mask_on_data, mask_off_data;
    if (mask_on_idx >= 0)  edf.readSignal(mask_on_idx, mask_on_data);
    if (mask_off_idx >= 0) edf.readSignal(mask_off_idx, mask_off_data);

    // Helper: safe index access for 1-sample-per-record signals
    auto val = [](const std::vector<double>& v, int i) -> double {
        return (i >= 0 && static_cast<size_t>(i) < v.size()) ? v[i] : 0.0;
    };

    // Use calendar arithmetic to compute each day's noon (handles DST correctly)
    auto computeNoonForDay = [&edf](int day_offset) -> std::chrono::system_clock::time_point {
        std::tm t = {};
        t.tm_year = edf.start_year - 1900;
        t.tm_mon  = edf.start_month - 1;
        t.tm_mday = edf.start_day + day_offset;  // mktime normalizes overflow
        t.tm_hour = edf.start_hour;
        t.tm_min  = edf.start_minute;
        t.tm_sec  = edf.start_second;
        t.tm_isdst = -1;
        return std::chrono::system_clock::from_time_t(std::mktime(&t));
    };

    for (int rec = 0; rec < edf.actual_records; ++rec) {
        double dur = val(duration_data, rec);
        if (dur <= 0) continue;  // no therapy this day

        STRDailyRecord r;
        r.device_id = device_id;
        r.record_date = computeNoonForDay(rec);
        r.duration_minutes = dur;
        r.patient_hours = val(patient_hours_data, rec);
        r.mask_events = static_cast<int>(val(mask_events_data, rec));

        // Official indices
        r.ahi = val(ahi_data, rec);
        r.hi  = val(hi_data, rec);
        r.ai  = val(ai_data, rec);
        r.oai = val(oai_data, rec);
        r.cai = val(cai_data, rec);
        r.uai = val(uai_data, rec);
        r.rin = val(rin_data, rec);
        r.csr = val(csr_data, rec);

        // Pressure
        r.blow_press_95 = val(blow_press_95_data, rec);
        r.blow_press_5  = val(blow_press_5_data, rec);
        r.mask_press_50  = val(mask_press_50_data, rec);
        r.mask_press_95  = val(mask_press_95_data, rec);
        r.mask_press_max = val(mask_press_max_data, rec);

        // Leak -- EDF stores L/s, convert to L/min
        r.leak_50  = val(leak_50_data, rec) * 60.0;
        r.leak_95  = val(leak_95_data, rec) * 60.0;
        r.leak_70  = val(leak_70_data, rec) * 60.0;
        r.leak_max = val(leak_max_data, rec) * 60.0;

        // SpO2 -- -1 means no oximeter data
        double sp50 = val(spo2_50_data, rec);
        double sp95 = val(spo2_95_data, rec);
        double spmax = val(spo2_max_data, rec);
        r.spo2_50  = (sp50 > 0) ? sp50 : 0;
        r.spo2_95  = (sp95 > 0) ? sp95 : 0;
        r.spo2_max = (spmax > 0) ? spmax : 0;

        // Respiratory
        r.resp_rate_50  = val(resp_rate_50_data, rec);
        r.resp_rate_95  = val(resp_rate_95_data, rec);
        r.resp_rate_max = val(resp_rate_max_data, rec);
        r.tid_vol_50    = val(tid_vol_50_data, rec);
        r.tid_vol_95    = val(tid_vol_95_data, rec);
        r.tid_vol_max   = val(tid_vol_max_data, rec);
        r.min_vent_50   = val(min_vent_50_data, rec);
        r.min_vent_95   = val(min_vent_95_data, rec);
        r.min_vent_max  = val(min_vent_max_data, rec);

        // Settings
        r.mode             = static_cast<int>(val(mode_data, rec));
        r.epr_level        = val(epr_level_data, rec);
        r.pressure_setting = val(press_setting_data, rec);
        r.max_pressure     = val(max_press_data, rec);
        r.min_pressure     = val(min_press_data, rec);

        // Faults
        r.fault_device = static_cast<int>(val(fault_device_data, rec));
        r.fault_alarm  = static_cast<int>(val(fault_alarm_data, rec));

        // ASV settings (only populated for ASV modes 7/8)
        if (r.mode == 7 || r.mode == 8) {
            auto optVal = [&val](const std::vector<double>& v, int i) -> std::optional<double> {
                double d = val(v, i);
                return (d > 0) ? std::optional<double>(d) : std::nullopt;
            };

            r.asv_start_press = optVal(asv_start_press_data, rec);
            r.asv_epap        = optVal(asv_epap_data, rec);
            r.asv_max_ps      = optVal(asv_max_ps_data, rec);
            r.asv_min_ps      = optVal(asv_min_ps_data, rec);

            // ASVAuto (Mode=8 only)
            if (r.mode == 8) {
                r.asvauto_min_epap = optVal(asvauto_min_epap_data, rec);
                r.asvauto_max_epap = optVal(asvauto_max_epap_data, rec);
            }

            // Target percentiles
            r.tgt_ipap_50  = optVal(tgt_ipap_50_data, rec);
            r.tgt_ipap_95  = optVal(tgt_ipap_95_data, rec);
            r.tgt_ipap_max = optVal(tgt_ipap_max_data, rec);
            r.tgt_epap_50  = optVal(tgt_epap_50_data, rec);
            r.tgt_epap_95  = optVal(tgt_epap_95_data, rec);
            r.tgt_epap_max = optVal(tgt_epap_max_data, rec);
            r.tgt_vent_50  = optVal(tgt_vent_50_data, rec);
            r.tgt_vent_95  = optVal(tgt_vent_95_data, rec);
            r.tgt_vent_max = optVal(tgt_vent_max_data, rec);
        }

        // MaskOn/MaskOff -- 10 slots per record, values are minutes since noon
        if (!mask_on_data.empty() && !mask_off_data.empty()) {
            int base = rec * 10;  // 10 samples per record
            for (int slot = 0; slot < 10; ++slot) {
                int idx = base + slot;
                if (static_cast<size_t>(idx) >= mask_on_data.size()) break;

                double on_min  = mask_on_data[idx];
                double off_min = mask_off_data[idx];

                // -1 means unused slot
                if (on_min < 0 || off_min < 0) continue;
                // both 0 with valid other = session at noon (unlikely), skip
                if (on_min == 0 && off_min == 0) continue;
                // on == off means zero-length (skip)
                if (on_min == off_min) continue;

                auto on_tp  = r.record_date + std::chrono::seconds(static_cast<int>(on_min * 60));
                auto off_tp = r.record_date + std::chrono::seconds(static_cast<int>(off_min * 60));
                r.mask_pairs.push_back({on_tp, off_tp});
            }
        }

        records.push_back(std::move(r));
    }

    return records;
}

} // namespace cpapdash::parser
