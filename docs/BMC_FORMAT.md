# BMC / React Health Luna SD card format

What `BmcParser` reads, and where each fact comes from. Every statement is either
**measured** on a real card or taken from **BMC_RESmart** (github.com/headrotor/BMC_RESmart,
MIT licensed, "Copyright (c) 2019 headrotor"), which decodes the older RESmart GII.
Where the two disagree, the measurement wins and the difference is called out.

The measurement card: a BMC G2S B20A (an auto bi-level, sold in the US as the React
Health Luna family), firmware string `G2.1-`, about 6.6 hours of therapy over two nights
plus several short sessions. The card itself is not in this repository and never will
be. The synthetic fixtures in `tests/` are written to the same layout.

An independent reader (OSCAR 2.0.1, run as a black box, its source not consulted) was
used to confirm the meaning of the fields marked **oracle**.

## Files

All files share one base name, the machine's serial number:

| File | Content |
|---|---|
| `<serial>.000`, `.001`, ... | Raw per-second data, numbered in sequence. |
| `<serial>.evt` | The machine's own event records. |
| `<serial>.idx` | Index; layout not decoded. |
| `<serial>.log` | Log; layout not decoded. |
| `<serial>.USR` | Device and **patient** details. Never surfaced or logged by the parser. |

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
| 4-28 | 25 Hz channel, pressure-related; physical scale unknown | BMC_RESmart |
| 29-53 | 25 Hz channel, pressure-related; physical scale unknown | BMC_RESmart |
| 54-78 | 25 Hz flow; OSCAR draws it as code / 10 with no zero offset removed | BMC_RESmart, oracle |
| 98 | **Leak**, 0.1 L/min per unit, capped at 1000 | measured + oracle (BMC_RESmart calls it SpO2) |
| 104 | **Respiratory rate**, breaths/min | measured + oracle (BMC_RESmart puts it at word 100) |
| 105 | **I:E ratio**, x10 | measured + oracle |

Tidal volume, minute ventilation and inspiratory/expiratory time match no word: an
independent reader derives them from the flow channel. The SpO2/pulse words BMC_RESmart
names hold junk when no oximeter is attached and are not read.

On the measurement card word 3 is always word 2 + 8 (4 cmH2O of pressure support) and
both move during the night: an auto bi-level.

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

No hypopnea type appeared on the measurement card, so a BMC session's index is apneas
only. The parser marks it `IndexKind::Ungraded` until a card shows the hypopnea record.

The span types (4, 5, 7-10) are not emitted as events: they are periods of state, not
annotations, and counting hours-long records as events would wreck every event
statistic. They are counted in the parser's notes instead.

## The header shared by `.idx`, `.evt`, `.log`

128 bytes: eight spaces, zeros, a file-type code at offset 0x20 (`.idx` 1, `.log` 4,
`.evt` 6), the serial as ASCII at 0x34. Not otherwise interpreted.
