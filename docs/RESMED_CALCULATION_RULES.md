# ResMed calculation rules

Which ResMed number comes from where, and why. One place, so the reasoning stops being
scattered across comments in `ParsingService.h`, `Models.cpp` and three SQL strings in
`QueryService.cc`.

Ruled by Albin 2026-09-04: **compute the indexes ourselves, take everything else from STR
when STR is present, fall back to the signal files when it is not.**

Every number in this document was measured from a real ResMed card
(`~/cool_shit/cpap_card_backup_20260827`, AirSense, `MID=36 VID=39`, 261 STR days,
181 EVE files). Nothing here was taken from OSCAR source. That rule is absolute, see
`BRAND_RESEARCH.md`.

---

## 1. The two kinds of number

A ResMed card gives us the same quantity twice, and which copy is better depends on the
kind of quantity it is.

**Kind A, the machine knows something we do not.** Mask-on time, and the percentiles it
took over its own therapy window. We can approximate these from the waveform files, but we
are guessing at which samples counted as therapy and how the percentile was taken. The
machine is not guessing. STR wins.

**Kind B, plain arithmetic over things we can count exactly.** The indexes: AHI, AI, HI,
OAI, CAI, UAI, RIN. Each is `count / hours`. We hold the exact event count from the EVE
file and the exact hours. The machine held the same numbers and then wrote the answer into
a channel that cannot carry it. We win.

The mistake to avoid is treating "STR is authoritative" as a single rule. It is
authoritative for kind A and lossy for kind B.

---

## 2. Measured STR channel resolution

`scale = (phys_max - phys_min) / (dig_max - dig_min)` from the EDF header. This is the
smallest step the channel can represent. Anything finer was destroyed by the machine
before we opened the file.

| Channel | Step | Kind |
|---|---|---|
| `Duration` | 1 min | A, machine's own mask-on time |
| `Leak.50` / `.70` / `.95` / `.Max` | 0.02 L/s | A, machine's own percentile |
| `MaskPress.50` / `.95` / `.Max` | 0.02 cmH2O | A |
| `BlowPress.5` / `.95` | 0.02 cmH2O | A |
| `RespRate.50` / `.95` | 0.2 bpm | A |
| `TidVol.50` | 0.02 L | A |
| `MinVent.50` | 0.125 L/min | A |
| `SpO2.50` / `.95` | 1 % | A |
| `MaskEvents` | 1 | A |
| **`AHI`, `HI`, `AI`, `OAI`, `CAI`, `UAI`, `RIN`** | **0.1** | **B, derived index** |
| `CSR` | 1 | B |

Leak and pressure at 0.02 are not coarse. STR wins those on definition, not precision.
The index group is the only place where STR is strictly a lower-precision copy of
arithmetic we can do exactly.

Reproduce with `tools/str_resolution.py` (see section 7).

---

## 3. ResMed floors its indexes, it does not round

Comparing the STR `AHI` channel against `(apneas + hypopneas) / hours` computed from the
same day's EVE annotations, 14 of 15 days match `floor(computed, 1dp)` exactly:

```
date        STR AHI  events   hours  computed AHI  floor(.1)   round(.1)
20250722       1.20       7   5.467        1.2805        1.2         1.3
20250723       2.80      11   3.817        2.8821        2.8         2.9
20250725       1.90      12   6.050        1.9835        1.9         2.0
20250729       4.50      14   3.067        4.5652        4.5         4.6
20250730       2.70      17   6.133        2.7717        2.7         2.8
20250806       3.80      10   2.600        3.8462        3.8         3.8
20250807       2.30      15   6.317        2.3747        2.3         2.4
```

STR matches the floor column, never the round column. So a customer whose true AHI is 0.49
sees 0.4 from STR, and 0.40 once a two-decimal formatter pads it. This is exactly the
"it always rounds down" report from Rohan Misquith, 2026-09-04.

**Consequence:** a second decimal on any STR-sourced index is always a lie. It is not a
formatting bug, the digit does not exist in the file.

---

## 4. Source of truth, per field

| Field | Source | Note |
|---|---|---|
| AHI, AI, HI, OAI, CAI, UAI, RIN | **computed** | `count / hours`, full precision |
| therapy hours / usage | STR `Duration` | machine's mask-on time, and the index denominator |
| leak p50 / p95 / max | STR `Leak.*` | `Leak.2s` percentiles land on flat steps |
| mask pressure p50 / p95 / max | STR `MaskPress.*` | percentiles only, mask channel only |
| therapy pressure | **computed** from PLD `Press.2s` | STR has no therapy percentile; `MaskPress` reads ~0.8 cmH2O low with EPR on |
| means, and STR's medians for resp rate / tidal volume / minute ventilation / SpO2 | **computed** | swapping a mean for a median is a definition change, not a correction |
| SpO2 | O2 Ring first, machine as fallback | the machine's own SpO2 can be unscaled or misassigned |

**No STR at all:** Löwenstein and Philips never emit one. Everything falls back to the
signal files, which is already the behaviour and must stay that way.

---

## 5. Traps that have already bitten, or are waiting to

**The index denominator is stale.** `sessions.ahi` is written once at parse time by
`updateSessionMetrics`, dividing by the *parser's* duration. `applyStrMetricsToSessions`
later overwrites `sessions.therapy_hours` with STR's duration and never recomputes `ahi`.
So the stored index and the stored hours disagree. Reading `sessions.ahi` straight gives
full precision of a number built on the uncorrected denominator. Any index must be
recomputed from a count and the *final* hours.

**The AHI numerator is not fully persisted.** The parser counts a generic
`EventType::APNEA` bucket (`apnea_other`) toward AHI, and `session_metrics` has no column
for it. It is not hypothetical, ResMed writes that label:

```
741  'Obstructive Apnea'
534  'Hypopnea'
396  'Arousal'
233  'Central Apnea'
 28  'Apnea'          <-- counts toward AHI, no column
```

28 of 1536 AHI-relevant events, about 1.8%. On a low-AHI user, where a whole night is
three events, dropping one moves the number visibly. AHI cannot be reconstructed from
`obstructive_apneas + central_apneas + clear_airway_apneas + hypopneas` alone.

**A night is not a session.** STR's index covers a whole day. A night split into two
sessions must aggregate as `sum(counts) / sum(hours)`, never as an average of per-session
indexes, and never as whichever single session `DISTINCT ON (date)` happened to pick.

**Event counts derived from a floored index.** `QueryService.cc:270` falls back to
`ROUND(d.ahi * d.duration_minutes / 60.0)` to reconstruct an event *count* from the floored
index. Under one event a night, and only when `session_metrics.total_events` is null, but
it is compounding a known-lossy number and disappears once the index is computed.

**`total_events` is not the AHI numerator.** RERA, flow limitation, vibratory snore and
large leak are recorded for display and are excluded from AHI. Using `total_events`
over-counted badly on machines that flag many flow-limitation events, such as the
Löwenstein Prisma SMART max.

---

## 6. What OSCAR and SleepHQ appear to do

Inferred from customer-visible behaviour only, never from reading their source.

They **do** rely on STR for the kind A fields. Adopting STR's leak, usage and mask pressure
is what made our numbers line up with theirs, confirmed by Rohan on 2026-08-09: "The
pressure (med/P95) seems to align with Oscar and SleepHQ now."

They **do not** rely on STR for AHI. They display two decimals, and two decimals cannot
come out of a 0.1-step channel. They compute it, the same division we do.

So matching them is not a compromise between two philosophies, it is the same split this
document describes.

---

## 7. Reproducing the measurements

The scripts used to produce sections 2, 3 and 5 live in `tools/`:

- `tools/str_resolution.py` — dumps every STR channel's step and distinct-value count
- `tools/ahi_compare.py` — STR AHI vs EVE-computed AHI per day, with floor and round columns
- `tools/eve_labels.py` — census of every annotation label ResMed writes
- `tools/index_validate.py` — every computed index against its own STR channel, per type,
  with the first day each one disagrees

They read the card directly and depend on nothing in this repo. The card path is a constant
at the top of each one. If a claim here is ever doubted, re-run them against a fresh card
rather than trusting the tables.
