# Sefam S.Box card format

Companion to `sdd/005-sefam-sbox-support.md`.

**Source: a donor card from hms-homelab/hms-cpap#28**, an S.Box AUTO, model code
`1263R`, 242 sessions spanning 2025-11-10 to 2026-09-05. Everything marked CONFIRMED
below was measured on it and nothing was transcribed from anyone else's document; the
provenance rules at the end still bind.

The parser reads 241 of the 242 sessions. The one refusal is a session in which every
declared channel is a stub, which is to say a session with nothing in it.

| Status | Meaning |
|---|---|
| `CONFIRMED` | measured on the donor card, or stated in vendor documentation |
| `SUPPORTED` | the reading the card's data supports and the alternatives contradict, but not proven |
| `UNKNOWN` | open, listed so it does not get quietly assumed |

## Card layout — CONFIRMED

```
<card root>/
  1263R/                          model code
    24337476/                     device serial
      24337476.RAM   2,097,307 bytes   device state, ignored
      24337476.BKP   2,097,307 bytes   device state, ignored
      DATA_0/ ... DATA_241/       one session each
        DATA_0.INI                the schema
        DATA_0.FLW                one file per channel, extension = channel name
        DATA_0.PRE
        ...
```

Both device-state files are exactly 2,097,307 bytes. 242 session folders, numbered
without gaps.

## The session INI — CONFIRMED

Plain Windows INI, **CRLF**, `[Section]` and `key=value`. Values carry trailing spaces
(`Created By=S.Box_AUTO `), so they must be trimmed, and one of them carries a colon
that is not a separator (`Version=VER :A020400`).

Sections: `[Create Info]`, `[Oximeter]`, `[BLE Device]`, `[Start Record]`, then
`[Chan0]`..`[Chan10]`.

`[Start Record]` states the recording start as six separate numbers, and the duration
fields alongside it are **not** the recording length — see Timing.

Every session declares the same eleven channels:

| Ext | Type | Unit | Min | Max | Freq | Bit |
|---|---|---|---|---|---|---|
| `FLW` | 4 | lpm | -180 | 280 | 25 | 8 |
| `PRE` | 11 | cmH20 | 0 | 255 | 5 | 8 |
| `LK` | 4 | lpm | 0 | 153 | 1 | 8 |
| `DET` | 13 | | 0 | 255 | 25 | 8 |
| `SPO` | 16 | % | 0 | 255 | 1 | 8 |
| `HRT` | 17 | bpm | 25 | 280 | 1 | 8 |
| `PLS` | 13 | | 0 | 65535 | 75 | 16 |
| `STS` | 13 | | 0 | 255 | 5 | 8 |
| `THO` | 13 | | 0 | 255 | 10 | 8 |
| `ABD` | 13 | | 0 | 255 | 10 | 8 |
| `POS` | 13 | | 0 | 255 | 1 | 8 |

`Type` is a per-quantity kind code, distinct from the name. `NSD` and `.Y17` files are
present in every session and declared in none; both are the size of a 25 Hz channel.

On this card `SPO`, `HRT`, `PLS`, `STS`, `THO`, `ABD` and `POS` are stubs in all 242
sessions: no oximeter, no belts, no position sensor, and nothing recorded to the status
channel.

## The 38-byte file header — CONFIRMED

Every channel file, stubs included, opens with the same 38 bytes. **Every byte XOR
`0xBF` yields printable ASCII**, in all 242 sessions:

```
#02/1263R24337476       /251110222516/
```

`#02` then the device identity, padded with spaces, then `YYMMDDhhmmss`. The identity
matches the INI's `Serial Number` and the timestamp matches `[Start Record]` in **all
242 sessions**, so the header is a free cross-check that an INI and a pile of channel
files belong together.

This is also the whole of the "scrambling". The padding is a run of `0x9F` bytes,
because `0x9F ^ 0xBF` is a space, and a 38-byte noisy lead-in shared by every file on
the card is what makes the format look XOR-obfuscated from the outside. **The samples
themselves are plain.**

## Block framing — CONFIRMED

After the header the samples run in blocks of **ten seconds**, whatever the channel
rate: 250 samples on `FLW`, 50 on `PRE`, 10 on `LK`. Each block is followed by three
bytes:

```
[ sum of the block's bytes mod 256 ][ uint16 big-endian block index ]
```

The final, short block is framed the same way.

**Across the donor card that is 838,440 blocks with zero failing checksums.** It is the
reason to trust anything this parser says about a Sefam card: a misread offset or a
misread rate cannot pass quietly.

Two details that cost real nights if assumed away:

- **The index does not always start at 1.** Two sessions open at 2, with every checksum
  correct. Continuity is worth checking; the starting value is not.
- **A short run at the end of a file is ambiguous** — a legitimate final block, or a
  write cut off mid-block. The checksum tells them apart.

A declared channel that was never recorded is written as the header and nothing else:
**38 bytes exactly**.

## Physical values — SUPPORTED, not confirmed

**The INI's Min/Max do not scale a channel.** Reading `FLW`'s declared -180..280 as a
quantisation of 8 bits puts its centre at code 99.8; the card's flow sits at mid-scale.
Reading `PRE`'s declared 0..255 literally puts a patient on 110 cmH2O.

| Channel | Reading | Evidence |
|---|---|---|
| `PRE` | code / 10 cmH2O | a night averages 8.35 and peaks at 16.3; across all 241 readable sessions the averages run 0.5 to 12 |
| `LK` | code / 10 L/min | 9.03 L/min mean; the declared range gives 54 L/min, which would alarm all night |
| `FLW` | code - 128 | see below |
| `SPO`, `HRT` | code, untouched | stubs in every session; nothing to calibrate against |

The flow offset is the weaker claim. Flow is measured at the blower, so it carries the
mask's intentional leak and a night's mean is not expected to be zero — but it should
be small against a signal that swings roughly ±70. It is: over the 40 nights of four
hours or more, the per-night mean sits between **-14.6 and +14.9** and averages
**-2.7**. Mid-scale is the right neighbourhood; it is not pinned to the code.

The flow *factor* has no evidence at all. Nothing in the data distinguishes L/min from
any multiple of it.

All of this is what SDD-005's oracle step exists for: a SEFAM Analyze export of the
same nights settles every row above in one pass.

## The detection channel — UNKNOWN, and deliberately unread

`DET` is **a bitfield of concurrent flags, not an enumeration of events**. Over one
6h24m night it takes 85 distinct values, decomposing into eight independent bits. Mean
share of a recording, across the card:

| bit | share | | bit | share |
|---|---|---|---|---|
| 0 (0x01) | 18.44% | | 4 (0x10) | 3.06% |
| 1 (0x02) | 58.40% | | 5 (0x20) | 38.25% |
| 2 (0x04) | 3.14% | | 6 (0x40) | 13.24% |
| 3 (0x08) | 0.22% | | 7 (0x80) | 31.22% |

A bit set for 58% of the night is not an apnea. It is far more likely a breath phase or
a device state, and the bit set for 0.22% might well be an event — but "might well be"
does not belong in a therapy record. Reading runs of the whole byte, which the first
cut of this parser did, would have produced tens of thousands of events a night out of
flags toggling against each other.

So the parser emits **no events**, reports the per-bit shares, and a Sefam session
carries no AHI. Decoding the bits needs a SEFAM Analyze report for the same nights.

## Timing — CONFIRMED

Session start comes from `[Start Record]`, cross-checked against the header stamp.

**Duration comes from the sample count, never from the INI.** Every one of the 242
sessions declares `Programmed Record Duration=28800` and `Real Record Duration=28800`,
eight hours. The actual recordings run from **2 minutes to 6h24m**. Believing the
declaration would put an eight-hour denominator under every index on the card — the
same failure that reported an 11h22m night as therapy when 3h42m had been delivered
(support ticket 87).

## Still open

| | |
|---|---|
| The physical scale of every channel | needs a SEFAM Analyze export |
| What the `DET` bits mean | needs the same |
| `NSD` and `.Y17` | undeclared, present in every session, 25 Hz |
| `.RAM` / `.BKP` | 2,097,307 bytes of device state |
| Byte order of a 16-bit channel | `PLS` is a stub in all 242 sessions |
| Whether S.Box C, DuoS or DuoST write the same layout | one card, one model |
| What the `Type` code enumerates | 4, 11, 13, 16, 17 seen |

## Provenance

Two prior write-ups of this format exist. One is **GPL-3.0**
(`garett09/OSCAR-V2`, `Notes/loaders/SD_CARD_FINGERPRINTS.md`), one has **no license at
all** (`ChrisAylen/sefam-nea-analysis`). This library is MIT. Neither may be
transcribed, quoted, or ported, and neither was: they were read once for orientation
before the card arrived, and everything above was measured on the card.

Worth recording that the card contradicts both of them on the central point. Both
concluded the sample data was XOR-scrambled; it is not. The obfuscation is confined to
the 38-byte ASCII header, and the shared noisy lead-in they took for evidence of a
card-wide cipher is that header, repeated in every file because every file carries it.
