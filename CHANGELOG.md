# Changelog

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
