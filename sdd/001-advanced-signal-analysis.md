# SDD-001: Advanced Signal Analysis — Desaturation Events, Breath-by-Breath, Event Durations (Parser)

**Status:** Draft
**Date:** 2026-06-14
**Consumers:** `hms-cpap` and the CpapDash apps (a Dart port mirrors this model)
**Companion:** `hms-cpap/docs/SDD-001-advanced-signal-charts.md`

## Problem

A user asked for advanced clinical charts: apnea-event overlays on the flow trace, SpO2 desaturation plots, and breath-level detail (Flow Limitation, Snore, breath-by-breath). Auditing the stack against established CPAP tools surfaced three gaps. All three are fundamentally **data-layer** gaps, and this library is the single source of truth that every downstream consumer parses with. Fixing them here means each consumer inherits the capability instead of divergent re-implementations.

Current state in this library:

1. **Event durations exist but are under-used.** `SleepEvent` already carries `event_type`, an absolute `timestamp`, and `duration_seconds` (`EDFParser_EVE.cpp:48-52`). Consumers currently render events as zero-width markers; the duration needed to draw a *span* over the flow trace is already produced — no parser change required for F1 beyond a documented guarantee.
2. **Desaturation detection only exists for the O2Ring/VLD path.** `VLDParser.cpp:165-192` computes ODI-3% with a rolling 120 s baseline and populates `desat_count_3pct`/`odi_3pct`. The machine-SpO2 path (SAD.edf → `vitals`) has **no** desaturation detection, and no desaturation *events* are emitted anywhere. `SessionMetrics.spo2_drops` (`Models.h:170`) is declared but never populated on the SAD path.
3. **Breaths are detected, then discarded.** `EDFParser::detectBreaths()` (`EDFParser_BRP.cpp:192-307`) already segments flow by zero-crossing and computes per-breath `tidal_volume`, `inspiratory_time`, `expiratory_time`, and a `flow_limitation` score (`BreathCycle`, `EDFParser.h:91-98`). But it is called per-minute purely to derive the per-minute `BreathingSummary` (`calculateRespiratoryMetrics()`, `EDFParser_BRP.cpp:309+`) and the `BreathCycle` vector is dropped. Raw 25 Hz flow is parsed but never persisted; the finest stored resolution is the per-minute `BreathingSummary`.

## Design

Three additive changes to the unified output model in `Models.h`. All are opt-in / additive so existing consumers keep compiling.

### F1 — Event durations (documentation + guarantee, no code change)

No structural change. We **document and test** the invariant that `SleepEvent.duration_seconds` is the machine-reported event length in seconds (0 when the manufacturer omits it), and that `timestamp` is the event *onset* (already true: `EDFParser_EVE.cpp:48-52` uses `start_time + onset_sec`). Consumers compute the span as `[timestamp, timestamp + duration_seconds]`.

> Note on convention: some loaders store `time(i)` as the event *end* and `data(i)` as duration. We deliberately use `timestamp` as the **onset**. Consumers must not copy a "subtract duration" convention.

### F2 — Desaturation events from machine SpO2

Promote the VLD desat algorithm into a shared, path-agnostic detector and run it on the SAD/machine-SpO2 vitals too, emitting first-class events.

New free function (new file `src/DesatDetector.cpp`, header `include/cpapdash/parser/DesatDetector.h`):

```cpp
namespace cpapdash::parser {

struct DesatParams {
    double drop_pct      = 3.0;    // % below rolling baseline to open an event
    double recover_pct   = 1.0;    // % below baseline to close an event
    double baseline_secs = 120.0;  // rolling baseline window (max SpO2 in window)
    double min_secs      = 8.0;    // minimum sustained duration to record (8s is a common clinical minimum)
};

struct DesatEvent {
    std::chrono::system_clock::time_point onset;
    double duration_seconds;
    double nadir;        // lowest SpO2 reached
    double depth;        // baseline - nadir (%)
};

// Detect desaturations from an evenly-or-unevenly-sampled SpO2 series.
std::vector<DesatEvent> detectDesaturations(
    const std::vector<VitalSample>& samples,   // must be time-ordered; uses .spo2 + timestamp
    const DesatParams& params = {});

}
```

Algorithm (generalized from `VLDParser.cpp:169-189`, with two corrections — a real time base instead of fixed `sample_interval`, and a `min_secs` floor + `nadir/depth` capture):

```
for each valid sample i:
    baseline = max spo2 over [t_i - baseline_secs, t_i)
    drop = baseline - spo2_i
    if drop >= drop_pct and not in_event:
        open event at onset=t_i, nadir=spo2_i, baseline_at_open=baseline
    if in_event:
        nadir = min(nadir, spo2_i)
        if drop < recover_pct:           # recovered
            dur = t_i - onset
            if dur >= min_secs:
                emit DesatEvent{onset, dur, nadir, baseline_at_open - nadir}
            in_event = false
```

Wiring:
- `EDFParser_SAD.cpp` (or the post-parse assembly in `EDFParser.cpp`) calls `detectDesaturations(session.vitals)` after vitals are populated.
- Emitted as `SleepEvent`s with a new `EventType::DESATURATION` (add to enum, `Models.h:14-37`; `eventTypeToString` → `"Desaturation"`, `Models.cpp:19-20`). They live in `session.events` alongside respiratory events so every consumer's existing events pipeline carries them for free.
- Populate `SessionMetrics.spo2_drops` = count, and add `SessionMetrics.odi` (events/hour) next to the SpO2 block (`Models.h:164-170`).
- The VLD path is refactored to call the same `detectDesaturations()` (dedupe), preserving `desat_count_3pct`/`odi_3pct`.

### F3 — Persist breath-by-breath

Surface the already-computed `BreathCycle`s instead of discarding them.

Add to `ParsedSession` (`Models.h:211-253`):

```cpp
struct Breath {
    std::chrono::system_clock::time_point onset;   // absolute time of inspiration start
    double tidal_volume;      // mL
    double inspiratory_time;  // s
    double expiratory_time;   // s
    double flow_limitation;   // 0..1
};
std::vector<Breath> breaths;   // empty unless raw flow was available (BRP / Prisma)
```

`detectBreaths()` returns sample-index-based `BreathCycle`s per minute. We map indices → absolute time using the BRP record start + `sample_rate`, and append to `session.breaths`. This is done once over the full flow series (not per-minute) to avoid double-counting breaths straddling a minute boundary.

Storage cost is modest — a night is ~7–9k breaths (≈4 doubles + a timestamp each), vs. ~28k raw 25 Hz samples/minute we deliberately still **do not** persist. Consumers that want a flow trace under a breath continue to use the per-minute `BreathingSummary`; breath rendering uses `breaths`.

`breaths` is left empty for summary-only sources (STR-only sessions) so the field is always safe to read.

### Public API / ABI

- New enum value, two new structs, two new vector fields, one new free function. All additive. No existing signature changes. Bump `CHANGELOG.md` minor version.

## Tests (GTest, `tests/`)

- `test_DesatDetector.cpp` — synthetic SpO2 series: (a) single 4% drop sustained 10 s → 1 event, depth≈4, nadir correct; (b) 2 s blip → 0 events (below `min_secs`); (c) gradual decline tracking baseline → 0 events; (d) back-to-back drops → 2 events with correct recovery gating; (e) empty/all-invalid → 0.
- `test_breaths_persisted` (extend `test_edf_file.cpp`) — parse a real BRP fixture: `session.breaths` non-empty, count ≈ `avg_respiratory_rate × minutes` within tolerance; each breath `tidal_volume` in [50,3000]; onsets monotonic and within session bounds.
- `test_event_duration_invariant` (extend `test_edf_file.cpp`) — EVE fixture: every `SleepEvent.duration_seconds >= 0`; `timestamp` is onset (≥ session start, + duration ≤ session end).
- `test_metrics_odi` — `SessionMetrics.spo2_drops`/`odi` populated on a SAD fixture; VLD path parity unchanged.

## Open questions

- Default `drop_pct`: 3% (AASM 2012 default) vs 4% (CMS). Shipping 3% to match the existing VLD behavior; expose `DesatParams` so a consumer can switch.
- Should `flow_limitation` per breath be smoothed (some tools apply a moving average)? Deferred — ship raw per-breath value, smooth in the UI if noisy.

## Migration

1. Add `EventType::DESATURATION` + `Breath`/`DesatEvent` structs + `breaths`/`odi` fields (additive).
2. Add `DesatDetector.{h,cpp}` + CMake entry; refactor VLD to use it.
3. Append breaths in the BRP assembly path; mirror for Prisma where raw flow exists.
4. Tests green (`ctest`), tag a minor release. Consumers pick it up via FetchContent on next build.
