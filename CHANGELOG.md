# Changelog

## [Unreleased]

### Fixed
- **Therapy pressure is read at last, so pressure now agrees with OSCAR and
  SleepHQ.** A ResMed PLD carries three pressure channels: `MaskPress.2s`
  measured at the mask, `Press.2s` the pressure the machine actually delivers,
  and `EprPress.2s` the expiratory set-point exactly one EPR level below
  `Press.2s`. Only the first two of those were ever read, and mask pressure was
  what got reported as a night's pressure. On a real AutoSet night that put the
  median at 9.10 cmH2O where OSCAR and SleepHQ show 9.88, and the 95th at 9.93
  against their 10.72: a consistent 0.8 cmH2O low read, about 8% of the therapy.

  `Press.2s` now lands in `BreathingSummary::therapy_pressure` and aggregates
  into `avg/min/max_therapy_pressure` and `therapy_pressure_p95/p50`. Mask
  pressure is untouched and stays its own signal.

- **A percentile of per-minute means is no longer published as a percentile of
  the signal.** `breathing_summary` is one row per minute, so a 95th taken over
  it averaged away exactly the spikes it was meant to describe. Leak's 95th came
  out 8.68 L/min where the machine's own STR summary says 8.4, and a real 87.6
  L/min blow-out inside one minute of otherwise-zero leak was filed as 4.8,
  leaving the night's peak reading 10.8 instead of 87.6.

  The PLD parser now keeps the samples at the machine's own 0.5 Hz in
  `ParsedSession::native_samples`, and the published mean, median, 95th and max
  are computed from those. Samples rather than precomputed statistics because a
  night is often several recordings merged into one session, and percentiles do
  not merge. Leak also carries its within-minute `leak_min`/`leak_max` now, so a
  peak survives in stored per-minute data. Parsers that keep no native samples
  (Prisma, Philips) fall straight through and their numbers are unchanged.

### Added
- **`EDFFile::findSignalPrefix()`**, matching a signal label by prefix rather
  than substring. `findSignal()` returns the first substring hit in signal
  order, which cannot distinguish a label that is a suffix of another one on the
  same file: `"MaskPress.2s"` contains `"Press"`, so `findSignal("Press")`
  silently hands back mask pressure and looks like it worked. That is the exact
  mismatch above. The PLD parser matches therapy pressure by prefix with no
  substring fallback, since a fallback there would reintroduce the bug; the BRP
  parser prefers the prefix and keeps the old substring match as a fallback, so
  a device labelling its BRP pressure differently does not lose the channel.
  Two tests pin the distinction, one of which documents the trap directly.

## [2026.1.6] - 2026-07-20

### Fixed
- **`project(VERSION)` now tracks the release version.** It had been stale at
  `1.0.0` since the repo was created, through every `2026.1.x` tag, so
  `cpapdash_parser_VERSION` reported `1.0.0` to every CMake consumer. Consumers
  pin this repo by git tag via `FetchContent` (`hms-cpap` uses
  `GIT_TAG v2026.1.3` when no local sibling checkout is present), so the
  declared version and the tag need to agree. No code change.

## [2026.1.5] - 2026-07-20

### Fixed
- **Malformed numeric fields no longer throw out of the parsers.** Every integer
  field the parsers read comes from a file an uploader supplies, and `std::stoi`
  throws on non-numeric input and on overflow. cpapdash-api runs these parsers on
  a detached thread, where an escaping exception calls `std::terminate` — so one
  malformed upload could take down the entire API. Three reachable throws, each
  reproduced before fixing: `PrismaParser::parseDeviceXml` on `<DeviceType>` /
  `<MainboardHWVersion>` from an uploaded `device.xml` (runs *before* the WMEDF
  parse, so the `.wmedf` need not even be valid); `EDFParser::parseDeviceInfo` on
  overlong `MID=` / `VID=` digit runs; and the `YYYYMMDD_HHMMSS` session-start
  parse, whose only guard was a length check that says nothing about the
  characters being digits. New internal `src/ParseNum.h` provides `parseIntOr` —
  exactly `std::stoi`'s conversion minus the throw, deliberately keeping stoi's
  leading-prefix behaviour (`"27abc"` → 27, `"5.05"` → 5), so any value that
  parses today parses to the same number. Behaviour changes only for input that
  currently crashes. An unusable session-start now drops the timestamp rather
  than recording a garbage date; the BRP header's own start date supplies it
  instead. Latent hardening — not observed in production.

## [2026.1.4] - 2026-07-02

### Added
- **`detectManufacturer()`** (SDD-022 slice 3), broad, detect-only-capable brand
  identification, separate from parse support. Extended `DeviceManufacturer` with
  `PHILIPS` and `BMC` (React Health/3B Luna share the BMC on-card format); both are
  detected but have no parser (`createParser()` still returns `nullptr` for them,
  same as before: detection is broader than parsing). Ordered, unambiguous-first
  signature match: Löwenstein (`.wmedf`), BMC (serial-named `\d\dC\d{5}.usr`),
  Philips (`Properties.txt` identity manifest), ResMed (`STR.edf`/`DATALOG`/`.edf`),
  else `UNKNOWN`. Two overloads: `detectManufacturer(filenames)` (pure, no disk
  I/O) and `detectManufacturer(data_dir)` (directory-walk). `createParser(data_dir)`
  now delegates to `detectManufacturer(data_dir)` internally, behavior-preserving
  for ResMed/Löwenstein (the existing `FactoryAutoDetectsFromDir` test still
  passes); the Löwenstein directory gate relaxed from requiring both `.wmedf` AND
  `.xml` to `.wmedf` alone (the unambiguous signal per the on-card format research;
  a real Prisma night always pairs the two anyway).

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
