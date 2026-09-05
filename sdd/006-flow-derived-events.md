# SDD-006: Apneas from the flow waveform, proven against a machine that reports its own

Status: **draft**
Raised by: hms-homelab/hms-cpap#28, via SDD-005
Depends on: SDD-003 (the breath detector), SDD-004 (event classification)

## 1. Why

A Sefam S.Box user gets no AHI. The device detects events and writes them to a `DET`
channel as a bitfield, and we do not know what a single bit means (SDD-005 §7a). The
same hole opens for any machine whose event encoding we cannot read, and it is the
number a CPAP user looks at first.

But we have the flow waveform, at 25 Hz, and an apnea is not a matter of opinion. It is
a measurable collapse in airflow. So compute the events ourselves.

The reason to do this now, rather than wait for a vendor export, is that **we can prove
it.** ResMed cards carry the flow AND the machine's own event annotations, side by
side, for the same nights. That is a labelled dataset: run the detector on the flow,
compare what it found against what the machine reported, and the disagreement is
measurable rather than argued about.

Done properly this is not a Sefam feature. It is an AHI for **any** machine whose flow
we can read.

## 2. The ground truth we hold

Three ResMed card backups. On the largest alone:

| | |
|---|---|
| Date folders | 238 |
| **With both `_EVE.edf` and `_BRP.edf`** | **175** |
| `_BRP.edf` files (flow, 25 Hz) | 304 |

`EVE.edf` carries the machine's own annotations with onset, duration and label —
obstructive apnea, central apnea, unclassified apnea, hypopnea, RERA, CSR. `BRP.edf`
carries the flow the machine saw when it made those calls. `EDFParser::parseSession()`
already returns both from one session folder, so the harness has nothing to build.

**This is a reference, not a truth.** ResMed's detector is a commercial black box with
its own thresholds, and where we disagree with it, it is not automatically we who are
wrong. What the comparison establishes is whether our numbers are *in the same
clinical world* as a device people are treated on. That is the bar, and §6 sets it.

## 3. What an apnea is

The definitions are the AASM scoring manual's, which is a published clinical standard.

- **Apnea** — a drop in peak airflow excursion of **≥90%** from the pre-event baseline,
  lasting **≥10 seconds**.
- **Hypopnea** — a drop of **≥30%** for **≥10 seconds**, *together with* either a ≥3%
  oxygen desaturation or an arousal.

Note what that second definition needs and we do not have. Steve's card has SpO2 as a
stub in all 242 sessions, and no CPAP gives us EEG arousals. **A flow-only hypopnea is
not an AASM hypopnea**, and calling it one would be inventing the part we cannot
measure. See §5.

## 4. Design

Everything below runs on the breath series `detectBreaths()` already produces (SDD-003),
so there is no second pass over the flow and no second opinion about where a breath is.

### 4.1 Amplitude

Per breath, from the flow. Peak-to-peak excursion over the breath, which is what "peak
airflow excursion" in the definition means, and unlike tidal volume it does not depend
on the flow scale being calibrated — an unresolved question for Sefam (SDD-005). A
threshold expressed as a *fraction of baseline* is scale-free, which is what makes this
portable across machines whose units we have not pinned down.

### 4.2 Baseline

The median breath amplitude over a trailing window, excluding the candidate event
itself so an apnea cannot lower the bar it has to clear.

Window length is an open decision (§7 Q1). A window of a couple of minutes is the usual
choice; too short and a long event erodes its own baseline, too long and a change of
posture or sleep stage reads as an event.

### 4.3 Detection

```
for each breath:
    amp        = peak-to-peak flow over the breath
    baseline   = median amp over the trailing window, event excluded
    suppressed = amp < 0.10 * baseline          (the >=90% drop)

a run of suppressed breaths spanning >= 10 s is an APNEA
```

Duration is measured from the flow, not from the breath count, so a 10-second gap with
no detectable breath at all still qualifies — which is the common case, since a
complete apnea has no breaths in it to count.

### 4.4 What must be excluded, and why this is the part that goes wrong

An airflow signal goes quiet for reasons that are not apneas, and a detector that
ignores them reports a patient as severely apneic when they got up for a glass of
water. Exclusions:

- **Mask off.** Long stretches of near-zero flow with no breath structure. Not therapy
  time at all, so excluded from both the numerator and the denominator of the index.
- **Large leak.** Above a leak threshold the flow signal no longer describes the
  patient's breathing, and both ResMed and OSCAR discount events there. Sefam and
  ResMed both give us a leak channel.
- **The start and end of a recording**, where there is no trailing window to build a
  baseline from.

### 4.5 Where it lives

`src/FlowEvents.cpp` + `include/cpapdash/parser/FlowEvents.h` in the shared parser,
taking a breath series and a flow series and returning `SleepEvent`s. Pure computation,
no I/O, no device knowledge — so ResMed, Sefam and anything else added later use the
same one, and the validation harness can call it directly.

## 5. What the events are allowed to be called

- A detected apnea is `EventType::APNEA` — ResMed's own bare "Apnea" label, its
  unclassified apnea, which counts toward AHI (SDD-004). We can measure that airflow
  stopped. We cannot tell obstructive from central without effort belts, so we will not
  guess: no `OBSTRUCTIVE`, no `CENTRAL`.
- A flow-only hypopnea candidate is **out of v1**. Emitting one as `HYPOPNEA` would put
  it in the AHI numerator on the strength of a rule we only satisfied half of.
- Where a machine reports its own events, **the machine wins**. This detector fills a
  hole; it does not overrule a device that already answered. A session must never carry
  both, or every event is counted twice.

## 6. Validation, and the gate

A tool, `tools/flow_events_validate.cpp`, run by hand over a card. **Not a unit test.**
Tests never run on a real person's therapy data; the unit tests use synthetic flow with
events written into it deliberately.

Per night, over the 175 labelled nights:

- **Event matching.** A detected apnea matches a reported one when they overlap in
  time. Report precision, recall and F1 against ResMed's apneas.
- **Index agreement.** Our AI/h against ResMed's AI/h — the number a user actually
  reads. Bland-Altman style: mean difference and the spread of it, per night.
- **Failure inspection.** The worst-disagreeing nights dumped for a look, because a
  detector that is right on average and catastrophic on six nights is not usable.

**Release gate.** No flow-derived event reaches a user, on any machine, until the
agreement targets in Q3 are met and Albin has seen the numbers. Until then the detector
is built, tested on synthetic flow, and run only by the validation tool.

## 7. Open decisions

These are Albin's, and the work below them proceeds under stated assumptions rather
than waiting.

- **Q1. Baseline window.** Proposed: 120 s trailing, event excluded.
- **Q2. Leak exclusion threshold.** ResMed's own large-leak line is 24 L/min. Proposed:
  reuse it, and make it a parameter rather than a constant.
- **Q3. The agreement targets that count as proven.** Proposed as a starting point:
  recall ≥ 0.80 and precision ≥ 0.70 against ResMed apneas, and per-night AI within
  ±1.0 events/hour on ≥ 90% of nights. These are a proposal, not a standard; the data
  may say they are too lax or unreachable.
- **Q4. Hypopneas.** Out of v1 here. Whether a flow-only hypopnea is ever surfaced —
  under a different name, outside the AHI — is a product decision.
- **Q5. Whether ResMed keeps using its own events** once this exists. Proposed: yes,
  always. §5.

## 8. Clean-room

The AASM criteria are a published clinical standard and may be implemented from the
definition. **OSCAR's event detection is GPL and must not be read**, on the same terms
as SDD-002 and SDD-005: it may be used as a black-box oracle by running the binary and
comparing numbers, never as a source. ResMed's own annotations are data, not code, and
using them as a reference is exactly what they are here for.
