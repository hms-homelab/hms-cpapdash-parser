# SDD-004: event classification and the counts the indexes need

Status: **DECIDED, not implemented.** Ruled by Albin 2026-09-04.
Companion to `cpapdash-ingest/sdd/001-computed-indexes.md`. Read that first for the why.
Measurements: `docs/RESMED_CALCULATION_RULES.md`.

This repo is shared, so this spec is also the hms-cpap local spec. Anything that lands here
lands on Albin's local path and on Rohan's Docker install on his Mac mini.

---

## 1. Problem

Two defects in `EDFParser_EVE.cpp` block computing the index group.

**1.1 Unknown annotations are silently classified as RERA.**

```cpp
} else if (desc_lower.find("csr") != std::string::npos) {
    event_type = EventType::CSR;
} else {
    // Arousal, flow limitation, etc. -- still record them
    event_type = EventType::RERA;
}
```

The final `else` is a catch-all: every annotation this repo does not recognise is stored as a
RERA event.

**CORRECTION, 2026-09-04.** An earlier draft of this spec claimed the catch-all swallows the
181 `'Recording starts'` markers on the sample card, one per EVE file, inflating
`session_metrics.reras` by the file count. **That is false in this repo.**
`EDFFile::readAnnotations()` at `src/EDFFile.cpp:391-392` already drops descriptions equal to
`"Recording starts"` or `"Recording ends"`, one layer above the classification chain, so
those annotations never become events at all. The card writes the label byte-exact, so the
filter catches every one of the 181. The claim was made from reading the classifier without
checking the layer above it.

What survives the correction: the catch-all is still wrong for **every other** unrecognised
label. A ResMed firmware that introduces a new annotation, or any label this chain does not
match, silently becomes a clinical RERA. That is the defect `EventType::OTHER` fixes, and it
is worth fixing on its own merits. The sample card simply happens to produce zero catch-all
events, so the corruption is latent here rather than active.

`'Arousal'` mapping to RERA is correct and deliberate: RIN computed from arousals alone
reconciles against ResMed's own `RIN` channel on 172 of 175 nights. The bug is only the
catch-all beneath it.

**1.2 The AHI numerator is not fully persisted.**

`Models.cpp` counts a local `apnea_other` (from `EventType::APNEA`) into the AHI numerator,
but there is no column for it, so a consumer reading
`obstructive_apneas + central_apneas + clear_airway_apneas + hypopneas` gets a short count.
It is not hypothetical: ResMed writes a bare `'Apnea'` label.

```
741  'Obstructive Apnea'
534  'Hypopnea'
396  'Arousal'
233  'Central Apnea'
181  'Recording starts'   <-- dropped by EDFFile.cpp:391 before classification, NOT a RERA
 28  'Apnea'              <-- counts toward AHI, no column
```

That bare `'Apnea'` is ResMed's unclassified apnea. Computing UAI from it reconciles against
ResMed's own `UAI` channel on **175 of 175** nights, which is what identifies it.

## 2. Design

### 2.1 A real 'other' bucket

Add `EventType::OTHER`. The catch-all `else` maps to it instead of RERA.

Ruling (Albin): unknown annotations are **recorded but belong to no index**, and they
**stay inside `total_events`**.

- Recorded, so a ResMed firmware that introduces a new label shows up in the data instead of
  quietly inflating a clinical index. That is precisely how this bug survived.
- Inside `total_events`, so no user's displayed event count drops at the release and no trend
  gets a seam.

`'Arousal'` keeps its explicit RERA mapping. Add it as an explicit branch rather than leaving
it to fall through the catch-all, so the classification is stated rather than incidental.

### 2.2 Counts to expose and persist

`SessionMetrics` exposes, and the API persists:

- `unclassified_apneas` — the `EventType::APNEA` bucket, previously the unpersisted
  `apnea_other` local.
- `other_events` — the new `EventType::OTHER` bucket.

Existing count fields are unchanged.

### 2.3 Index computation

The parser keeps computing `metrics->ahi` for its own callers, but the numerator becomes
explicit and the full index group is derived from the same counts. Definitions are in the
ingest spec, 2.2. They are stated once there; do not restate them in code comments here,
reference the rules doc.

The denominator remains the caller's supplied duration. The parser does not know about STR.
Correcting the denominator after STR arrives is the ingest service's job, and the parser must
stay recomputable so it can be redone (ingest spec, 3.7).

## 3. Migration

Parser-side is additive: a new enum value and two new count fields. No format change, no
stored data.

The user-visible consequence lands downstream: `reras` stops including file markers, so RIN
and any RERA display drop by roughly one per EVE file. That is a correction, not a
regression, and it is why the ingest spec's backfill reparses history rather than migrating
it.

## 4. Tests

- Every label in the census (2.1 table) classifies to its intended type. `'Recording starts'`
  is `OTHER`, `'Arousal'` is `RERA`, bare `'Apnea'` is `APNEA`.
- An unrecognized invented label lands in `OTHER`, not `RERA`. This is the regression guard
  for the original defect.
- `total_events` is unchanged before and after the reclassification for the same input.
- The AHI numerator equals obstructive + central + clear-airway + unclassified + hypopnea,
  and excludes RERA, OTHER, CSR, flow limitation, snore and large leak.
- UAI computed from the bare `'Apnea'` bucket floors to the STR `UAI` channel.

## 5. Cross-repo note

**CORRECTED 2026-09-04. hms-cpap does NOT inherit this fix.** An earlier draft said it did,
on the strength of `hms-cpap/CMakeLists.txt:212-220` pulling this repo in by `SOURCE_DIR`.
That much is true, but hms-cpap ALSO carries its own parallel implementation:

- its own `EventType` at `include/models/CPAPModels.h:22`
- its own EVE classification chain at `src/parsers/EDFParser.cpp:1003-1004`, holding the
  identical unfixed catch-all, comment and all

So Albin's local path and Rohan's Docker install keep the old behaviour until that second
copy is fixed too. Verify which of the two paths actually runs before assuming either.

Two implementations of the same classification in one product is the underlying problem here,
and it is what made the wrong inference easy. Consolidating them is not in this spec's scope
but should be raised.
It has its own display path though; the two-decimal formatting there was part of the
uncommitted 2026-08-29 sweep and is tracked in `cpapdash-app/sdd/079-index-precision.md`
alongside the app's.
