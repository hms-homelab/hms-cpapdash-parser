# SDD-003: The breath detector, and what its flow limitation number is worth

**Status:** D1 and D2 implemented. **D3 REJECTED on validation, see below.**
**Date:** 2026-08-29
**Consumers:** `hms-cpap`, `cpapdash-app` (Dart port), `hms-cpapdash-api`
**Companion:** `sdd/001-advanced-signal-analysis.md` (F3, which built this)
**Fork note:** `hms-cpapdash-parser-philips` hoisted this code into
`BreathAnalysis.{h,cpp}`. Whatever lands here has to be replayed there.

## Problem

`EDFParser::detectBreaths()` (`src/EDFParser_BRP.cpp:294`) segments the 25 Hz
BRP flow trace into breaths by zero-crossing and derives four numbers per
breath: tidal volume, Ti, Te, and a flow-limitation score. It has three
defects, one of which is only worth fixing once you know who actually reads
the output.

### P1. The zero-crossing pairing has a parity bug

`zero_crossings` is seeded with index 0 unconditionally, then the loop consumes
crossings three at a time (`i += 2`) reading them as `start -> mid -> end`,
that is inspiration then expiration.

That is only correct when the flow series opens in inspiration. When a file
opens mid-expiration the first real crossing is negative-to-positive, so the
pairs become `(0, first_inspiration, first_expiration)`, and the segment
labelled inspiration is the tail of an expiration. For the whole file after
that point, Ti and Te are swapped and tidal volume is integrated over the wrong
half of the breath.

Two knock-on effects, both observed:

- The tidal-volume sanity filter (50 to 3000 mL) discards most of these
  mispaired cycles, so a badly-phased file yields far fewer breaths rather than
  obviously wrong ones. It fails quietly.
- A synthetic sine starting exactly at 0.0 yields **zero** breaths. The Philips
  fork's fixture generator works around this with a +0.1 rad phase offset
  (`tests/fixtures/philips/gen/make_fixtures.py`), which is a test accommodating
  a bug.

It survives on real ResMed cards because BRP files almost always open with the
mask already on and the sleeper mid-inspiration.

### P2. The flow series is scanned twice

`EDFParser_BRP.cpp:184` runs `detectBreaths` once over the whole file to
populate `session.breaths`. `calculateRespiratoryMetrics` (`:411`) then slices
each minute out and runs `detectBreaths` **again** on that slice, per minute,
to derive `BreathingSummary`.

An eight-hour night at 25 Hz is 720k samples, so this is roughly a second full
pass plus 480 vector copies. Not a crisis, but the two passes can also disagree:
a breath straddling a minute boundary is counted whole by the file-level pass
and truncated (then usually filtered out) by the minute-level pass.

### P3. The flow-limitation score is a peak-to-mean ratio, and mostly nobody reads it

```cpp
double peak_to_mean_ratio = insp_max / insp_mean;
breath.flow_limitation = std::max(0.0, std::min(1.0,
    1.0 - (peak_to_mean_ratio - 1.0) / 2.0));
```

One scalar from the inspiratory half. It cannot distinguish the shapes that
matter clinically: a flat-topped (obstructed) breath, an M-shaped one, and a
late-peaking one can all land on the same peak-to-mean ratio, because the ratio
throws away *when* the flow was high.

**The finding that changes the scope of this.** `BreathingSummary.flow_limitation`
is written twice, and the machine wins:

| Source | Writes | Order |
|---|---|---|
| `EDFParser_BRP.cpp:475` | our peak-to-mean average over the minute | first |
| `EDFParser_PLD.cpp:182` | the machine's own `FlowLim` channel, averaged | **overwrites** |
| `PrismaParser_Signals.cpp:196` | Löwenstein's own value, `/100.0` | (Prisma path) |

`EDFParser.cpp` parses BRP at `:114`/`:250` and PLD at `:128`/`:279`, so PLD
lands second. On any ResMed session that has a PLD file, and on Prisma, the
per-minute flow limitation the dashboards render is the **machine's**, not ours.
`Models.cpp:255` averages that same field into
`SessionMetrics.avg_flow_limitation`, so the session-level number is the
machine's too.

So our heuristic actually reaches a user through exactly two paths:

1. `Breath.flow_limitation`, the per-breath value, which only the Flutter app
   renders (F3 web was deliberately not built: hms-cpap's web has per-minute
   signals, not raw flow).
2. `BreathingSummary.flow_limitation` as a **fallback**, when BRP exists but PLD
   has no `FlowLim` channel.

That is a smaller audience than "flow limitation across four repos" suggests,
and it is worth saying out loud before spending the effort. It does not make
the work pointless: the per-breath value is the whole point of the F3 chart,
and a per-breath number is something no machine channel gives us, since
`FlowLim` is already averaged to the minute. But it does mean this is an
improvement to the app's breath chart and to a fallback path, not a correction
to the headline number on the dashboards.

## Design

### D1. Seed the crossing list from the actual first phase

Do not push index 0 unconditionally. Establish the opening phase from the first
sample that clears the noise threshold, and begin the pair walk at the first
crossing **into inspiration**, discarding the partial breath the file opens on.

```cpp
// Establish opening phase from the first sample that clears the threshold,
// not from clean_flow[0], which is 0.0 for any file that starts at rest.
size_t first_signal = 0;
while (first_signal < clean_flow.size() &&
       std::abs(clean_flow[first_signal]) <= FLOW_THRESHOLD) ++first_signal;
if (first_signal == clean_flow.size()) return breaths;

bool was_positive = clean_flow[first_signal] > 0;
// The list now holds only real crossings, alternating, and we start the walk
// at the first negative-to-positive one so every triple is (insp, exp, insp).
```

A partial breath at each end of the file is dropped rather than guessed. At one
breath per roughly four seconds that is a loss of under 0.1% of an eight-hour
night, against Ti/Te being correct for files that open in expiration.

**This changes output on real data**, measured rather than assumed. Baseline
built from HEAD in a second worktree, both binaries run over 175 real nights:

| | old | new |
|---|---|---|
| breaths, whole card | 733,908 | 747,349 (+1.83%) |
| nights that gained | | 5 (+13,854 breaths, median +1,759) |
| nights that lost | | 168, median **2** breaths, max 9, 413 total |
| per-minute summary count | 49,271 | 49,271 (unchanged) |

The losses are the intended partial breath dropped at each end of each BRP
checkpoint file, about 0.05% of a night. The gains are the five nights whose
files opened in expiration, including **20250926, which produced literally zero
breaths and now produces 5,884**.

### D2. One pass, bucketed by minute

`detectBreaths` runs once over the full flow series, as it already does at
`:184`. `calculateRespiratoryMetrics` stops calling it and instead receives the
breaths whose onset falls in its minute.

One constraint to respect: the leak calculation at `:488` indexes
`minute_flow[j]` using `breath.start_idx`/`end_idx`, which are currently
minute-relative. With a whole-file pass those indices become file-relative, so
the leak loop must index `flow_data` instead. This is the only place breath
indices escape the detector.

A breath straddling a minute boundary is now counted once, in the minute its
onset falls in, instead of being truncated and filtered out of both.

(An earlier draft of this section claimed the last partial minute under-reports
respiratory rate. It does not: `n_minutes` floors, so the trailing partial
minute never gets a `BreathingSummary` at all. Nothing to fix. Whether that
discarded tail is itself worth keeping is a separate question, not this one.)

### What D1 and D2 together actually fixed

This is the payoff, and it is much larger than the five broken nights above.
`Ti` and `Te` are the **only** per-minute fields the breath detector owns.
`EDFParser_PLD.cpp` overwrites respiratory rate, tidal volume, minute
ventilation, leak and flow limitation with the machine's own values, so a fault
in our detector was invisible in every other number on the page.

Measured over the same 175 nights, 49,271 summary minutes:

| | old | new |
|---|---|---|
| minutes carrying a `Ti` | 20,992 (42.6%) | 49,261 (99.98%) |
| mean `Ti` on those minutes | 1.5961 s | 1.5922 s |
| mean `Te` on those minutes | 1.8833 s | 1.9287 s |

**Ti, Te and therefore the I:E ratio were absent from 57% of every night**, and
the mean values barely move, so this is coverage rather than a shift in what the
numbers say. The cause is the two defects compounding: a 1500-sample minute
slice starts at an arbitrary phase, so roughly half of them opened
mid-expiration, hit the D1 parity bug, found no valid breaths, and left the
minute blank. Detecting once over the whole file removes the arbitrary phase and
the parity fix removes the failure, so both halves of the cause are gone.

### D3. REJECTED: a flattening index, computed clean-room

**This was built, validated, and taken back out. Do not rebuild it without an
oracle that says it works.** The design is kept below for whoever tries again.

Albin's gate was that the index had to validate against something like OSCAR or
it would not ship. Two findings came out of that.

**OSCAR cannot be the oracle.** OSCAR does not compute flow limitation for
ResMed at all. It renders the machine's own `FlowLim` channel, the 0 / 0.5 / 1.0
step graph the AirSense records about every two seconds. Running it would have
compared us against the same PLD channel we already read at
`EDFParser_PLD.cpp:182`. Separately, from the S9 onward ResMed derives flow
limitation from a blend of **a flatness index, a breath shape index, ventilation
change, and breath duty cycle**, and that blend is proprietary, which is why
OSCAR never reimplemented it. Our flattening index is only the first of the four
terms.

**So we validated against `FlowLim` directly**, which is a stronger oracle than
OSCAR since OSCAR is only a viewer of it. Per minute: the machine's value
against our per-breath index averaged over that minute. Real card data,
read-only, 175 sessions, **49,256 paired minutes**:

| | new flattening index | old peak-to-mean |
|---|---|---|
| Pearson r vs machine | -0.019 | -0.115 |
| Spearman r vs machine | **-0.003** | -0.021 |
| machine's LOW-FL minutes (n=16447) | 0.0973 | 0.7426 |
| machine's HIGH-FL minutes (n=16436) | 0.0906 | 0.7363 |
| separation | **-0.0066** | -0.0062 |

The machine channel was not degenerate: only 29.6% of minutes were exactly zero,
median 0.0083, p90 0.083, max 0.803, with 299 minutes above 0.3. There was real
range to correlate against and no correlation was found. The single clearest
illustration: on the minute where ResMed reports its highest flow limitation of
the whole card (0.803), our index says 0.233; on the minute where our index
peaks at 0.803, ResMed says 0.000.

The M-shape gap found in unit testing was the tell, and it generalises. A
flatness scalar measures flatness. What ResMed calls flow limitation is three
other things as well.

Harness kept at `scratchpad/fl_oracle.cpp` for whoever tries the next idea.

#### The rejected design, for reference

Replace the peak-to-mean scalar with a shape measure over the inspiratory half.

**Licensing, first.** OSCAR is GPLv3 and this library is MIT. Nobody reads
OSCAR's flow-limitation source, at any point, for any reason. See
`reference_oscar_gpl_constraint`. The method below is the standard published
flattening index from the sleep-physiology literature (Mansour, Aittokallio and
Morgenstern all describe variants of normalise-then-measure-dispersion). It is
described here from the physiology, and OSCAR may be used only as a black-box
oracle, running the binary and comparing output numbers, exactly as SDD-002
constrained the Philips work.

The method:

1. Take the inspiratory samples, `start` to `mid`.
2. Resample to a fixed N = 64 points over normalised time 0..1, linear
   interpolation. This makes fast and slow breaths comparable.
3. Normalise amplitude by the **mean** inspiratory flow, not the peak. The peak
   is a single sample and therefore the noisiest statistic in the breath, which
   is the original heuristic's core weakness.
4. Take the middle 50%, points 16 to 48, discarding the rise and fall.
5. `flattening_index = stddev(mid) / mean(mid)`. A rounded, unobstructed breath
   has a pronounced mid-inspiratory arc and therefore high dispersion. A
   flow-limited breath is flat across the middle and disperses very little.
6. Map to the existing 0..1 convention, where 1 means maximally limited:
   `flow_limitation = clamp(1 - FI / FI_ref, 0, 1)`.

`FI_ref` is the dispersion of an unobstructed breath. A half-sine over its
middle 50% gives an analytic value we can derive and pin as a constant, so the
reference is a property of geometry rather than a number tuned to one night's
data.

**Sample-rate floor.** A 64-point resample of a 1.6 s inspiration needs real
samples underneath it. ResMed BRP at 25 Hz gives about 40, which is enough.
Philips `.005` flow is roughly 5 Hz (`docs/PHILIPS_FORMAT.md`), giving about 8,
which is not. Below a floor of 15 Hz the detector must return **no** flow
limitation rather than a confident-looking number computed from eight points.
That requires `Breath.flow_limitation` to become `std::optional<double>`, which
is the compatibility question below.

## Compatibility

D3 changes the meaning of a number already persisted in every consumer:
`cpap_breaths.flow_limitation` in hms-cpap (all three engines),
`session_metrics.dart` and the Isar store in cpapdash-app, and the API's
session model. Old rows and new rows would sit in the same column under the
same name meaning different things, and nothing in the schema records which is
which.

The two honest ways out are a decision, not an assumption, so they are in the
open questions.

## Tests

New `tests/test_breath_analysis.cpp` (the fork already has a version of this
file, 7 tests, which should be reconciled rather than rewritten):

- **P1 regression:** the same synthetic breath train, phase-shifted to open in
  inspiration and to open in expiration, yields the same breath count and the
  same Ti/Te within tolerance. Today the second case yields near zero.
- A pure sine starting exactly at 0.0 yields breaths. Today it yields none.
- Ti and Te are not swapped: an asymmetric waveform with Ti:Te of 1:2 reports
  that ratio, not 2:1.
- **P2:** breaths from the whole-file pass, bucketed, match a per-minute pass on
  a signal with no boundary-straddling breath; and a breath deliberately placed
  across a boundary is counted exactly once.
- Last partial minute reports a plausible respiratory rate, not a scaled-down one.
- **P3:** a synthetic flat-topped breath scores materially higher than a
  half-sine of identical duration and tidal volume. This is the test the
  peak-to-mean heuristic cannot pass, and it is the reason for the change.
- An M-shaped (twin-peaked) breath scores higher than a half-sine, since
  peak-to-mean puts it near a rounded breath today.
- Flow at 5 Hz returns no flow limitation, not a number.
- Existing `test_prisma_channels.cpp` still passes: the Prisma path sets
  `flow_limitation` from the machine and must not be touched.

Full suite with `CPAPDASH_PARSER_WITH_LOWENSTEIN=ON` before any tag, since
`test_prisma*.cpp` are gated on that flag and the count looks wrong otherwise.

## Resolved

- **Scope.** All three were approved, then D3 failed its validation gate and was
  removed. D1 and D2 stand on their own evidence: they are defect fixes in the
  zero-crossing pairing and the scan structure, and have nothing to do with flow
  limitation.
- **How D3 lands in the data model.** Moot. `Breath.flattening_index` was added
  and then removed; `Breath` is unchanged from 2026.4.1.
- **Re-parse of history.** Moot for the same reason. `flow_limitation` never
  changed meaning, so no consumer has stale-meaning rows.

## Open questions, all Albin's call

1. **Version.** D1 changes output on real data, but only for files that open in
   expiration, which today yield almost nothing. Patch or minor from 2026.4.1?
   Per `feedback_version_numbers_are_albins`.

2. **The fork.** Replay D1 and D2 into `hms-cpapdash-parser-philips` as part of
   this work, or leave it for whenever SDD-002 unparks? The fork's fixture
   generator carries a +0.1 rad workaround that D1 makes unnecessary, and its
   `BreathAnalysis.{h,cpp}` hoist is the same code this SDD just edited in place.

3. **Whether anything replaces D3.** ResMed's four-term blend is proprietary and
   the machine already gives us its answer on every ResMed and Prisma session.
   The only paths with no machine value are BRP-without-PLD, and Philips, whose
   5 Hz flow is too coarse for shape work anyway. There may simply be nothing
   worth building here.

## Migration

1. D1 and D2 in `EDFParser_BRP.cpp` plus `tests/test_breath_analysis.cpp`, with
   the breath-analysis block moved to the public section of `EDFParser.h` so it
   can be tested as a unit. **Done, 176/176 green with
   `CPAPDASH_PARSER_WITH_LOWENSTEIN=ON`.**
2. ~~D3~~ rejected on validation.
3. Consumer wiring, once the parser tag moves: hms-cpap's `CMakeLists` `GIT_TAG`
   pin. No model change, so nothing downstream has to be touched to compile; the
   only visible difference is more breaths on nights whose BRP opened in
   expiration. The app's Dart port is a separate implementation and inherits
   none of this automatically, so it still carries the parity bug.
