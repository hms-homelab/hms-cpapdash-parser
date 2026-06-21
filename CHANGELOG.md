# Changelog

## [Unreleased]

## [2026.1.3] - 2026-06-21

### Fixed
- **Löwenstein event XML: tolerate spaced attributes.** SMART max (fw 3.17)
  writes `RespEvent` attributes spaced out (`RespEventID = "101"`) while
  `DeviceEvent` uses the tight form; the extractor now handles both, so SMART
  max respiratory events parse (was 0 events) — with a left-boundary check so an
  attr name embedded in a longer one (e.g. `Time` in `EndTime`) isn't matched.
- **AHI counts apneas + hypopneas only.** Was `total_events / hours`, which
  over-counted RERA, flow limitation, vibratory snore, and large leak — badly on
  devices that flag many flow-limitation events (e.g. SMART max). Those are still
  recorded for display; ResMed is unaffected (uses the official STR AHI).

### Added
- **DesatDetector** — SpO2 desaturation detection from a time-ordered vitals
  series (`detectDesaturations`), generalizing the O2Ring/VLD ODI rule to the
  SAD/machine-SpO2 path: rolling 120 s baseline, 3% drop, 1% recovery, 8 s
  minimum. Returns `DesatEvent{onset, duration, nadir, depth}`.
- **`ParsedSession::desaturations`** — detected desats, kept separate from
  `events` so they never inflate `total_events`/AHI.
- **`ParsedSession::breaths`** — breath-by-breath detail (`Breath{onset, tidal
  volume, Ti, Te, flow limitation}`) persisted from the existing BRP
  zero-crossing detection (previously computed per-minute then discarded).
- **`SessionMetrics::odi`** — Oxygen Desaturation Index (desats/hour).
- **`EventType::DESATURATION`** + `eventTypeToString` entry.

### Changed
- `calculateMetrics()` now populates `spo2_drops`/`odi` from the rolling-baseline
  `DesatDetector` instead of the previous naive consecutive-sample 4% diff.

## [2026.1.0] - 2026-04-02

### Added
- **EDFFile** — EDF header parser with memory-buffer support (`open(const uint8_t*, size_t)`)
  - Signal metadata extraction (label, physical dimension, min/max, samples per record)
  - Signal data reading with physical unit scaling
  - Growing file detection (actual_records > header records)
  - EDF+ format detection
  - `findSignal()` (partial match) and `findSignalExact()` (exact match)
- **EDFParser** — file-type-specific parsers:
  - **BRP** — breathing flow/pressure (25 Hz), breath detection (zero-crossing),
    respiratory metrics (RR, TV, MV, I:E ratio, flow limitation, leak)
  - **PLD** — machine metrics (0.5 Hz): mask pressure, EPR, leak, snore, target ventilation
  - **SAD** — vitals (1 Hz): SpO2 with validity filtering, heart rate
  - **EVE** — respiratory events from EDF+ TAL annotations (OA, CA, H, RERA, CSR)
  - **STR** — daily therapy summary (81 signals: AHI, pressure/leak/SpO2 percentiles, settings)
- **Models** — data structures:
  - `ParsedSession` with `calculateMetrics()` aggregation
  - `SessionMetrics` (45+ fields)
  - `BreathingSummary` (per-minute flow/pressure/respiratory stats)
  - `SleepEvent`, `VitalSample`, `STRDailyRecord`
- **Unit tests** — synthetic EDF buffer construction, header parsing, signal extraction,
  metrics calculation, desaturation detection, growing file detection
- **CMake** — static library target `cpapdash_parser`, GTest integration,
  consumable via FetchContent

### Architecture
- Namespace: `cpapdash::parser`
- Pure C++17, no external dependencies
- Memory-buffer API for cloud ingestion (no file I/O required)
- Extracted from hms-cpap EDFParser.cpp (~1600 lines)
