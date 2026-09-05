# SDD-005: Sefam S.Box support

Status: **draft, implementation in progress**
Raised by: hms-homelab/hms-cpap#28 (2026-09-05)
Depends on: SDD-004 (the OTHER bucket)

## 1. Why

A user asked for Sefam S.Box support and offered an SD card dump. Nobody reads this
format today: OSCAR has no Sefam loader and Sefam themselves say the device is not
OSCAR compatible; SleepHQ's supported list is ResMed, Philips and Löwenstein only.
Supporting it is a first, and it is the cheapest new brand we have looked at, for one
reason given in section 3.

The S.Box is made by Sefam (Nancy, France), industrial design by Philippe Starck, and
sold as "S.Box by Starck". The range shares one clinician application (SEFAM Analyze)
and, as far as we can tell, one card format:

| Model | Therapy |
|---|---|
| S.Box, S.Box C | CPAP / Auto-CPAP |
| S.Box AUTO | Auto-CPAP **and** Type-3 polygraph (effort belts, oximetry, position) |
| S.Box DuoS, S.Box DuoST | bi-level NIV (adds respiratory rate, tidal volume) |

## 2. Sources, and what we are allowed to use

Two write-ups of the format exist. Neither may be copied.

| Source | License | Standing |
|---|---|---|
| `garett09/OSCAR-V2`, `Notes/loaders/SD_CARD_FINGERPRINTS.md` | **GPL-3.0** | hint sheet only |
| `ChrisAylen/sefam-nea-analysis` (Sefam Néa viewer) | **none, all rights reserved** | hint sheet only |

The GPL document is *not* derived from OSCAR loader code -- it says outright that no
Sefam loader exists anywhere in `oscar/SleepLib/`, so it is an independent analysis of
a donor card that happens to live in a GPL repository. That makes it cleaner than
`prs1_loader.cpp` was, and it is still GPL-licensed text, and this library is MIT. The
rule from SDD-002 stands unchanged: **read for orientation, transcribe nothing.**

The two sources also disagree. The Néa write-up reads `.STS` as event data with codes
1/2/3; the S.Box card has `.STS` as a status channel and a separate `.DET` for event
detection. That is a good reminder that neither is authoritative.

So this SDD does not encode a channel table, a header length, a scrambling key, or an
event-code map. It encodes a **design that discovers all four from the card itself**.
Everything normative comes from the donor dump when it arrives.

## 2a. What the donor card did to sections 6.2 and 6.3

The card arrived on 2026-09-05, after this design shipped as 2026.7.0. It is an S.Box
AUTO, 242 sessions, ten months.

Sections 6.2 and 6.3 below were reasoned out with no card to look at, and the card
falsified both. They are kept as written, because how a careful guess went wrong is
worth more than a tidy document, and because the discipline they describe is the reason
the damage was nil: the parser refused every session on this card rather than reporting
numbers from a model that did not hold.

What was wrong:

- **There is no header length to search for.** It is a fixed 38 bytes, and the samples
  are not a flat array after it. They are framed in ten-second blocks, each followed by
  a checksum and a block index. Section 6.2's arithmetic assumed a flat array and could
  never have fitted the sizes.
- **There is no scrambling to break.** The samples are plain. Section 6.3 solves a
  problem the format does not have. What looks scrambled is the 38-byte header, which
  is ASCII under XOR `0xBF` and appears in every file — including the stubs, which is
  why a shared noisy lead-in looked like a card-wide cipher to everyone who saw it.
- **The declared Min/Max do not scale a channel**, so `toPhysical()` was wrong too.
- **`DET` is a bitfield, not an enumeration**, so section 7's runs-of-a-code model was
  the wrong shape. See section 7a.

What survived, and it is the part that mattered:

- Duration from the sample count and never from the INI. Every session on this card
  declares eight hours; the real recordings run from two minutes to 6h24m.
- A declared-but-unrecorded channel is written as header-only.
- Refuse rather than guess. A design that had quietly assumed an offset would have
  produced 242 plausible-looking nights of nonsense.
- The card layout, the INI sections and the channel set, all as expected.

And one thing the card gave us that no amount of reasoning would have: **a checksum on
every block**. 838,440 of them across the card, all correct. That is a stronger
guarantee than anything in the original design — a misread offset or rate cannot pass
quietly — and it is now what the parser refuses on.

`docs/SEFAM_FORMAT.md` carries the measured format.

## 3. The insight this design rests on

Every session folder carries a plain Windows INI that declares its own schema:
channel name, unit, sample frequency, bit depth, and the min/max range of each
channel. A Sefam parser therefore needs almost no per-device knowledge baked in. It
needs to read an INI, work out where each binary file's samples start, and undo
whatever scrambling is applied to them.

That is why this is cheap compared to Philips PRS1, where the channel layout is
firmware-internal and every field had to be pinned down in advance.

It is also why the clean-room constraint costs us nothing. A parser that hard-codes
a channel table would depend on somebody else's document. A parser that reads the
table out of the INI depends on the card.

## 4. Scope

### In, v1

- Detection of a Sefam card and of a single session folder.
- The INI reader: identity, recording start, and the channel table.
- Header-length discovery (section 6.2) and descrambling (section 6.3), both derived
  from the files at parse time, neither hard-coded.
- Flow, pressure and leak into `BreathingSummary` per minute, plus leak into
  `native_samples`.
- SpO2 and heart rate into `VitalSample` when an oximeter was attached.
- `.DET` runs into `SleepEvent` as **`EventType::OTHER`**, never as apnea or hypopnea.
  See section 7.
- Sessions: one `ParsedSession` per `DATA_<N>/` folder.

### Out, v1

- **Event classification.** We do not know what a `.DET` code means. Guessing would
  put invented numbers into somebody's AHI, so v1 counts them as OTHER and the map is
  filled in only when a donor card can be checked against a Sefam Analyze report.
- Effort belts, body position, and the pulse waveform (`.THO`, `.ABD`, `.POS`,
  `.PLS`). The models have nowhere to put them yet. Their presence is recorded.
- The undeclared `.NSD` and `.Y17` files. Not in the INI, meaning unknown, ignored.
- `.RAM` / `.BKP` device state dumps and `upload.dat`. Not session data.
- The bi-level members of the range (DuoS, DuoST). Same format, but no donor card and
  no way to validate pressure support numbers.

## 5. Layout

```
<card root>/
  <model><rev>/                 e.g. digits then a letter
    <serial>/
      <serial>.RAM              device state, ignored
      <serial>.BKP              device state, ignored
      DATA_0/
        DATA_0.INI              the schema
        DATA_0.FLW              one file per channel, extension = channel name
        DATA_0.PRE
        ...
      DATA_1/
      DATA_2/
  upload.dat                    cloud staging, ignored
```

Detection anchors on `DATA_<N>/DATA_<N>.INI`, which is specific enough that no
other supported brand can collide with it, and cheap enough to check by name.

## 6. The three things the parser has to work out for itself

### 6.1 The channel table

Read from `[Chan0]`..`[ChanN]` in the INI: `Name`, `Unit`, `Freq`, `Bit`, `Min`,
`Max`. The file whose extension equals `Name` holds that channel's samples. Nothing
about the set is assumed: an unknown `Name` is carried through as an unmapped channel
rather than dropped or guessed at.

### 6.2 Where the samples start

Each channel file begins with a fixed-size header of unknown length `h`, the same for
every file on the card.

The channels of one session cover **one span of wall-clock time**, each at its own
rate. So the right `h` is the one that makes every file agree on how long the
recording was, and every wrong one makes them disagree, by more the further off it is,
because the channels divide by different rates. That is the whole method: try each
`h`, keep the one that reconciles them.

Both of the derivations one might write down separately fall out of it. A pair of
channels at different rates pins `h` arithmetically, since

```
(size_A - h) / (bytes_A * freq_A)  ==  (size_B - h) / (bytes_B * freq_B)
```

has one solution. And a channel that was declared but never recorded is written as a
file of exactly `h` bytes, whose payload goes to zero at the right answer and to a
nonsense one-sample recording anywhere else.

Two guards, both learned by writing the tests:

- **Only channels at different rates constrain anything.** One populated channel
  agrees with itself at every offset, and a set that all run at one rate shifts
  together. Without two distinct rates there is no answer, only a preference. This is
  also what stops a huge `h` from scoring perfectly by declaring every channel but one
  to be a stub.
- **A channel may end a sample or two short of its neighbours.** A second of slack
  absorbs that; a full minute of disagreement is a different recording and is refused.

Anything short of a single reconciling `h` returns nothing. An offset wrong by one
byte shifts every sample in the night and the output still looks like therapy data.

### 6.3 The scrambling

The sample bytes are probably not plain. We do not assume a key; we **search** for
one.

The declared range cannot score the candidates, because a raw code is mapped into
`[Min, Max]` by construction (section 6.1), so every key "fits". Smoothness can: flow
at 25 Hz moves a little between neighbouring samples, and XOR against the wrong key
turns a step of 1 into a step of 128 wherever it flips a high bit. So the true key
minimises the mean absolute difference between neighbouring samples.

**That gets us to two candidates and provably no further.** XOR by `0xFF` is
`x -> 255 - x`: it mirrors the signal and leaves every step exactly the length it was,
so a key and its complement score *identically*, always. This is a property of the
measure, not a shortcoming of the search.

An **idle stretch** settles it. A channel that recorded nothing recorded zero, so the
byte a long constant run repeats is the key outright. Real cards have them: the
detection channel is zero whenever nothing is being detected.

With no idle stretch, or where the idle stretch names a key the smoothness search does
not (which is what a repeating multi-byte key looks like through a single-byte
search), the result is marked ambiguous and **the session is refused**. Reading a card
mirrored is not a near miss: a pressure of 10 comes out as 15.5 and still looks like
breathing.

If the donor card turns out to use a repeating key, the same loop takes the wider
candidate set and the idle-stretch tie-break still applies.

## 7. Events

`.DET` is a per-sample channel of event codes, not an annotation list. The parser
turns each run of one non-zero code into a single `SleepEvent`, with the run length as
the duration.

Every one of them is `EventType::OTHER`, with the raw code in `details`. Per SDD-004
that means they are inside `total_events` and inside no clinical index. Sefam's AHI
therefore reads 0 in v1, which is correct: we would rather report no AHI than a
fabricated one.

Filling in the map is a follow-up, and it needs two things together: a donor card, and
the Sefam Analyze report for the same nights to check against.

## 7a. Events, after the card

`DET` is a bitfield of concurrent flags. Over one 6h24m night it takes 85 distinct
values decomposing into eight independent bits, one of them set for 58% of the
recording and another for 0.22%.

A bit set for most of the night is not an apnea. Reading runs of the whole byte, which
is what section 7 describes and what shipped in 2026.7.0, would have produced tens of
thousands of "events" a night out of flags toggling against each other — a count that
is an artefact of the encoding, not a finding about the patient.

So v1 emits **no events at all** and reports each bit's share of the recording in
`SefamSessionNotes::det_bit_share` instead. A Sefam session carries no AHI. That is a
reduction from what 2026.7.0 did, and it is the honest reading: we now know the shape
of the channel and still not the meaning of a single bit.

## 8. Verification

**Now, without a card.** Synthetic fixtures generated by
`tests/fixtures/sefam/gen/make_fixtures.py`, using a header length and a scrambling
key that appear nowhere in the library, so a parser that ever starts assuming either
one fails the suite. They cover a scrambled session inside the two-level card tree, an
unscrambled one with oximetry and an unmapped channel and an undeclared file, a card
where every channel is a stub, and a card whose header length cannot be recovered.
Alongside them, unit coverage of the INI reader, the header search, the key search and
the event decoding. 59 tests.

These prove the discovery logic, which is the part that would otherwise be untestable
until a card shows up, and writing them is what turned up the complement degeneracy in
section 6.3 and the two guards in section 6.2.

**On the donor card, done 2026-09-05.** `tools/sefam_probe.cpp` reads a card and reports
what the parser made of it. Over the 242 sessions:

- **241 parsed, 1 refused** — the refusal being a session in which every channel is a
  stub, which is a session with nothing in it.
- **838,440 block checksums, none failing.** The framing is read correctly.
- Identity and start time agree between the file header and the INI in all 242.
- Durations 2 minutes to 6h24m, against the eight hours every session declares.
- Pressures average 0.5 to 12 cmH2O across the card; leak 2 to 25 L/min.

**Still outstanding, and it needs SEFAM Analyze.** The reporter sent the vendor
application alongside the card. Its session export is the oracle that settles the three
things the data alone cannot: the physical scale of each channel, the meaning of the
`DET` bits, and therefore whether a Sefam night can carry an AHI at all. Nothing should
be promoted out of `SUPPORTED` in `docs/SEFAM_FORMAT.md` before that.

**Release gate.** Released at **2026.7.0** on Albin's call, as a beta that is off by
default: a consumer gets it only by asking for `CPAPDASH_PARSER_WITH_SEFAM=ON`, and
the changelog says plainly that nothing here has met a real card.

That is a decision about *availability*, not about correctness. The donor-card step
still gates everything that matters: nothing leaves `EventType::OTHER`, no `.DET` code
gets a meaning, and no number out of this parser should be shown to a patient as
therapy data until a real card and its SEFAM Analyze export agree with it.

## 9. Shape of the change

Mirrors Löwenstein throughout.

| | |
|---|---|
| Build flag | `CPAPDASH_PARSER_WITH_SEFAM` -> `CPAPDASH_WITH_SEFAM` |
| Header | `include/cpapdash/parser/SefamParser.h` |
| Sources | `src/SefamIni.cpp`, `src/SefamParser.cpp`, `src/SefamParser_Channels.cpp`, `src/SefamParser_Events.cpp` |
| Enum | `DeviceManufacturer::SEFAM` |
| Detection | `DATA_<N>/DATA_<N>.INI`, both `detectManufacturer` overloads |
| Factory | a `SEFAM` case in `createParser` |
| Tests | `tests/test_sefam_ini.cpp`, `tests/test_sefam_channels.cpp`, `tests/test_sefam.cpp` |
| Fixtures | `tests/fixtures/sefam/`, generated by `tests/fixtures/sefam/gen/make_fixtures.py` |
| Docs | `docs/SEFAM_FORMAT.md`, and a Sefam section in `docs/BRAND_RESEARCH.md` |

## 10. Open questions for the donor card

1. What is the header length, and is it the same on every card and firmware?
2. Is the transform a single-byte XOR, or a repeating key?
3. What do the `.DET` codes mean?
4. `Real Record Duration` in the INI need not match the sample count. Which one
   describes the therapy? The parser trusts sample count; the card decides whether
   that is right.
5. Is one session per `DATA_<N>` folder, and are older folders pruned?
6. What are `.NSD` and `.Y17`?
7. Does the S.Box C, DuoS or DuoST write the same layout?
