# Changelog

## [Unreleased]

### Sefam: apneas, from the bit that turned out to be the apnea flag

Which DET bit means what was established from the card itself, without the
vendor's software, by two tests that need no external truth because the
definitions constrain what each flag can look like: run-length shape (an apnea
is by definition at least ten seconds) and airflow collapse (during a real one
the flow stops).

Bit 2 is the apnea flag. Its long runs sit at a flow ratio of **0.06** -- a 94%
collapse -- and no other bit is close; the next lowest that occurs often is
0.74. It is also the only bit that had correlated with apneas derived
independently from the flow waveform, found earlier by an unrelated method.

Two results worth keeping from the same table. Bits 5 and 6 fire ~841 times an
hour, which is 14 a minute, the respiratory rate our own breath detector finds
independently. And bit 4 is the trap: event-shaped in duration, no airflow drop
at all -- judging by shape alone would have called it an apnea.

Over the donor card: **94 apneas across 241 sessions and 582.3 hours**, which
the parser and the standalone analysis agree on to the event.

**This is an APNEA INDEX, not an AHI.** No hypopneas are detected: bit 0 is the
only candidate and its 0.74 ratio does not meet the <= 0.70 the definition
requires. `calculateMetrics()` still puts the number in `ahi` because that is
the only field there is, and the web front end grades `ahi` against the AASM
severity thresholds of 5/15/30 -- so a Sefam user currently gets an AHI-shaped
badge computed from half the measurement. A consumer needs to be able to tell
the two apart before this is shown as an AHI.

## [2026.8.0] - 2026-09-05

### Sefam S.Box, rewritten against two real cards — still BETA

A donor card arrived hours after 2026.7.0 shipped: an S.Box AUTO, 242 sessions,
ten months. It falsified most of what that release had reasoned out. Inside the
vendor application the reporter sent alongside it was a second card — Sefam's own
demo recording, a different device family on older firmware — which falsified two
more things and, between them, they are what this release is built on.

| | Sessions | Blocks | Failing checksums |
|---|---|---|---|
| S.Box AUTO, 1263R | 242 → **241 read** | 838,440 | **0** |
| SleepBox_AUTO, 1200R | 34 → **33 read** | 259,688 | **0** |

Both refusals are sessions in which every channel is a stub. Before this release
the parser refused all 276.

### Two device families, and nothing about the framing assumed

- **A session is a folder AND a manifest name.** The 1263R writes one session per
  `DATA_<n>` folder; the 1200R writes `<YYMMDD>/<HHMMSS>.ini`, so one folder holds
  every recording that started that day. `listSessionStems()` and
  `parseSessionNamed()` handle both; `parseSession()` delegates when a folder
  holds exactly one.
- **The block framing is derived, not known.** Both families use ten-second
  blocks, but the 1263R writes a three-byte trailer (checksum + block index) and
  the 1200R writes **one** (checksum only, no index). `deduceBlockLayout()`
  sweeps the candidates and keeps the one under which every checksum verifies.
  Worth recording that file sizes alone cannot settle this — `30s/3B` and
  `10s/1B` produce identical sizes, and reading the sizes is exactly how this
  first went wrong. Only the checksums separate them.

The 1200R demo is also the only Sefam recording we hold with the oximetry, effort
and position channels populated; the donor card has all of them as stubs.

- **The header is a fixed 38 bytes**, ASCII under XOR `0xBF`, reading
  `#02/<model><serial>      /<YYMMDDhhmmss>/`. Its identity and timestamp match
  the INI in all 242 sessions, so it is now a cross-check that an INI and a pile
  of channel files belong together — a mismatch is refused.
- **Samples are framed in ten-second blocks**, each followed by a checksum and a
  big-endian block index. This was not in any prior description of the format.
  The checksum is what the parser refuses on, and it is a far stronger guarantee
  than 2026.7.0 had: a misread offset or rate cannot pass quietly.
- **There is no scrambling.** The samples are plain. What looked scrambled is the
  38-byte header, repeated in every file including the stubs.
- **The INI's Min/Max do not scale a channel.** Pressure and leak read as tenths
  of their unit; flow is centred on mid-scale. All of it is evidenced, none of it
  is confirmed, and `docs/SEFAM_FORMAT.md` says which is which per row.
- **`DET` is a bitfield, not an enumeration**, so this release emits **no events
  at all** and reports each bit's share of the recording instead. 2026.7.0 turned
  runs of the whole byte into `OTHER` events; on this card that would have been
  tens of thousands of them a night, out of flags toggling against each other.
  A Sefam session carries no AHI.

**API changed.** `SefamDescrambler`, `deduceHeaderLength`, `deduceDescrambler`,
`decodeChannel`, `decodeEvents` and `SefamChannel::toPhysical` are gone; they
solved a format that does not exist. `decodeFileHeader`, `deframe`,
`deduceBlockLayout`, `scaleFor`, `readChannel`, `listSessionStems` and
`parseSessionNamed` replace them. Only hms-cpap consumes the Sefam parser, and it
is being updated in step.

`tools/sefam_probe` (behind `CPAPDASH_PARSER_BUILD_TOOLS=ON`) reads a card and
reports what the parser made of it, which is how both cards above were checked.

**Still open, and it needs the vendor application.** The reporter sent SEFAM
Analyze alongside the card. Its session export is the only thing that can settle
the physical scale of each channel and the meaning of the `DET` bits.

## [2026.7.0] - 2026-09-05

### Sefam S.Box support — BETA, UNVALIDATED

Released so it can be consumed, **not** because it has been proven. Nothing here
has met a real Sefam card; the fixtures behind it are synthetic. The donor-card
gate in SDD-005 section 8 is still open and still governs whether any of this can
be called correct.

Off by default. A consumer gets it only by asking for
`CPAPDASH_PARSER_WITH_SEFAM=ON`.

- **`SefamParser`, behind `CPAPDASH_PARSER_WITH_SEFAM`.** Raised by
  hms-homelab/hms-cpap#28. Reads a `DATA_<n>` session folder: flow, pressure and
  leak into the per-minute rows, SpO2 and heart rate into vitals, leak also into
  `native_samples`, and detections into events.
- **`DeviceManufacturer::SEFAM`**, and detection on the `DATA_<n>/DATA_<n>.INI`
  pair. A Sefam session sits two levels down from the card root, so unlike every
  other brand here detection walks rather than looking in one directory.
- `docs/SEFAM_FORMAT.md` records what is actually known, per fact, with a status.
  At the time of writing almost all of it is `HYPOTHESIS`.

### Why it is built the way it is
The card describes itself: every session folder carries an INI declaring each
channel's name, unit, rate, bit depth and range. So the parser hard-codes no
channel table, and it hard-codes neither of the two things the files do not state
outright. The header length and the byte scrambling are both **derived at parse
time**, and where they cannot be derived the session is refused rather than read
with a guessed offset or a guessed key.

That is partly independence. The only two write-ups of this format are GPL-3.0
and unlicensed respectively, and this library is MIT, so neither may be
transcribed. It is also robustness: a firmware that changes either one still
parses.

### What it deliberately does not do
Every detection comes out as `EventType::OTHER` carrying its raw code. What a
Sefam detection code means is not known, and a guess would put an invented number
into somebody's AHI. Under SDD-004 that keeps them inside `total_events` and
inside no clinical index, so a Sefam session reports an AHI of 0 next to its
recorded events. The map gets filled in against a donor card and the matching
SEFAM Analyze report, and not before.

## [2026.6.0] - 2026-09-05

### Added
- **`EventIndexes.h`: the seven respiratory indexes, defined once.** AHI, AI, HI,
  OAI, CAI, UAI and RIN, plus the event-type mapping that feeds them. Pure
  arithmetic, no I/O. It lives here for the same reason `SleepIndex.h` does:
  hms-cpapdash-api, cpapdash-ingest and cpapdash-app would otherwise each own a
  copy of the rule and there would be nothing keeping them equal.
- **`tests/fixtures/event_indexes/index_vectors.csv`**, the cross-language
  contract, and `test_event_indexes.cpp` holding this side to it. The Dart copy
  in cpapdash-app is held to the same table. Change the mapping here and the
  parser's tests fail first, then every consumer's.

### Why
STR.edf carries the machine's own indexes, but every one of those channels is a
0..2400 integer scaled by 0.1, so the file holds ONE decimal and ResMed FLOORS
into it. Measured on a real card: 14 of 15 days match `floor()`, none match
`round()`. A true AHI of 0.49 is stored as 0.4, so a customer shown two decimals
is shown a fabricated zero. The indexes have to be computed, and computed the
same way everywhere.

### Notes for anyone transcribing this into SQL
- **RIN keys on `details = 'Arousal'`, not `event_type = 'RERA'`.** Measured on
  the live table: of 4073 RERA rows, 3557 are arousals and 516 are the pre-OTHER
  catch-all, so keying on the type inflates RIN by 12.7% across stored history.
  There are no case or whitespace variants, so an exact match is correct and a
  defensive `lower()`/`trim()` would be worse than useless.
- **Excluded events reach no index.** 2856 of 35,040 live rows are vibratory
  snore, flow limitation, large leak or CSR, plus 4073 RERA that belong only to
  RIN. A numerator written as `count(*)` runs about 25% high. The fixture has a
  paired row that fails exactly this mistake.
- **Divide by `60.0`, not `60`.** `duration_minutes` is an INT, so integer
  division turns 7.9 hours into 7 and the index comes out ~13% high.

## [2026.5.0] - 2026-09-05

### Added
- **`EventType::OTHER`, so an unrecognised annotation stops becoming a RERA.**
  The classification chain in `EDFParser_EVE.cpp` ended in a catch-all `else`
  that assigned `RERA` to every label it did not match. Any annotation a future
  ResMed firmware introduces would silently have become a clinical RERA in a
  patient's event counts. `OTHER` is recorded and stays inside `total_events`,
  but belongs to no index, so no displayed count drops at this release. The
  enum value is appended, so existing integer values are unchanged.
- `'arousal'` now has its own explicit `RERA` branch instead of relying on the
  catch-all, so the mapping is stated rather than incidental.
- `SessionMetrics.unclassified_apneas` and `SessionMetrics.other_events`.
  `unclassified_apneas` is ResMed's bare `'Apnea'` label, which the AHI
  numerator already counted through an unpersisted local and which no consumer
  could reconstruct. It reconciles 175/175 against STR's own UAI channel.
- `docs/RESMED_CALCULATION_RULES.md`, and `tools/` holding the four scripts
  that produced every measurement in it. Which ResMed number comes from where,
  and why: the STR.edf index channels are 0..2400 scaled by 0.1, so they carry
  one decimal and the machine floors into them. Leak and pressure at 0.02 are
  not coarse, and STR wins those on definition rather than precision.

### Notes
- No format change and no behaviour change to any existing index. `total_events`
  is unchanged for identical input, which is covered by a test.

## [2026.4.2] - 2026-08-29

### Fixed
- **Inspiratory and expiratory time were missing from 57% of every night.**
  The breath detector seeded its zero-crossing list with sample 0 unconditionally
  and then walked crossings in triples from there, which is only correct when the
  flow series opens in inspiration. A series opening mid-expiration had every
  triple shifted by half a breath, Ti and Te came out swapped, and the
  tidal-volume sanity filter then discarded most of the mispaired cycles, so the
  failure was silent: the file yielded almost no breaths rather than visibly
  wrong ones.

  Per-minute summaries made this constant rather than occasional, because they
  re-ran detection on a 1500-sample slice starting at an arbitrary phase, so
  roughly half of all minutes tripped it. `Ti`, `Te` and the I:E ratio are the
  only per-minute fields not overwritten by PLD's machine-reported values, which
  is why nothing else on the page ever looked wrong.

  Measured over 175 real nights: minutes carrying a `Ti` went from 20,992 to
  49,261 out of 49,271, with the mean essentially unchanged (1.5961 s to
  1.5922 s). Whole-card breath count rose 1.83%, concentrated in five nights
  whose files opened in expiration, one of which had been producing zero breaths
  and now produces 5,884.

### Changed
- Breath detection runs **once over the whole flow series** and is bucketed by
  the minute each breath's onset falls in, instead of a second full pass per
  minute. A breath straddling a minute boundary is now counted exactly once
  instead of being truncated and filtered out of both minutes. The breath
  analysis block moved to the public section of `EDFParser.h` so it can be tested
  as a unit; `tests/test_breath_analysis.cpp` covers both defects.

  No model change: `Breath` and `BreathingSummary` are unchanged, so consumers
  need no edit to compile.

See `sdd/003-the-breath-detector.md`, which also records a shape-based flattening
index that was built and then **rejected**: validated against ResMed's own
FlowLim channel over 49,256 paired minutes it scored Spearman -0.003, because
ResMed blends a flatness index with a breath shape index, ventilation change and
duty cycle, and a flatness scalar alone measures almost none of it.

## [2026.4.1] - 2026-08-26

### Fixed
- **The sleep index fixtures are found wherever the tests are run from.**
  v2026.4.0's two contract tests read `tests/fixtures/sleep_index/*.csv` by a
  path relative to the repository root. `ctest` runs the binary from the BUILD
  directory, so both tests failed in CI on a tag whose library code was
  perfectly fine. They passed locally only because a hand-run `./run_tests`
  usually starts at the root.

  CMake now passes the source directory in as `CPAPDASH_PARSER_SOURCE_DIR` and
  the lookup falls back through the working directory and its parents, so the
  tables are found from the build tree, from the root, and from anywhere else.
  Verified all three ways, including `ctest` from the build directory, which is
  the shape CI actually runs.

  **v2026.4.0 should not be used.** Its library is identical to this one, but
  its test suite fails under ctest and it therefore has no release artifacts.

## [2026.4.0] - 2026-08-26

### Added
- **`SleepIndex`**, the nightly therapy-quality index and the consistency
  streak, moved here from cpapdash-app. It was defined once, in Dart, in
  `lib/services/sleep_score.dart`; hms-cpapdash-api never had it and hms-cpap
  has nothing score-shaped at all. Writing it again in each consumer would make
  three copies of a set of tunable constants in three languages with nothing
  keeping them equal. See hms-cpap `docs/SDD-019`.

  `nightlyIndex()` weights usage 40, AHI 40 and leak 20, renormalising over
  whichever inputs are actually present, so a machine that reports no leak
  scores out of 80 rather than being punished for its silence. A negative leak
  is a sentinel and is dropped rather than clamped to full credit.
  `currentStreak()` / `bestStreak()` count compliant nights over YYYYMMDD dates
  with real calendar arithmetic, so 31 April is rejected instead of quietly
  becoming 1 May.

  `tests/fixtures/sleep_index/*.csv` are the contract, not just a test: the
  Dart implementation reads the same two tables, and a change to any weight or
  cutoff here fails the app's tests until it follows. CSV rather than JSON
  because this library links no JSON reader and a five column table is not
  worth adding one.

## [2026.3.0] - 2026-08-25

### Fixed
- **Session duration is therapy time, not the envelope around it.**
  `parseBRPFile` set `duration_seconds` to `session_end - session_start`. A
  night is not one file: ResMed opens a fresh checkpoint after every mask-off
  break, so that span counted every break as therapy.

  Support ticket 87, verified against the customer's own card. Three
  checkpoints holding 1, 114 and 107 minutes, separated by a 7h evening break
  and a 31m gap, reported 11h 22m where OSCAR reports 3h 42m. One error, two
  wrong numbers, because AHI is events over duration: 2 obstructive apneas came
  out as 0.18/h instead of 0.54/h. Both of the customer's figures are
  reproduced exactly by the two formulas, which is what identified the line.

  `duration_seconds` is now the sum of what the checkpoints actually hold,
  keyed on `file_start` in a new `ParsedSession::brp_spans` rather than
  accumulated. The same checkpoint is legitimately parsed more than once: a
  merge re-parses earlier files, and a live file grows between reads. Assigning
  by key makes a re-parse idempotent and lets a grown file replace its own
  smaller value, where a running total would double-count both and a live
  session polled every minute would inflate without bound.

  `session_end` is untouched and still wall clock. The charts need the real end
  of the night, and the break has to stay visible as a break.

## [2026.2.0] - 2026-08-23

### Changed
- **A minor rather than a patch, because the oximetry path is now whole.** The
  O2 Ring CSV reader landed alongside the writer and the `.vld` parser, so this
  library covers all of oximetry rather than half of it, and both hms-cpap and
  hms-cpapdash-api can delete their own copies. Also corrects
  `project(VERSION)`, which had been left behind while the tags moved on, a
  mismatch nobody reads until something links against the wrong thing.

## [2026.1.16] - 2026-08-23

### Added
- **`readO2RingCsv`, one reader instead of one per consumer.** hms-cpap and
  hms-cpapdash-api had each grown their own reader for the Wellue/Viatom
  export, and the two drifted exactly where a duplicated format definition
  always drifts: in the corners one consumer hit and the other had not yet. The
  API had learned to resolve an ambiguous numeric date from the night the
  client filed it under (ticket 84) and to strip the comma out of
  "Jun 19, 2026" before sniffing the date order; hms-cpap had learned to
  summarise through `calculateMetrics` rather than a hand-rolled proxy. Neither
  knew what the other knew, and a user with the wrong one got their night filed
  on the wrong day. This is the union.

  Numeric dates are the hard part: the phone app writes the date in the
  PHONE's locale, so "05/08/2026" is the 5th of August or the 8th of May and
  nothing in the file says which. Resolved in order by a component above 12
  (only a day can be), the caller's day hint, consecutive-day progression
  across the file, the YYYYMMDD stamp in the export's own filename, an AM/PM
  clock implying a US locale, then day-first.

  Stays timezone-free, which is the constraint that makes the file safe to
  share between consumers. A ring records a wall clock with no zone, so
  timestamps go in through `timegm` and come out through `gmtime`, the same
  clock `writeO2RingCsv` renders, which is what makes read-then-write an exact
  round trip. Whose evening that wall clock was is the caller's question.

## [2026.1.15] - 2026-08-23

### Fixed
- **A ResMed STR's mode enum means different things on different machines**,
  and one enum was being applied to all of them. A bi-level reporting mode 8
  was read through the ASV enum, so its owner was told he is on ASVAuto, a
  treatment for central apnea he is not on.

  The model code cannot separate them either: an AirSense (AutoSet) and an
  AirCurve (bi-level) both report MID=46, verified across six real devices. The
  signal set is unambiguous and self-describing, so it is now the
  discriminator: `S.AV.*`/`S.AA.*` is ASV, `S.VA.*`/`S.S.*` is bi-level,
  `S.AS.*` is AutoSet, `S.C.*` alone is CPAP. Read the family first, then
  interpret the mode within it.

  `mode` itself is left exactly as the machine reported it. Rewriting the
  number would silently change what every already-stored row means, so
  consumers read it through the family instead.

### Added
- **The bi-level settings that were being dropped:** `S.VA.MaxIPAP`,
  `S.VA.MinEPAP`, `S.VA.PS`, `S.S.IPAP` and `S.S.EPAP`. Deliberately not gated
  on a mode number, since trusting `mode` to decide which settings exist was
  the whole defect. A machine without them simply has no such signals.

  Proven on real files: the affected customer's STR classifies BiLevel with
  MaxIPAP 14 / MinEPAP 5 / PS 3 / IPAP 8 / EPAP 5, corroborated by his measured
  mask pressure sitting at p50 6.40 / p95 7.20, between EPAP and IPAP as a
  bi-level should. An AutoSet machine on the same MID classifies AutoSet with
  every bi-level field empty.

## [2026.1.14] - 2026-08-23

### Fixed
- **`.vld` duration and sample interval are read from the header, not derived
  by division.** `parse()` read a u16 at offset 18 as the recording duration
  and computed the interval as duration over record count. Offset 18 is not the
  duration.

  Found on the first real Wellue export we have ever had: 7591 records, a true
  4s interval and a true 30364s duration, but offset 18 held 3673, so the
  interval came out as 0.4839s. Nothing samples at half a second. The recording
  was reported as 1.02h instead of 8.43h and ODI as 43.1/h instead of 9.13/h,
  which is severe instead of mild. What caught it was the CPAP session recorded
  alongside that night: 8.20 therapy hours.

  The real layout, recovered by scanning the header for arithmetic identities:
  offset 9 is a u32 file size, offset 13 a u32 duration in seconds, offset 22 a
  u16 sample interval in seconds, and offset 18 is what we were reading.
  7591 records at 4s is 30364s, and offset 13 holds exactly that.

  Everything that scales with the interval had inherited the error, including
  the 120s smoothing window `calculateMetrics` sizes as `120.0 / interval`, so
  desaturation detection was mis-windowed too. That is why the corrected ODI is
  9.13 rather than the 5.2 a naive rescale of the old event count suggests.

  An interval outside 1 to 10s is now refused rather than propagating a
  nonsense rate, and a duration that disagrees with the samples by more than 5%
  is corrected to record count times interval. The samples are what gets
  plotted; a duration that contradicts them silently distorts every rate
  computed against it.

## [2026.1.13] - 2026-08-14

### Added
- **`writeO2RingCsv` / `o2RingCsvFilename`**, the Wellue/Viatom CSV writer
  SleepHQ accepts for oximetry. It lives here because hms-cpap and
  hms-cpapdash-api both have to produce this exact file, and two copies of a
  format definition drift. The readers stay per-consumer, since each has its
  own upload path.

  Real exports come in two dialects; this writes the unquoted 24-hour one and
  only that. Timestamps render as UTC because that is the clock the readers
  parse them in. Unreadable samples are written as the 255 sentinel rather than
  skipped, so a stretch with the ring off the finger stays a gap instead of
  quietly shortening the night.

## [2026.1.12] - 2026-08-14

### Added
- **`parseSessionFromBuffers` takes every file a session has**, not one per
  type. A caller that reads files itself now hands over `SessionBuffers` with a
  vector per type, matching what the directory form has always collected.

  hms-cpapdash-api kept only the LARGEST file of each type, so a night lost
  every BRP checkpoint but one and every EVE but one. The previous fix in
  2026.1.11 did not reach it, because that fixed the DIRECTORY form and the
  cloud never calls it. The old signatures remain and delegate.

### Fixed
- **Header-only files are skipped rather than parsed.** ResMed leaves these
  behind routinely -- a card night can hold six BRP files with four of them
  header-only. Such a file contributes no samples but does contribute its
  timestamp, which is how a night once had 70 minutes of genuine flow anchored
  to an empty checkpoint and came out early and short. Now that callers are
  asked to hand over everything they found, the parser refuses the empties
  itself rather than trusting each caller to filter them. An EVE with no
  records also no longer counts as "has events": that is the 832-byte stub a
  mask-fit check leaves behind.

## [2026.1.11] - 2026-08-14

### Fixed
- **A session keeps every EVE and CSL it has, not just one.** A ResMed night is
  several mask-on blocks and each writes its own pair, but `parseSession` held a
  single `eve_file` beside its BRP/PLD/SAD vectors, so the last file
  `directory_iterator` happened to yield won and the rest were dropped.
  Iteration order is unspecified, so which one survived was not even stable
  between runs.

  The cost, from hms-cpap issue 22: a card whose first block is a seconds-long
  mask-fit check carrying the empty 832-byte EVE stub reported AHI 0.0 for the
  night, while OSCAR read 2.84 off the same bytes.

  EVE and CSL are now vectors, sorted with the same comparator the checkpoints
  use, and every EVE is parsed. The events are sorted by timestamp afterwards,
  because the concatenation is otherwise only ordered within each block.

## [2026.1.10] - 2026-08-13

### Fixed
- **Löwenstein channels are bound by exact name, from an explicit table.**
  Two Prisma firmwares in the field disagree on nearly half their channel
  names: one writes `EPAPsoll`/`IPAPsoll`/`BreathVolume`/`BreathFrequency`/
  `MV`/`InspExpirRel`, a SMART max writes `EPAP`/`IPAP`/`PressureMeasured`/
  `CPAPPressure`/`FlowFull`/`rRMV` and none of the others.

  Binding used to fall back to a substring search, which is silently dangerous
  on labels this short. A SMART max has no exact `MV`, so the minute-ventilation
  read matched **`rRMV`** — a 0-255 relative percentage — and it would have been
  served as minute ventilation in L/min, on 808 of 814 minutes of a real night.
  A wrong therapy number is worse than a missing one, so matching is now exact
  and confined to the table; an unrecognised spelling yields nothing, and a new
  firmware is added deliberately.

### Added
- **`PressureMeasured` and the unsuffixed `EPAP`/`IPAP` are now read.** The
  first is the pressure at the mask on a SMART max — the `avg_mask_pressure`
  that hms-cpap issue 15 reports as always zero — and reading it also unblocks
  the per-minute row its consumers gate on. The second is why EPR came out
  empty on exactly the machine the issue was filed about.

  What these machines do NOT record, and this release does not invent: a Prisma
  declares `SpO2`, `HeartFrequency`, `BreathVolume`, `BreathFrequency`,
  `InspExpirRel` and `MV` and writes zero to every sample, and a SMART max does
  not declare them at all. Neither has a snore channel.

## [2026.1.9] - 2026-08-12

### Fixed
- **A PLD minute merges onto the nearest flow minute within its tolerance, not
  the first one it finds inside it.** The match took the first row within 30
  seconds and stopped looking; where more than one row sits inside that window
  the first is not necessarily the closest, so the wrong minute could be claimed
  while the one that should have carried those values kept none.

  Honest about the scope: this is a correctness fix, not a fix for the reported
  chart gaps. It was written while chasing those gaps and does not explain them.
  On the night investigated the flow checkpoints were hours apart, so only one
  row was ever in range and the old and new matchers behave identically —
  confirmed by reverting the change and watching the test still pass. Those gaps
  line up with checkpoints the machine never wrote a PLD for at all (five flow
  files, three PLD), which no matching strategy can invent, and which the
  nightly summary cannot fill either since it carries one row per night rather
  than one per minute.

## [2026.1.8] - 2026-08-12

### Fixed
- **Every BRP checkpoint is placed on the clock by its own start, so a night
  with a break in it finally looks like one.** A night is not one file: the
  machine opens a fresh checkpoint every few minutes and again after any
  mask-off break, so a session routinely arrives as several files separated by
  real gaps. Each of them was being anchored to the session's start, which
  stacked them all from the same instant — the break vanished because every
  segment restarted at the beginning, and the series finished early by the total
  of the gaps it had swallowed.

  That is hms-cpap issue #21: a 2:20am bathroom break that never appeared, and a
  flow chart that stopped around 4am while the summary metrics, which come from
  STR, covered the whole night. Reproduced on a real card whose night carried six
  checkpoints, four of them header-only: 70 minutes of genuine flow were written
  at the *empty* first checkpoint's timestamp, 2m21s early and a minute short.
  They now land on the file's own 20:15:36 → 21:24:36.

  The name is trusted ahead of the header, because an AirSense 10 was observed
  writing header date 29.06.26 into `20260706_195339_BRP.edf`, a full week
  behind. Buffer-mode reads have no name and keep using the header.

- **The end of a session is the latest checkpoint's end**, rather than whichever
  file happened to be parsed last, so re-reading an earlier checkpoint can no
  longer pull the end backwards over a later one.

## [2026.1.7] - 2026-08-08

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

## [2026.1.2] - 2026-06-14

### Removed
- **The Philips DreamStation 2 parser.** The DS2/PRS1 implementation (crypto,
  chunk reader, F0V6 event decoder) had been adapted from a GPLv3 reference and
  is license-incompatible with this MIT library. Removed along with its tests,
  the `CPAPDASH_PARSER_WITH_PHILIPS` build flag, the OpenSSL dependency it
  required and the `PHILIPS` manufacturer enum.

  Supported hardware is ResMed (EDF) and Löwenstein Prisma (WMEDF/XML). A
  clean-room DreamStation parser may be added later behind the same flag, which
  is what SDD-002 and the private hms-cpapdash-parser-philips fork exist for.

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
