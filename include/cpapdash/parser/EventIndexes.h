#pragma once

#include <string>

namespace cpapdash::parser {

/**
 * The seven respiratory indexes, and the event-type mapping that feeds them.
 *
 * Pure arithmetic. No I/O, no database, no dates. It lives here for the same
 * reason SleepIndex.h does: hms-cpapdash-api, cpapdash-ingest and cpapdash-app
 * would otherwise each own a copy of the rule below and there would be nothing
 * keeping them equal.
 *
 * WHY THIS EXISTS AT ALL. ResMed's STR.edf carries its own AHI, AI, HI, OAI,
 * CAI, UAI and RIN, but every one of those channels is a 0..2400 integer scaled
 * by 0.1, so the file can hold ONE decimal and the machine FLOORS into it: a
 * true 0.49 is stored as 0.4. Measured on a real card, 14 of 15 days match
 * floor() and none match round(). We therefore compute the indexes ourselves
 * and take from STR only what the machine alone knows -- mask-on time and its
 * own percentiles. See docs/RESMED_CALCULATION_RULES.md.
 *
 * *** THE MAPPING IS THE HALF THAT DRIFTS, NOT THE ARITHMETIC. ***
 * Dividing a count by hours is not where two implementations disagree. WHICH
 * events land in which numerator is. One real example from this codebase: RIN
 * was briefly going to be sourced from RERA-classified events, which at the
 * time also swallowed every annotation the classifier did not recognise. That
 * reads as obviously correct in two places while meaning two different things.
 * So the mapping is stated here, once, and consumers ask this header rather
 * than reimplementing it.
 */

/// Per-type event counts for one scope (a session, or a whole night).
///
/// Populate from the parser's classification, or from stored rows. A SQL
/// consumer counting stored `events` rows maps them by `event_type`, whose
/// spellings are exactly what `eventTypeToString` emits.
struct EventCounts {
    int obstructive  = 0;  ///< event_type 'Obstructive'
    int central      = 0;  ///< event_type 'Central'
    int clear_airway = 0;  ///< event_type 'Clear Airway' (Philips; ResMed emits none)
    int unclassified = 0;  ///< event_type 'Apnea' -- ResMed's bare "Apnea" label,
                           ///< which is its UNCLASSIFIED apnea. Reconciles 175/175
                           ///< against STR's own UAI channel. It counts toward AHI
                           ///< and was for a long time counted but never persisted.
    int hypopnea     = 0;  ///< event_type 'Hypopnea'

    /// The RIN numerator: AROUSALS.
    ///
    /// *** IF YOU ARE WRITING SQL, READ THIS LINE, NOT THE ONE YOU EXPECTED. ***
    /// Count `details = 'Arousal'`. Do NOT count `event_type = 'RERA'`.
    ///
    /// They are the same thing only for rows written after EventType::OTHER
    /// existed. Before that, the classifier's catch-all assigned RERA to every
    /// annotation it could not recognise, so on historical rows 'RERA' means
    /// either "an arousal" or "a label nobody classified", and there is no way
    /// to tell them apart afterwards. `details = 'Arousal'` is correct across
    /// both eras.
    ///
    /// This warning is here rather than only in a note further down because a
    /// transcription of the obvious-looking expression is exactly how the
    /// floored-AHI bug was copied verbatim from one service into another.
    ///
    /// MEASURED on the live events table, 4073 stored RERA rows: 3557 carry
    /// details='Arousal' and 516 carry details NULL, which are the pre-OTHER
    /// catch-all. So keying RIN on event_type='RERA' inflates it by 12.7%
    /// across stored history.
    ///
    /// There are NO case or whitespace variants -- only 'Arousal' and NULL --
    /// so an exact `details = 'Arousal'` is correct and does NOT need
    /// lower() or trim(). Said explicitly because the instinct is to defend
    /// against variants that do not exist, and that defence would also swallow
    /// a future label nobody has looked at.
    int arousal      = 0;

    /// Recorded, but deliberately in NO index: RERA-other, CSR, flow limitation,
    /// vibratory snore, large leak, desaturation, and the OTHER bucket. They stay
    /// inside total_events; using total_events as an AHI numerator over-counted
    /// badly on machines that flag many flow-limitation events.
    ///
    /// *** THIS IS THE MOST LIKELY WAY TO GET THE SQL WRONG. *** A numerator
    /// written as count(*) over the night's event rows reproduces every
    /// arithmetic expectation and is still wrong in production. Measured on the
    /// live events table, 35,040 rows: Vibratory Snore 1680, Flow Limitation
    /// 1001, Large Leak 172, CSR 3 -- 2856 events that belong to no index at
    /// all, plus 4073 RERA which belong only to RIN. A count(*) AHI numerator
    /// is inflated by roughly 25%.
    ///
    /// The fixture has a column for this and a row where it is large, so a
    /// consumer that counts everything fails the contract rather than passing
    /// it and being wrong later.
    int excluded_from_indexes = 0;
};

/// The seven indexes, events per hour. Full precision: rounding is a DISPLAY
/// concern and must not happen here or at any storage or query layer.
struct RespiratoryIndexes {
    double ahi = 0.0;  ///< all apneas + hypopneas
    double ai  = 0.0;  ///< all apneas
    double hi  = 0.0;  ///< hypopneas
    double oai = 0.0;  ///< obstructive apneas
    double cai = 0.0;  ///< central apneas
    double uai = 0.0;  ///< unclassified apneas
    double rin = 0.0;  ///< respiratory-effort-related arousals
};

/// The AHI numerator: every apnea, plus hypopneas.
inline int ahiNumerator(const EventCounts& c) {
    return c.obstructive + c.central + c.clear_airway + c.unclassified + c.hypopnea;
}

/// The AI numerator: every apnea, no hypopneas. Clear-airway is included so the
/// definition survives Philips; ResMed emits none, so ResMed still reconciles.
inline int aiNumerator(const EventCounts& c) {
    return c.obstructive + c.central + c.clear_airway + c.unclassified;
}

/**
 * Compute all seven indexes.
 *
 * `hours` MUST be the final therapy hours for the scope. On ResMed that is
 * STR's own `Duration` (measured: `floor(n/Duration)` equals the STR channel on
 * 164 of 165 nights, while `PatientHours` -- a lifetime counter -- matches 0 of
 * 165). Do NOT pass a duration derived from file spans.
 *
 * *** IN SQL, DIVIDE BY 60.0 AND NOT BY 60. *** The stored duration is
 * `daily_summary.duration_minutes`, an INT. In Postgres `duration_minutes / 60`
 * is INTEGER division, so 7.9 hours becomes 7 and every index built on it comes
 * out about 13% high -- silently, and in the direction that overstates a
 * patient's disease. This function takes `hours` already computed, so the
 * fixture structurally cannot catch the mistake; it can only be caught by
 * reading it here.
 *
 * Returns all zeros when `hours <= 0` rather than dividing. A caller that has
 * not yet learned the night's duration should not publish the result.
 *
 * FOR A WHOLE NIGHT, sum the COUNTS across the night's sessions and pass the
 * summed hours. Never average per-session indexes, and never sum across two
 * transports of the same night -- a zip upload and a bridge can both carry one
 * physical night, and adding them roughly doubles both halves, which leaves the
 * ratio plausible while every stored count is twice reality.
 */
inline RespiratoryIndexes computeIndexes(const EventCounts& c, double hours) {
    RespiratoryIndexes r;
    if (hours <= 0.0) return r;
    r.ahi = ahiNumerator(c) / hours;
    r.ai  = aiNumerator(c)  / hours;
    r.hi  = c.hypopnea      / hours;
    r.oai = c.obstructive   / hours;
    r.cai = c.central       / hours;
    r.uai = c.unclassified  / hours;
    r.rin = c.arousal       / hours;
    return r;
}

/**
 * RIN's numerator, stated because it is the one that has already bitten.
 *
 * RIN counts AROUSALS. In this parser an annotation whose description contains
 * "arousal" is classified RERA, and RIN computed from arousals alone reconciles
 * against STR's own RIN channel on 172 of 175 nights.
 *
 * The trap is in STORED history rather than in live parsing. Before
 * EventType::OTHER existed, the classifier's catch-all assigned RERA to EVERY
 * annotation it did not recognise, so `event_type = 'RERA'` on an old row can
 * mean "an arousal" or "a label nobody classified". A SQL consumer reading
 * historical rows should therefore key RIN on the raw description
 * (`details = 'Arousal'`), which is correct across both eras, rather than on
 * `event_type = 'RERA'`, which is only correct for rows written after that fix.
 *
 * (Markers such as "Recording starts" are dropped by EDFFile::readAnnotations
 * before classification and never became events, so they are not part of this
 * problem despite appearing in a raw annotation census.)
 */
inline constexpr const char* kRinNumeratorNote =
    "RIN counts arousals. On stored history key on details='Arousal', not "
    "event_type='RERA': before EventType::OTHER, RERA was also the catch-all.";

/**
 * Whether a night's own event data is complete enough to trust a computed index.
 *
 * STR floors, so the machine's own figure bounds the true numerator from below:
 * `n_true >= str_index * hours`. A count under that bound means events are
 * missing from our copy and the computed index would read LOW, which understates
 * a patient's disease. Such a night must fall back to STR's floored value, and
 * the source must be recorded so the fallback is visible.
 *
 * Duration-based detectors were tried and REJECTED on measured data: comparing
 * STR Duration against parsed signal coverage gives p50 = 1 minute but p90 = 257
 * and p95 = 364, and 30 of 164 nights with a CORRECT index show gaps over 15
 * minutes. Signal coverage does not predict event completeness. Do not
 * reintroduce a duration tolerance here.
 */
/// The tolerance below is representation slack, not a judgement about how many
/// events may go missing. `str_index * hours` is a floating-point product of two
/// values that came from a file and a division, so a night sitting exactly on
/// the bound can miss it by an ulp. Without this, a complete night is
/// occasionally declared incomplete. That direction is the safe one -- it falls
/// back to STR's floored value rather than publishing a low index -- but being
/// wrong safely is still being wrong.
inline constexpr double kCompletenessEpsilon = 1e-9;

inline bool eventsLookComplete(int our_numerator, double str_index, double hours) {
    if (hours <= 0.0 || str_index <= 0.0) return true;  // nothing to check against
    return our_numerator >= str_index * hours - kCompletenessEpsilon;
}

}  // namespace cpapdash::parser
