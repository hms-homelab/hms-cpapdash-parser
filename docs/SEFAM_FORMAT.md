# Sefam S.Box card format

Companion to `sdd/005-sefam-sbox-support.md`. This is the living record of what we
actually know about the format, and how we came to know it.

> **Status: nothing here is confirmed yet.** No donor card has been received. Every
> row below is a hypothesis the parser is built to *test*, not a fact it assumes.
> The library discovers the header length, the descrambling and the channel table at
> parse time (SDD-005 §6), so an entry being wrong costs us a failed parse, never a
> wrong number in somebody's therapy data.

## Provenance rules

Two prior write-ups of this format exist. One is **GPL-3.0**
(`garett09/OSCAR-V2`, `Notes/loaders/SD_CARD_FINGERPRINTS.md`), one has **no license
at all** (`ChrisAylen/sefam-nea-analysis`). This library is MIT. Neither may be
transcribed, quoted, or ported. They were read once for orientation, and they are the
reason we knew to look for an INI, which is an idea and not expression.

They also contradict each other on `.STS`, which is a fair warning about treating
either as authoritative.

Consequently, **every row in this document gets a status and a source**, and the only
source that promotes a row to `CONFIRMED` is a real card, or the vendor's own
documentation, or an experiment we ran ourselves.

| Status | Meaning |
|---|---|
| `CONFIRMED` | verified on a real card we hold, or stated in vendor documentation |
| `HYPOTHESIS` | plausible, unverified, and something the parser probes for at runtime |
| `UNKNOWN` | open question, listed so it does not get quietly assumed |

## Vendor documentation

These few come from Sefam's own published SEFAM Analyze user manual, so they are
`CONFIRMED` in the sense that the vendor says so, without a card in hand.

| Fact | Status |
|---|---|
| Compliance data is recorded per session, on the card, up to about a year at 8 h/day | CONFIRMED (vendor) |
| The card also holds real-time "HD" signals for about three months | CONFIRMED (vendor) |
| HD signals are reachable *only* by reading the card, not over the network link | CONFIRMED (vendor) |
| Channels include flow, pressure and unintentional leak on every S.Box | CONFIRMED (vendor) |
| DuoS / DuoST add respiratory rate and tidal volume | CONFIRMED (vendor) |
| An attached oximeter adds SpO2, heart rate and a pulse waveform | CONFIRMED (vendor) |
| The PolyLink kit adds respiratory effort and body position | CONFIRMED (vendor) |
| Files on the card are grouped by device serial number | CONFIRMED (vendor) |
| The device classifies apnea and hypopnea as obstructive or central, and reports AHI, snoring, flow-limitation runs and desaturations | CONFIRMED (vendor) |
| SEFAM Analyze can export sessions as semicolon-delimited UTF-16 text | CONFIRMED (vendor) |

That last one matters for verification: it is the oracle we check decoded numbers
against, the way OSCAR was used as a black-box oracle for Philips.

## Card layout

| Element | Status |
|---|---|
| `<card root>/<model><rev>/<serial>/` two-level nesting | HYPOTHESIS |
| `DATA_<N>/` session folders inside the serial folder | HYPOTHESIS |
| `DATA_<N>/DATA_<N>.INI` schema file per session | HYPOTHESIS |
| One channel file per declared channel, extension = channel name | HYPOTHESIS |
| `<serial>.RAM` and `<serial>.BKP` device state at serial level | HYPOTHESIS |
| `upload.dat` at card root | HYPOTHESIS |
| Whether older `DATA_<N>` folders are pruned, and the cap | UNKNOWN |

## The INI

Plain Windows INI, `[Section]` and `key=value`.

| Section | Carries | Status |
|---|---|---|
| `[Create Info]` | model string, serial, firmware version, creation date | HYPOTHESIS |
| `[Oximeter]` | oximeter type, `NONE` when unattached | HYPOTHESIS |
| `[BLE Device]` | pairing addresses | HYPOTHESIS |
| `[Start Record]` | recording start split across `Hour`/`Min`/`Sec`/`Day`/`Month`/`Year`, plus programmed and real durations in seconds | HYPOTHESIS |
| `[Chan0]`..`[ChanN]` | per channel: `Name`, `Unit`, `Freq`, `Bit`, `Min`, `Max` | HYPOTHESIS |

`Serial Number` in `[Create Info]` appears to be the model code and the serial
concatenated, which is what the device treats as its identity string. The parser keeps
the whole string and does not try to split it.

**Two-digit years.** If the date is written `DD/MM/YY`, the century is ambiguous.
The parser windows it (69 and below is 20xx) rather than guessing per card.

## Channels

The parser reads this table from the INI at run time and hard-codes none of it. What
follows is only the set we expect to meet, so that the name-to-meaning mapping in
`SefamParser_Channels.cpp` has somewhere to start.

| Extension | Expected meaning | Status |
|---|---|---|
| `.FLW` | flow, L/min | HYPOTHESIS |
| `.PRE` | pressure, cmH2O | HYPOTHESIS |
| `.LK` | unintentional leak, L/min | HYPOTHESIS |
| `.DET` | event detection codes | HYPOTHESIS |
| `.SPO` | SpO2, % | HYPOTHESIS |
| `.HRT` | heart rate, bpm | HYPOTHESIS |
| `.PLS` | pulse waveform, 16-bit | HYPOTHESIS |
| `.STS` | device status / state | HYPOTHESIS |
| `.THO` | thoracic effort | HYPOTHESIS |
| `.ABD` | abdominal effort | HYPOTHESIS |
| `.POS` | body position | HYPOTHESIS |
| `.NSD` | undeclared in the INI, meaning unknown | UNKNOWN |
| `.Y17` | undeclared in the INI, meaning unknown | UNKNOWN |

An extension we have never seen is not an error. If the INI declares it, the parser
reads it generically; if the INI does not, the parser ignores the file and records a
note.

## Sample storage

| Element | Status |
|---|---|
| Fixed-size header at the start of every channel file, identical length card-wide | HYPOTHESIS |
| The header length itself | **UNKNOWN, discovered at parse time** (SDD-005 §6.2) |
| Samples after the header, one byte each, two for a 16-bit channel | HYPOTHESIS |
| A declared-but-unused channel is written as header only, no samples | HYPOTHESIS |
| Sample bytes are scrambled | HYPOTHESIS |
| The transform | **UNKNOWN, searched for at parse time** (SDD-005 §6.3) |
| Signedness of an 8-bit channel whose declared `Min` is negative | UNKNOWN |

The header length and the transform are the two facts a naive implementation would
hard-code from somebody else's document. Discovering both from the card is what keeps
this library independent of sources it may not use, and it is also simply more robust:
a firmware that changes either one still parses.

One result worth recording, because it is a fact about the problem rather than about
Sefam. **Smoothness can never identify a single-byte XOR key on its own.** XOR by
`0xFF` is `x -> 255 - x`, which mirrors a signal while leaving the distance between
neighbouring samples untouched, so a key and its complement score identically under
any measure of smoothness. An idle stretch breaks the tie, because a channel recording
nothing is recording zero. Without one, the parser refuses the session: a mirrored
reading turns a pressure of 10 into 15.5, and it still looks like breathing.

## Events

`.DET` is a sampled channel, not an annotation list: one code per sample at the
channel's declared rate, so an event is a run of equal non-zero codes.

**What the codes mean is `UNKNOWN`, and v1 does not guess.** Every run becomes
`EventType::OTHER` carrying the raw code, which under SDD-004 counts toward
`total_events` and toward no clinical index. Sefam AHI reads 0 until the map is
established against a donor card plus the matching Sefam Analyze report.

## Timing

| Element | Status |
|---|---|
| Session start comes from `[Start Record]` | HYPOTHESIS |
| Therapy duration is sample count divided by declared rate | **decision, not observation** |
| `Real Record Duration` in the INI may disagree with the sample count | HYPOTHESIS |

Where the two disagree the parser believes the samples. A duration field that
describes an intended recording length rather than what was written would otherwise
inflate the denominator of every index. This is the same failure that put an 11h22m
duration on a 3h42m night for ResMed (support ticket 87), and it is worth not
repeating.

## Open questions

Tracked in SDD-005 §10.
