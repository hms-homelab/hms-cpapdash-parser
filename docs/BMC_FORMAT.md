# BMC / React Health Luna SD card format

What `BmcParser` reads, and where each fact comes from. Every statement is either
**measured** on a real card or taken from **BMC_RESmart** (github.com/headrotor/BMC_RESmart,
MIT licensed, "Copyright (c) 2019 headrotor"), which decodes the older RESmart GII.
Where the two disagree, the measurement wins and the difference is called out.

The measurement cards:

- a BMC **G2S B20A** (an auto bi-level, sold in the US as the React Health Luna family),
  firmware string `G2.1-`, about 6.6 hours of therapy over two nights plus several
  short sessions;
- a BMC **G3 A20** (an auto CPAP), firmware string `G3-2.00.`, one night of about
  3.1 hours plus four short sessions filmed on the machine's own screen (Pressure,
  Flow and Leak, live) from 2.5 to 20 cmH2O and up to 170 L/min of flow.

Both decode with the same framing and the same word positions. Neither card is in this
repository and neither ever will be. The synthetic fixtures in `tests/` are written to
the same layout.

An independent reader (OSCAR 2.0.1, run as a black box, its source not consulted) was
used to confirm the meaning of the fields marked **oracle**. Fields marked **screen**
were read against the G3's own display, frame by frame, with the card's packets
aligned to the video. A public reverse-engineering note, github.com/riaancillie/BmcCpapData
(its format documents `02-nnnfile.md` and `0c-waveforms.md` only), agrees where it is
cited as **corroboration**; it was not the source of any number here.

## Files

All files share one base name, the machine's serial number:

| File | Content |
|---|---|
| `<serial>.000`, `.001`, ... | Raw per-second data, numbered in sequence. |
| `<serial>.evt` | The machine's own event records. |
| `<serial>.idx` | Index; layout not decoded. |
| `<serial>.log` | Log; layout not decoded. The firmware string is ASCII at offset 2060 (`G3-2.00.`..., `G2.1-`...); not read by the parser. |
| `<serial>.USR` | Device and **patient** details. The parser reads one thing from it, the **model string**: ASCII at offset 8854, at most 12 bytes, ended by a zero (`G3 A20`, `G2S B20A`). It reads the file only up to the end of that field; nothing else in it is read, surfaced or logged. |

Other files on the card (display language packs named like `nP2-24.ENG`) are firmware
assets and are ignored.

The serial is not always the `NNCNNNNN` shape BMC_RESmart describes (`16C01034`): the
measurement card's is eight digits. Detection therefore keys on an eight-character
alphanumeric `.usr` whose base name is shared by a `.evt`.

## Raw data: one 256-byte packet per second

Each packet (BMC_RESmart; confirmed on every packet of the measurement card):

| Bytes | Content |
|---|---|
| 0-239 | 120 little-endian `uint16` words |
| 240-247 | 4 more words (unused on this card) |
| 248-249 | year, `uint16` LE |
| 250-254 | month, day, hour, minute, second, one byte each |
| 255 | unknown |

`0xFFFF` in a word means "not valid" (BMC_RESmart; seen on respiratory rate in the first
seconds of a session).

**Packets are not stored in time order.** On the measurement card the machine's clock
was set once, and packets written afterwards sit after packets stamped 2 h 41 min
later. A reader must sort by the timestamp. (OSCAR does not, and loses every session
written after the jump: 1.9 hours of therapy on this card.)

Two packets can carry the same second; the first is kept.

### Words

| Word | Meaning | Source |
|---|---|---|
| 0 | `0xAAAA` sync | BMC_RESmart, measured |
| 1 | **Session number**, the same number the `.evt` records carry | measured; BMC_RESmart calls it "Reslex", which does not hold here |
| 2 | **EPAP**, 0.5 cmH2O per unit | measured + oracle (BMC_RESmart calls it IPAP) |
| 3 | **IPAP**, 0.5 cmH2O per unit | measured + oracle (BMC_RESmart calls it EPAP) |
| 4-28 | **Pressure**, 25 Hz, signed; the scale depends on the model (see "Pressure") | measured on both models, screen (G3); corroboration ("Pressure Wave", no units) |
| 29-53 | 25 Hz channel, **not pressure**, meaning unknown; not read | measured (it moves 1.1 to 2.8 codes per cmH2O and fits no pressure); corroboration guesses snoring |
| 54-78 | **Total flow**, 25 Hz, signed: L/min = (code - 383) / 10 (see "Flow") | measured, screen; corroboration (x10, no zero named) |
| 79-83, 84-88 | two 5 Hz groups, about 8,000 to 30,000, rising with pressure; meaning unknown, not read | measured |
| 98 | **Leak**, 0.1 L/min per unit, capped at 1000 | measured + oracle + screen (BMC_RESmart calls it SpO2) |
| 99 | **Tidal volume**, mL, the last breath's, held until the next | measured (tracks the flow's inspiratory volume, r 0.95 / 0.92 per minute); corroboration |
| 101 | **Minute ventilation**, 0.1 L/min per unit | measured; corroboration |
| 104 | **Respiratory rate**, breaths/min | measured + oracle (BMC_RESmart puts it at word 100) |
| 105 | **I:E as Te/Ti**, x10 (expiration over inspiration) | measured: against the breaths in the flow, Te/Ti fits (r 0.91 / 0.88, median difference 0.05 / 0.00) and Ti/Te does not (r -0.82 / -0.88). The corroborating note reads it the other way round |

Words 99, 101, 104 and 105 are written once per breath, 1 to 2 seconds after it ends.
They are 0 before a session's first breath and while nothing is breathed; the parser
reads 0 in words 99, 101, 104 and 105 as "no reading", not as a value (a short
session of word 104 = 0 is no rate, not a rate of 0). The SpO2/pulse words
BMC_RESmart names hold junk when no oximeter is attached and are not read.

The 25 samples of a channel are in time order, and contiguous from one packet to the
next: the step across a packet boundary is the same size as a step within one.

On the measurement card word 3 is always word 2 + 8 (4 cmH2O of pressure support) and
both move during the night: an auto bi-level.

The parser reports IPAP per minute as `therapy_pressure` and EPAP per minute as
`epr_pressure`, the fields a ResMed AirCurve's IPAP and EPAP use.

### Pressure (words 4-28)

cmH2O = (code - zero) / gain, per model:

| Model | Scale | How measured | Range seen |
|---|---|---|---|
| `G3 A20` | code / 10 | end-expiratory pauses against the set pressure over a night (324 windows, rms 0.18 cmH2O); the machine's own Pressure display (rms 0.45 cmH2O frame by frame; every set-pressure plateau from 5 to 20 within 0.1 cmH2O) | 2.5 to 20 cmH2O |
| `G2S B20A` | (code - 102) / 23.23 | end-expiratory pauses against EPAP over two nights (85 windows, rms 0.08 cmH2O). No screen reading: no G2S was at hand to film | EPAP 4.5 to 10 cmH2O fitted; IPAP to 14 seen |

A zero-offset line fails on the G2S (rms 0.69 cmH2O), and code / 10 would read it
about 24 cmH2O high: the scale really does differ between models. **A model not in
this table gets no pressure waveform**, and neither does a card read without its
`.USR`; each new model needs its own fit first. The per-minute IPAP and EPAP (words 3
and 2) are reported for every model.

The channel is the pressure the machine itself shows as "Pressure", not a copy of the
setting: it swings with the breath (on the G3 about +0.06 cmH2O per L/min of patient
flow, roughly -1.4 in expiration and +1.6 in inspiration at 4 cmH2O). Whether it is
sensed at the outlet or estimated at the mask cannot be told from the card, so it is
not called mask pressure.

A second in which all 25 samples are exactly 0 (seen on the G3 in sessions with the
blower running at a set pressure and nobody breathing) is the machine not writing
the channel, and is read as no pressure, not as 0 cmH2O.

### Flow (words 54-78)

The channel is **total flow**: everything leaving the machine, the mask's vent and any
leak included. Signed 16-bit; negative codes are rare (a few samples a night on the G3).

**Total flow, L/min = (code - 383) / 10.** Measured against the G3's own Flow display,
which shows total flow (with negatives shown as 0.0):

| Session | Free fit (L/min per code, zero) | rms of (code - 383) / 10 |
|---|---|---|
| 4 cmH2O, normal breathing | 0.0966, 371 (block bootstrap 0.091 to 0.100, 345 to 384) | 3.1 L/min |
| 7 cmH2O, breathing, a leak, the hose capped | 0.0990, 382.8 (0.093 to 0.101, 372 to 386) | 1.25 L/min |
| 5 to 20 cmH2O, flow to 170 L/min, leak to 92 L/min | 0.0984, 374 | median residual within 0.3 L/min in every band but one (+0.9 at 110 to 140) |

At gain 0.1 the zero is 382.6, 383.3 and 384.3 in those sessions, and 384.2 on the
steady capped stretch: no drift between 4 and 7 cmH2O. No saturation was seen: the
largest code was 2,105 (172 L/min), with no flat top. The rms figures are mostly
timing on fast swings: the display refreshes about every 0.17 s.

What these show is that the card agrees with the machine's own display. Whether that
display is right in physical units would take a calibration syringe or an in-line
flowmeter; it was not checked. The G2S was not filmed; its nights give the same leak
response (11.5 against 11.6 codes per L/min of leak), the same zero (374 against 369,
fitted from the mean flow) and the same gain from words 99 and 101, so it is read the
same way.

**Patient flow** is total flow minus the leak level, (code - leak level) / 10 L/min,
positive in inspiration. The leak level is the mean flow over each whole breath:
first a centred 12 s mean, breaths found against it (zero crossings with a 2.5 L/min
hysteresis), then each breath's own mean placed at its centre and joined linearly.
Patient flow does not use the zero at all. It is what the parser charts and what its
breath detector reads, the same quantity as a ResMed's flow; the machine's screen shows
total flow, so at a pause the screen reads about the vent flow (some 15 L/min at
4 cmH2O) where the chart reads 0.

On a bi-level the vent passes more during IPAP, which a per-breath leak level counts
as inspiration: integrated patient flow reads Vt about 20 to 25% above word 99 on the
G2S (about 8 to 9% on the CPAP-mode G3). The parser reports the machine's words 99 and 101 for volumes,
not that integral.

### What the parser reports per minute

| Field | From |
|---|---|
| `avg/max/min_flow_rate` | patient flow, L/min |
| `total_leak` | mean total flow, (mean code - 383) / 10: vent plus leak |
| `avg/max/min_pressure` | words 4-28 at the model's scale |
| `tidal_volume`, `minute_ventilation` | words 99 and 101 |
| `respiratory_rate` | word 104 |
| `inspiratory_time`, `expiratory_time`, `ie_ratio` | the breaths found in patient flow; `ie_ratio` is Ti/Te as for every other machine, and a minute with no breath found takes it from word 105 turned round. A breath touching a sample with no flow reading (a second missing from the card, or one at the leak cap) is dropped, not carried across the gap |
| `leak_rate`, `leak_min`, `leak_max` | word 98 |
| `flow_limitation` | not reported: the score depends on the leak level and has not been shown to hold on this machine |

**Unmeasured is never 0.** `pressure_measured` is false for a minute of a model with
no measured scale, or where fewer than half of the minute's seconds carry a pressure
reading; `flow_measured` is false where fewer than half carry flow, which includes
every second with the leak at the 1000 cap (the mask is off: what flows is not
breathing). The fields are then 0 placeholders and every session figure leaves them
out. A missing word 99 or 101 leaves `tidal_volume` or `minute_ventilation` empty.

Each breath in `breaths` carries the machine's word 99 for that breath (the first new
value in the three seconds after it ends) as its tidal volume, or the integral when
the word has none. `BmcParser::waveform` returns the 25 Hz patient flow and pressure
of any stretch of a card, calibrated the same way, for a view zoomed into a night.

### Sessions

Packets are grouped into sessions by time: a gap of more than 60 seconds between
consecutive stamps starts a new session. Duration is the count of packets, one second
each.

## Events (`.evt`)

After a 2048-byte region (a 128-byte header shared with `.idx` and `.log`, then `0xFF`
fill), 32-byte records:

| Bytes | Content |
|---|---|
| 0-1 | `0xAAAA` |
| 2-3 | session number (word 1 of the raw packets) |
| 4-5 | type |
| 6-7 | zero |
| 8-11 | onset, `uint32` LE, **seconds since noon of the session's sleep day** |
| 12-15 | duration, `uint32` LE, seconds |
| 16-31 | mostly zero; meaning unknown |

The sleep day of a session number is the date of its first packet (by time), or the day
before if that packet is before noon. Every apnea-length record of the measurement card
falls inside a recorded session under this anchor.

| Type | Meaning | Source |
|---|---|---|
| 1 | Clear airway (central) apnea | oracle: 16 of 16 on the night checked |
| 2 | Obstructive apnea | oracle: 26 of 26, and the single one of a second session to the minute |
| 4, 5, 7 | spans of minutes to hours; meaning unknown | measured |
| 8, 9, 10 | span a whole session; meaning unknown | measured |

The index is reported as an AHI (`IndexKind::AHI`): the machine classifies its own
apneas, and on the night the independent reader also scored, the two indices are equal
(9.61). No hypopnea type appeared on the measurement card; a record of a type not listed
here is counted in the parser's notes rather than dropped, so one would be visible.

The span types (4, 5, 7-10) are not emitted as events: they are periods of state, not
annotations, and counting hours-long records as events would wreck every event
statistic. They are counted in the parser's notes instead.

## The header shared by `.idx`, `.evt`, `.log`

128 bytes: eight spaces, zeros, a file-type code at offset 0x20 (`.idx` 1, `.log` 4,
`.evt` 6), the serial as ASCII at 0x34. Not otherwise interpreted.
