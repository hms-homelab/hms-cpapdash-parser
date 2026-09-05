#pragma once

#include <string>
#include <vector>
#include <chrono>
#include <optional>
#include <map>

namespace cpapdash::parser {

/**
 * Event types for respiratory events
 */
enum class EventType {
    APNEA,
    HYPOPNEA,
    RERA,
    CSR,
    OBSTRUCTIVE,
    CENTRAL,
    CLEAR_AIRWAY,
    FLOW_LIMITATION,
    PERIODIC_BREATHING,
    LARGE_LEAK,
    VIBRATORY_SNORE,
    DESATURATION,  // SpO2 desaturation (detected from vitals, NOT counted toward AHI)
    OTHER          // Recognised as an annotation, belongs to no index. Recorded so a new
                   // firmware label shows up in the data instead of quietly inflating a
                   // clinical index; still inside total_events (SDD-004 2.1).
};

/**
 * Device manufacturer identifier. PHILIPS and BMC are DETECT-only: named by
 * detectManufacturer() so a foreign card is labeled and rejected cleanly, but
 * createParser() has no parser for them (returns nullptr) -- detection is
 * broader than parse support.
 */
enum class DeviceManufacturer {
    UNKNOWN,
    RESMED,
    LOWENSTEIN,
    PHILIPS,
    BMC
};

std::string eventTypeToString(EventType type);

/**
 * SleepEvent - Respiratory event (apnea, hypopnea, etc.)
 */
struct SleepEvent {
    EventType event_type;
    std::chrono::system_clock::time_point timestamp;
    double duration_seconds;
    std::optional<std::string> details;

    SleepEvent() = default;
    SleepEvent(EventType t, std::chrono::system_clock::time_point ts, double dur)
        : event_type(t), timestamp(ts), duration_seconds(dur) {}
};

/**
 * VitalSample - Vital signs (SpO2, Heart Rate) from SAD.edf
 */
struct VitalSample {
    std::chrono::system_clock::time_point timestamp;
    std::optional<double> spo2;         // Oxygen saturation %
    std::optional<int> heart_rate;      // bpm

    VitalSample() = default;
    VitalSample(std::chrono::system_clock::time_point ts)
        : timestamp(ts) {}
};

/**
 * DesatEvent - SpO2 desaturation event detected from vitals.
 *
 * Kept separate from SleepEvent so it never inflates total_events / AHI.
 * Consumers may surface these alongside respiratory events (e.g. an
 * "events" table row with type "Desaturation"), but the parser keeps the
 * AHI math clean by detecting them independently.
 */
struct DesatEvent {
    std::chrono::system_clock::time_point onset;  // start of the drop
    double duration_seconds = 0;                  // onset -> recovery
    double nadir = 0;                             // lowest SpO2 reached (%)
    double depth = 0;                             // baseline_at_open - nadir (%)
};

/**
 * BreathingSummary - Summary statistics for breathing waveforms (from BRP.edf)
 * Includes calculated respiratory metrics
 */
struct BreathingSummary {
    std::chrono::system_clock::time_point timestamp;

    // Raw flow/pressure stats
    double avg_flow_rate = 0;
    double max_flow_rate = 0;
    double min_flow_rate = 0;
    double avg_pressure = 0;
    double max_pressure = 0;
    double min_pressure = 0;

    // Calculated respiratory metrics
    std::optional<double> respiratory_rate;      // Breaths per minute
    std::optional<double> tidal_volume;          // mL per breath (avg)
    std::optional<double> minute_ventilation;    // L/min (RR x TV)
    std::optional<double> inspiratory_time;      // Ti (seconds, avg)
    std::optional<double> expiratory_time;       // Te (seconds, avg)
    std::optional<double> ie_ratio;              // I:E ratio
    std::optional<double> flow_limitation;       // Flow limitation score 0-1
    std::optional<double> leak_rate;             // Unintentional leak L/min

    // PLD-derived metrics (machine's own calculations)
    std::optional<double> therapy_pressure;    // cmH2O (PLD Press.2s) -- the DELIVERED
                                               // therapy pressure, which is what OSCAR and
                                               // SleepHQ plot as "Pressure". Distinct from
                                               // mask_pressure below, which is the measured
                                               // pressure at the mask and reads ~0.8 lower.
    std::optional<double> mask_pressure;       // cmH2O (PLD MaskPress.2s)
    std::optional<double> epr_pressure;        // cmH2O (PLD EprPress.2s)
    std::optional<double> snore_index;         // 0-5 (PLD Snore.2s)
    std::optional<double> target_ventilation;  // L/min (PLD TgtVent.2s, ASV only)

    // Leak spread WITHIN this minute. The per-minute mean alone flattens blow-outs:
    // a real 87.6 L/min spike inside one minute of mostly-zero leak averages to 4.8,
    // so the night's peak reads an order of magnitude low. Carrying the extremes keeps
    // the peak recoverable from stored data.
    std::optional<double> leak_min;            // L/min
    std::optional<double> leak_max;            // L/min

    // Percentile statistics
    std::optional<double> flow_p95;              // 95th percentile flow
    std::optional<double> pressure_p95;          // 95th percentile pressure

    BreathingSummary() = default;
    BreathingSummary(std::chrono::system_clock::time_point ts)
        : timestamp(ts),
          avg_flow_rate(0), max_flow_rate(0), min_flow_rate(0),
          avg_pressure(0), max_pressure(0), min_pressure(0) {}
};

/**
 * Breath - A single detected breath cycle (from BRP flow, zero-crossing).
 *
 * The per-minute BreathingSummary aggregates these; this is the breath-level
 * per-breath detail for breath-by-breath inspection. Populated only when
 * raw flow is available; empty for summary-only sessions.
 */
struct Breath {
    std::chrono::system_clock::time_point onset;  // inspiration start (absolute)
    double tidal_volume = 0;      // mL
    double inspiratory_time = 0;  // s
    double expiratory_time = 0;   // s
    double flow_limitation = 0;   // 0..1
};

/**
 * SessionMetrics - Aggregated metrics for a CPAP session (standards-based)
 */
struct SessionMetrics {
    // ===== EVENT METRICS =====
    int total_events = 0;
    double ahi = 0.0;  // Apnea-Hypopnea Index (events/hour)

    // Event breakdown
    int obstructive_apneas = 0;
    int central_apneas = 0;
    int hypopneas = 0;
    int reras = 0;
    int clear_airway_apneas = 0;
    // ResMed's bare 'Apnea' label: an apnea it did not classify. Counts toward the
    // AHI numerator, so it has to be persisted or AHI cannot be reconstructed from
    // the stored counts (docs/RESMED_CALCULATION_RULES.md section 5).
    int unclassified_apneas = 0;
    // Annotations that match no known event. Recorded, part of total_events, part of
    // no index (SDD-004 2.2).
    int other_events = 0;

    // Event statistics
    std::optional<double> avg_event_duration;     // seconds
    std::optional<double> max_event_duration;     // seconds
    std::optional<double> time_in_apnea_percent;  // % of session

    // ===== PRESSURE METRICS =====
    std::optional<double> avg_pressure;           // cmH2O
    std::optional<double> min_pressure;           // cmH2O
    std::optional<double> max_pressure;           // cmH2O
    std::optional<double> pressure_p95;           // 95th percentile cmH2O
    std::optional<double> pressure_p50;           // Median cmH2O

    // ===== LEAK METRICS =====
    std::optional<double> avg_leak_rate;          // L/min
    std::optional<double> max_leak_rate;          // L/min
    std::optional<double> leak_p95;               // 95th percentile L/min
    std::optional<double> leak_p50;               // Median L/min

    // ===== FLOW METRICS =====
    std::optional<double> avg_flow_rate;          // L/min
    std::optional<double> max_flow_rate;          // L/min
    std::optional<double> flow_p95;               // 95th percentile L/min

    // ===== RESPIRATORY METRICS =====
    std::optional<double> avg_respiratory_rate;   // breaths/minute
    std::optional<double> avg_tidal_volume;       // mL per breath
    std::optional<double> avg_minute_ventilation; // L/min
    std::optional<double> avg_inspiratory_time;   // Ti seconds
    std::optional<double> avg_expiratory_time;    // Te seconds
    std::optional<double> avg_ie_ratio;           // I:E ratio
    std::optional<double> avg_flow_limitation;    // 0-1 score

    // ===== PLD-DERIVED METRICS =====
    // Therapy pressure (PLD Press.2s). The pressure* fields above describe the
    // measured mask waveform; these describe what the machine actually delivered,
    // and are the OSCAR/SleepHQ-comparable numbers.
    std::optional<double> avg_therapy_pressure;    // cmH2O
    std::optional<double> min_therapy_pressure;    // cmH2O
    std::optional<double> max_therapy_pressure;    // cmH2O
    std::optional<double> therapy_pressure_p95;    // cmH2O
    std::optional<double> therapy_pressure_p50;    // cmH2O

    std::optional<double> avg_mask_pressure;       // cmH2O
    std::optional<double> avg_epr_pressure;        // cmH2O
    std::optional<double> avg_snore;               // 0-5 average
    // ASV-specific (NULL for CPAP/APAP)
    std::optional<double> avg_target_ventilation;  // L/min
    std::optional<int> therapy_mode;               // 0=CPAP, 1=APAP, 7=ASV, 8=ASVAuto

    // ===== SPO2 METRICS =====
    std::optional<double> avg_spo2;               // %
    std::optional<double> min_spo2;               // %
    std::optional<double> max_spo2;               // %
    std::optional<double> spo2_p95;               // 95th percentile %
    std::optional<double> spo2_p50;               // Median %
    std::optional<int> spo2_drops;                // Count of desaturations
    std::optional<double> odi;                    // Oxygen Desaturation Index (desats/hour)

    // ===== HEART RATE METRICS =====
    std::optional<int> avg_heart_rate;            // bpm
    std::optional<int> min_heart_rate;            // bpm
    std::optional<int> max_heart_rate;            // bpm
    std::optional<int> hr_p95;                    // 95th percentile bpm
    std::optional<int> hr_p50;                    // Median bpm

    // ===== USAGE METRICS =====
    std::optional<double> usage_hours;            // Total hours
    std::optional<double> usage_percent;          // % of 8-hour target

    // ===== DATE LABEL (for range queries) =====
    std::string sleep_day;  // "YYYY-MM-DD", filled by getMetricsForDateRange()
};

/**
 * DeviceSettings - Therapy device configuration for the session
 */
struct DeviceSettings {
    std::optional<int> therapy_mode;        // 0=CPAP, 1=APAP, 2=BiPAP, 7=ASV, 8=ASVAuto
    std::optional<double> set_pressure;     // cmH2O (fixed CPAP)
    std::optional<double> min_pressure;     // cmH2O (Auto range)
    std::optional<double> max_pressure;     // cmH2O (Auto range)
    std::optional<double> ipap;             // cmH2O (BiPAP/ASV)
    std::optional<double> epap;             // cmH2O (BiPAP/ASV)
    std::optional<int> flex_mode;           // Exhalation-relief mode: 0=Off, 1=Flex, 2=AFlex, 3=Rise, 4=BiFlex
    std::optional<int> flex_level;          // 1-3
    std::optional<int> ramp_time;           // minutes
    std::optional<double> ramp_pressure;    // cmH2O
    std::optional<int> humidifier_mode;     // 0=Off, 1=Fixed, 2=Adaptive, 3=HeatedTube
    std::optional<int> humidifier_level;    // 0-5
    std::optional<int> tube_temp;           // 0-5 (heated tube)
    std::optional<int> mask_type;           // 0=Pillows, 1=Nasal, 2=FullFace
    std::optional<int> hose_diameter;       // mm (22, 15, 12)
};

/**
 * ParsedSession - Complete CPAP session data
 */
struct ParsedSession {
    // Device info
    std::string device_id;
    std::string device_name;
    std::string serial_number;
    std::optional<int> model_id;
    std::optional<int> version_id;
    DeviceManufacturer manufacturer = DeviceManufacturer::UNKNOWN;

    // Session metadata
    std::optional<std::chrono::system_clock::time_point> session_start;
    std::optional<std::chrono::system_clock::time_point> session_end;
    // THERAPY time, not the envelope: the sum of the data each BRP checkpoint
    // actually holds, with the mask-off gaps between them excluded. This used to
    // be session_end - session_start, which counted every break as therapy: a
    // night of 1 + 114 + 107 minutes split by a 7h evening break reported 11h22m
    // instead of 3h42m, and deflated AHI by the same factor because AHI is
    // events / duration (support ticket 87, confirmed against OSCAR).
    std::optional<int> duration_seconds;

    // file_start -> seconds of data in that checkpoint, which is what makes the
    // sum above safe to recompute. Keyed rather than accumulated because the same
    // BRP is legitimately parsed more than once: a merge can re-parse an earlier
    // checkpoint, and a live file GROWS between reads. Assigning by key makes a
    // re-parse idempotent and lets a grown file simply replace its own older,
    // smaller value; a running total would double-count both.
    std::map<std::chrono::system_clock::time_point, int> brp_spans;

    int data_records = 0;
    bool file_complete = false;

    // EDF file growth tracking
    int extra_records = 0;
    bool growing = false;

    // Session status tracking
    enum class Status {
        IN_PROGRESS,
        COMPLETED
    };
    Status status = Status::IN_PROGRESS;
    bool has_summary = false;
    bool has_events = false;

    // File path references (optional, populated when parsing from disk)
    std::optional<std::string> brp_file_path;
    std::optional<std::string> eve_file_path;
    std::optional<std::string> sad_file_path;
    std::optional<std::string> pld_file_path;
    std::optional<std::string> csl_file_path;

    // Device settings for this session
    std::optional<DeviceSettings> settings;

    // Session data
    std::vector<SleepEvent> events;
    std::vector<VitalSample> vitals;
    std::vector<BreathingSummary> breathing_summary;
    std::vector<DesatEvent> desaturations;  // SpO2 desats (kept out of events/AHI)
    std::vector<Breath> breaths;            // breath-by-breath detail (empty if no raw flow)

    /**
     * Signal samples at the rate the machine actually wrote them, kept alongside the
     * per-minute rows.
     *
     * breathing_summary is one row per minute, so a percentile taken over it is a
     * percentile of 30-sample means rather than of the signal. For a spiky channel
     * that is materially wrong: on a real night the 95th of the leak samples is
     * 8.4 L/min (matching the machine's own STR summary to the decimal) where the
     * 95th of the minute means comes out 8.68, and a 87.6 L/min blow-out averages
     * down to 10.8 and disappears from the night's peak entirely.
     *
     * Samples rather than precomputed statistics because a night is often several
     * recordings merged into one session, and percentiles do not merge -- appending
     * the samples and taking the percentile once over the whole night does.
     *
     * Already in display units (leak L/min, pressures cmH2O). Empty for parsers that
     * do not populate it (Prisma, Philips), which leaves their numbers unchanged.
     */
    struct NativeSamples {
        std::vector<double> leak;     // L/min
        std::vector<double> therapy;  // cmH2O (ResMed PLD Press.2s)
        std::vector<double> mask;     // cmH2O (ResMed PLD MaskPress.2s)

        void append(const NativeSamples& o) {
            leak.insert(leak.end(), o.leak.begin(), o.leak.end());
            therapy.insert(therapy.end(), o.therapy.begin(), o.therapy.end());
            mask.insert(mask.end(), o.mask.begin(), o.mask.end());
        }
    };
    NativeSamples native_samples;

    // Aggregated metrics
    std::optional<SessionMetrics> metrics;

    /**
     * Calculate aggregated metrics from session data
     */
    void calculateMetrics();

    /**
     * Get human-readable session summary
     */
    std::string toString() const;
};

/**
 * STRDailyRecord - Daily therapy summary from STR.edf
 *
 * ResMed writes STR.edf at the SD root with 81 signals, 1 record per day
 * (86400s duration). Contains official ResMed values: AHI, mask timing,
 * pressure/leak percentiles, device settings, and cumulative hours.
 */
struct STRDailyRecord {
    std::string device_id;
    std::chrono::system_clock::time_point record_date;  // noon of this day

    // Mask timing (on/off pairs as timestamps)
    std::vector<std::pair<
        std::chrono::system_clock::time_point,
        std::chrono::system_clock::time_point>> mask_pairs;
    int mask_events = 0;

    // Duration
    double duration_minutes = 0;
    double patient_hours = 0;

    // Official ResMed indices (events/hour)
    double ahi = 0, hi = 0, ai = 0, oai = 0, cai = 0, uai = 0;
    double rin = 0;                // RERA index
    double csr = 0;               // Cheyne-Stokes minutes

    // Pressure (cmH2O)
    double blow_press_95 = 0, blow_press_5 = 0;
    double mask_press_50 = 0, mask_press_95 = 0, mask_press_max = 0;

    // Leak (L/min -- stored as L/s in EDF, multiply by 60)
    double leak_50 = 0, leak_95 = 0, leak_70 = 0, leak_max = 0;

    // SpO2 (%)
    double spo2_50 = 0, spo2_95 = 0, spo2_max = 0;

    // Respiratory
    double resp_rate_50 = 0, resp_rate_95 = 0, resp_rate_max = 0;
    double tid_vol_50 = 0, tid_vol_95 = 0, tid_vol_max = 0;
    double min_vent_50 = 0, min_vent_95 = 0, min_vent_max = 0;

    // Settings
    int mode = 0;
    double epr_level = 0, pressure_setting = 0;
    double max_pressure = 0, min_pressure = 0;

    // ===== SDD-064: WHICH FAMILY OF MACHINE WROTE THIS =====
    //
    // `mode` alone is NOT enough to know what therapy this is, because the mode
    // enum differs BETWEEN MACHINE FAMILIES. A bi-level reporting 8 was being read
    // through the ASV enum and shown to its owner as "ASVAuto" -- a treatment for
    // central apnea, which is not what he is on.
    //
    // The model code cannot separate them either: an AirSense (AutoSet) and an
    // AirCurve (bi-level) BOTH report MID=46, verified across six real devices.
    //
    // The SIGNAL SET is unambiguous and self-describing, so that is what we use.
    // Read the family first, then interpret `mode` WITHIN it.
    enum class Family { Unknown, Cpap, AutoSet, BiLevel, Asv };
    Family family = Family::Unknown;

    // ===== BI-LEVEL SETTINGS (S.VA.* = VAuto, S.S.* = S) =====
    // Present on an AirCurve and absent on an AirSense. These are the pressures the
    // patient is actually prescribed; a single averaged mask pressure is a number a
    // bi-level machine is rarely at, because it alternates EPAP<->IPAP every breath.
    std::optional<double> bl_max_ipap;    // S.VA.MaxIPAP
    std::optional<double> bl_min_epap;    // S.VA.MinEPAP
    std::optional<double> bl_ps;          // S.VA.PS   (pressure support)
    std::optional<double> bl_ipap;        // S.S.IPAP  (fixed bi-level)
    std::optional<double> bl_epap;        // S.S.EPAP  (fixed bi-level)

    // Faults
    int fault_device = 0, fault_alarm = 0;

    // ===== ASV SETTINGS (Mode=7: ASV Fixed EPAP, Mode=8: ASV Variable EPAP) =====
    std::optional<double> asv_start_press;
    std::optional<double> asv_epap;
    std::optional<double> asv_max_ps;
    std::optional<double> asv_min_ps;

    // ASVAuto settings (Mode=8 only)
    std::optional<double> asvauto_min_epap;
    std::optional<double> asvauto_max_epap;

    // ===== TARGET PERCENTILES (ASV daily targets) =====
    std::optional<double> tgt_ipap_50;
    std::optional<double> tgt_ipap_95;
    std::optional<double> tgt_ipap_max;
    std::optional<double> tgt_epap_50;
    std::optional<double> tgt_epap_95;
    std::optional<double> tgt_epap_max;
    std::optional<double> tgt_vent_50;
    std::optional<double> tgt_vent_95;
    std::optional<double> tgt_vent_max;

    bool hasTherapy() const { return duration_minutes > 0; }
};

} // namespace cpapdash::parser
