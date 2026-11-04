# Philips Respironics PRS1 On-Card Format (clean-room transcription)

**Companion to:** `sdlc/sdd/002-philips-prs1-support.md` (SDD-002). This document is the ONLY format
reference the PhilipsParser implementation may code against.

## Provenance

Every fact below carries a citation. Sources:

- **[A #section]** Apnea Board wiki, "PR System One Data Format",
  https://www.apneaboard.com/wiki/index.php/PR_System_One_Data_Format (retrieved 2026-07-07,
  transcribed by hand; the page documents a lawful reverse-engineering effort and contains
  format facts only, no source code).
- **[B file:Lx-y]** cpap-lib by Jonah Stagner (StagPoint), MIT license,
  https://github.com/EEGKit/cpap-lib pinned at commit `f760b21205e0d3f49342eacf2eed11912881aa01`.
  Files cited: `cpap-lib/PRS1DataLoader.cs`, `cpap-lib/Calculations/CRC.cs`,
  `cpaplib_tests/PRS1_tests.cs`. Attribution in `THIRD_PARTY.md`.
- **[S fixture @offset]** Observed in sample data (added as real cards arrive; none yet).

Forbidden source reminder: OSCAR / SleepyHead code was NOT consulted for any fact in this
document and must never be. The wiki page is authored by the OSCAR documentation team but
contains only format description; where it defers to their code ("see sleepyhead code"), that
gap is filled from [B] or marked OPEN, never from their code.

Scope note: v1 targets Family 0 (CPAP/APAP/BiPAP), System One + DreamStation 1.
DataFormatVersion 3 files (DreamStation 2) are out of scope and must be rejected cleanly.

---

## 1. Card layout and session discovery

- Card root contains a `P-SERIES` folder [B PRS1DataLoader.cs:L19, PRS1_tests.cs:L22].
- Inside it, one folder named by the machine serial number (e.g. `P1192913945CE`)
  containing `Properties.txt` and patient data folders `P0`, `P1`, ... `Pn`
  [B PRS1_tests.cs:L22-23,L163-174]. The active folder index is the `PatientFolderNum`
  key of Properties.txt (folder name = `P` + value) [B PRS1DataLoader.cs discovery + PRS1_tests.cs:L167].
- Session data files are named by a zero-padded 10-digit session sequence number plus a
  numeric extension: `0000000003.001`, `0000000003.002`, `0000000003.005`
  [B PRS1DataLoader.cs:L362-363,L380-381]. The filename stem equals the session sequence
  number stored in each block header; the internal value is authoritative [A #PRS1-Data-File-Format].
- Extension meaning [A #PRS1-Data-File-Format]:

  | Ext | Content |
  |---|---|
  | `.000` | Compliance-only data from "brick" machines (250P and below); ~44 bytes; usage time, pressure, c-flex, humidifier. Not therapy data. |
  | `.001` | Session summary (times, settings) |
  | `.002` | Event data |
  | `.004` | Short file, partially understood; contains two 32-bit timestamps [A #.004-files] |
  | `.005` | Waveform data |

- Import strategy: `.001` files drive discovery; the matching `.002`/`.005` are looked up by
  the same stem and are each optional [B PRS1DataLoader.cs:L298,L362-407].
- One `.001` file can contain multiple mask-on/mask-off sub-sessions; see section 5
  [A #Summary-Data-.001-files] [B PRS1DataLoader.cs:L1244-1248].

## 2. Properties.txt

- Plain text, one `key=value` per line, split on `=`, reading stops at the first empty line
  [B PRS1DataLoader.cs:L773-795]. Filename match must be case-insensitive (`properties.txt`
  observed) [B PRS1DataLoader.cs:L763].
- Keys used: `SerialNumber`, `ModelNumber`, `Family`, `FamilyVersion`, `DataFormatVersion`,
  `FirstDate`, `LastDate` (unix), `PatientFolderNum`, `PatientFileNum`, `ProductType`
  [B PRS1_tests.cs:L117-167].
- ModelNumber to product name table, ported verbatim from [B PRS1DataLoader.cs:L21-90]
  (MIT, attributed). Selected entries; the implementation carries the full 67-entry table:

  | ModelNumber | Product |
  |---|---|
  | 560P / 560PBT / 561P / 562P | REMstar Auto (System One 60 Series) |
  | 550P / 551P / 552P | REMstar Auto (System One) |
  | 460P / 461P / 462P | REMstar Pro (System One 60 Series) |
  | 500X110..500X180 | DreamStation Auto CPAP |
  | 400X110..400X150 | DreamStation CPAP Pro |
  | 700X110..700X150 | DreamStation Auto BiPAP |
  | 900X110/120/150 | DreamStation BiPAP autoSV |
  | 410X150C / 420X150C | DreamStation 2 (DETECT ONLY, format v3, rejected) |

## 3. Chunk container (all file types)

A PRS1 data file is a sequence of one or more blocks ("chunks"). Each block:
15-byte standard header, optional extra header (by FileType), 8-bit header checksum,
data area, 16-bit data checksum [A #PRS1-Data-File-Format]. Only waveform files have been
observed with multiple blocks [A #PRS1-Data-File-Format], but the reader must loop
until EOF for all types [B PRS1DataLoader.cs:L369-393].

### 3.1 Standard 15-byte header

All multi-byte integers little-endian [B PRS1DataLoader.cs:L1509-1516 uses BinaryReader LE].

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0x00 | 1 | DataFormatVersion | Must be 2 for this parser; 3 = DreamStation 2, reject [A #Standard-PRS1-Header-Example] [B L1524-1527,L875-899] |
| 0x01 | 2 | BlockLength | Bytes, including header and checksums [A] |
| 0x03 | 1 | FileType (HeaderType) | 0 = standard, 1 = signal/interval data [A #Known-FileType-values] [B L845-850] |
| 0x04 | 1 | Family | 0 = CPAP/APAP/BiPAP line; 5 = ASV (v2 scope) [A] |
| 0x05 | 1 | FamilyVersion | Event/summary schema selector; see section 4 preamble [A] [B L1519-1522] |
| 0x06 | 1 | FileExtension | Matches the file's numeric extension [A] |
| 0x07 | 4 | SessionSequenceNumber | Equals filename stem [A] [B L1515] |
| 0x0b | 4 | Timestamp | Unix epoch seconds [A]; [B L1517] converts via UnixEpoch.AddSeconds().ToLocalTime(). Timezone semantics OPEN, verify on real card (section 8 Q1) |

- FileType 0: no extra header; the 8-bit checksum immediately follows at 0x0f [A #Known-FileType-values].
- FileType 1: extra header follows (section 3.2). The 1160P (AVAPS) stores even its `.002`
  event data as FileType 1 fixed-interval records [A #Known-FileType-values]; out of v1 scope.

### 3.2 Signal extra header (FileType 1, .005 files)

| Offset | Size | Field |
|---|---|---|
| 0x0f | 2 | Number of interval records [A #Waveform-Data-Header] [B L1543] |
| 0x11 | 1 | Seconds per interval record [A] [B L1544] |
| 0x12 | 1 | Number of signals (nsig) [A] [B L1545] |
| then | 3*nsig | Per signal: 1 byte sample format/kind + 2 bytes interleave [A] [B L1553-1563] |
| then | 1 | Terminator byte, always 0 [A] [B L1565-1567] |

Block duration in seconds = interval records * seconds per interval [B L1454-1457].

### 3.3 Checksums

- **Header checksum** (1 byte, after all header bytes): 8-bit additive sum of every header
  byte before it [A #Known-FileType-values] [B PRS1DataLoader.cs:L1575-1585, CRC.cs Checksum8:
  `checksum += data[i]` over the header bytes].
- **Data checksum** (2 bytes after the data area): CRC-16/Kermit, i.e. reflected CRC-CCITT,
  polynomial 0x1021 in reflected form 0x8408, init 0, no final xor; table-driven update
  `crc = table[(byte ^ crc) & 0xFF] ^ (crc >> 8)` computed over the data area only
  [B CRC.cs CRC16 table (first entries 0x0000, 0x1189, 0x2312...) + Calc loop;
  B PRS1DataLoader.cs:L880-888].
- DataFormatVersion 3 uses a 4-byte data checksum and a different container; reject
  [B PRS1DataLoader.cs:L875,L893-899].

### 3.4 Data area size

`DataSize = BlockLength - headerSize - headerChecksum(1) - dataChecksum(2)` where headerSize
is 15 for FileType 0 and 15 + 4 + 3*nsig for FileType 1 (equivalently: BlockLength-18, or
BlockLength-18-(nsig*3) minus the extra 4 fixed signal-header bytes; use the
position-derived form like [B L879-880] rather than hardcoded constants)
[A #Calculating-just-the-size-of-a-blocks-data-area] [B PRS1DataLoader.cs:L879-881].

## 4. Event data (.002), Family 0

Events are stored after the standard header as a stream of records:
1-byte event code, 16-bit time delta (seconds, advances a running clock that starts at the
header timestamp), then code-specific fields [A #Event-Data-Format] [B PRS1DataLoader.cs:L914-916].

**The schema is selected by (DataFormatVersion, Family, FamilyVersion); it is not stored in
the file** [A #Event-Data-Format]. The two sources document two different Family 0 code
tables, and both are transcribed below. [B] hard-requires FamilyVersion 4 (its DreamStation-
era sample) [B L1519-1522]; the [A] table matches the earlier System One firmware
generation (FamilyVersion not stated on the page; presumed 2/3). The parser selects the
table by FamilyVersion: **V4+ = table 4.2; below V4 = table 4.1 (provisional until a real
System One card confirms; see section 8 Q2).**

### 4.1 Family 0 code table per [A #Family-0-Codes] (System One generation)

| Code | Meaning | Fields after delta | Notes |
|---|---|---|---|
| 0x01 | Unknown | none | |
| 0x02 | Pressure | 1 byte | value / 10.0 cmH2O |
| 0x03 | BiPAP pressures | EPAP 1 byte, IPAP 1 byte | each / 10.0 |
| 0x04 | Pressure pulse | 1 byte | |
| 0x05 | RERA | offset 1 byte | offset semantics below |
| 0x06 | Obstructive apnea | offset 1 byte | |
| 0x07 | Clear airway | offset 1 byte | |
| 0x0a | Hypopnea | offset 1 byte | |
| 0x0c | Flow limitation | offset 1 byte | |
| 0x0d | Vibratory snore | none | |
| 0x0e | Unknown | 3 bytes | |
| 0x0f | CSR / periodic breathing | duration 2 bytes, offset 1 byte | |
| 0x11 | Graph data | leak 1 byte, snore 1 byte | |
| 0x12 | Summary? | 4 bytes | partially understood |

Offset semantics [A #Family-0-Codes]: delta advances the running clock BEFORE the event is
placed; the per-event "offset" is subtracted for that event only and does not adjust the
running clock.

### 4.2 Family 0 FamilyVersion 4 code table per [B PRS1DataLoader.cs:L918-1153] (DreamStation generation)

Running clock: `timestamp += u16 delta` read immediately after each code [B L916].

| Code | Meaning | Fields after delta | Event placement |
|---|---|---|---|
| 0x01 | Pressure set | 1 byte, / 10.0 | stat at clock |
| 0x02 | Pressure set (bilevel) | pressure 1 byte, EPAP 1 byte, each / 10.0 | stats at clock |
| 0x03 | Opti-Start pressure change? | 1 byte, skipped | |
| 0x04 | Pressure pulse | 1 byte, skipped | |
| 0x05 | RERA | elapsed 1 byte | start = clock - elapsed, duration 0 |
| 0x06 | Obstructive apnea | elapsed 1 byte | start = clock - elapsed, duration 0 |
| 0x07 | Clear airway | elapsed 1 byte | start = clock - elapsed, duration 0 |
| 0x0A | Hypopnea (type 1) | elapsed 1 byte | start = clock - elapsed, duration 0 |
| 0x0B | Hypopnea (type 2) | duration 1 byte, elapsed 1 byte | start = clock - elapsed, duration as read |
| 0x0C | Flow limitation | elapsed 1 byte | start = clock - elapsed, duration 0 |
| 0x0D | Vibratory snore | none | start = clock, duration 0 |
| 0x0E | Variable breathing | duration u16 * 2 sec, elapsed 1 byte | start = clock - elapsed - duration |
| 0x0F | Periodic breathing | duration u16 * 2 sec, elapsed 1 byte | start = clock - elapsed - duration |
| 0x10 | Large leak | duration u16 * 2 sec, elapsed 1 byte | start = clock - elapsed - duration |
| 0x11 | Statistics | leak 1 byte, snore 1 byte, EPAP 1 byte / 10.0 | stats at clock |
| 0x12 | Snore-related stats | u16, skipped | |

**Onset conversion (both tables):** PRS1 records apnea-family events at their END (the
elapsed/offset byte is the seconds back to the start). Our unified model timestamps the
ONSET (SDD-001 F1 invariant), so the parser computes onset = clock - elapsed(- duration
where applicable) exactly as shown above and stores the duration separately. For codes
where [B] emits duration 0 (RERA/OA/CA/H type 1/FL), the elapsed value is back-distance,
NOT the event duration; do not store elapsed as duration.

Unknown codes: the file gives no lengths, so an unknown code makes the rest of that block
unparseable. Count it (`unknown_event_codes`), log the code once, and abandon THAT BLOCK
(not the session); [B L1151-1152] throws instead, we must not.

## 5. Summary data (.001), Family 0

[A #Summary-Data-.001-files] documents only: stored after standard header; contains totals,
pressure settings, machine settings; typically ~77 bytes; multiple sessions per file;
structure messy; "a non zero value in the 6th data position inserts 4 bytes directly
afterwards". The layout below is from [B PRS1DataLoader.cs:L1163-1444], FamilyVersion 4.
**Sub-V4 `.001` layout is OPEN (section 8 Q3); the parser must length-check and bail to
skip-with-warning on violation.**

Slot-coded stream, same running-clock idea as events: 1-byte code, then code-specific body
[B L1172-1242]:

| Code | Meaning | Body |
|---|---|---|
| 0x00 | Equipment on | settings block, 24 bytes (below) |
| 0x01 | Equipment off | delta u16 (advances clock), 5 bytes skipped |
| 0x02 | Mask on | delta u16, 3 bytes skipped, humidifier 2 bytes; clock value = mask-on time |
| 0x03 | Mask off | delta u16; emit sub-session [mask-on, clock]; 34 bytes skipped |
| 0x04 | Time correction? | delta u16 only |
| 0x05, 0x06 | Unknown | no body observed |
| 0x07 | Humidifier change | delta u16, humidifier 2 bytes |
| 0x08 | CPAP-check related? | delta u16, 9 bytes skipped |

Session assembly: session start = first mask-on, end = last mask-off; intermediate mask
on/off pairs are sub-sessions merged into one [B L1170-1248]. Multiple mask-off events
without matching mask-on (or vice versa) = malformed, skip-with-warning.

### 5.1 Settings block (code 0x00 body, 24 bytes) [B PRS1DataLoader.cs:L1258-1316]

| # | Field | Notes |
|---|---|---|
| 1 | unknown 1 byte | |
| 2 | operating mode 1 byte | 0x00 CPAP, 0x20 bilevel fixed, 0x40 APAP, 0x60 bilevel auto variable PS, 0x80 APAP, 0xA0 CPAP [B L1424-1444] |
| 3 | min pressure 1 byte | * 0.1 cmH2O |
| 4 | max pressure 1 byte | * 0.1 |
| 5 | min PS 1 byte | * 0.1 |
| 6 | max PS 1 byte | * 0.1 |
| 7 | startup mode 1 byte | |
| 8 | ramp time 1 byte | minutes |
| 9 | ramp pressure 1 byte | * 0.1 |
| 10 | flex flags 1 byte | bit7 enabled, bit6 locked, bit5 plain flex, bit4 rise time, bit3 plus mode, bits0-1 level [B L1357-1422] |
| 11-12 | humidifier 2 bytes | flags1: bits0-2 humidity level, bits4-6 tube humidity, bit7 = tube temp low bit; flags2: bits0-1 tube temp high bits, bit2 adaptive, bit3 heated tube, bit4 no-humidifier [B L1333-1355] |
| 13 | resistance flags 1 byte | bits3-5 mask resistance level, bit6 resistance lock, bit0 hose diameter (1=15mm else 22mm), bit1 tube temp lock [B L1274-1278] |
| 14 | unknown 1 byte (=1) | |
| 15 | general flags 1 byte | bit6 auto-on, bit4 auto-off, bit2 mask alert, bit1 show AHI [B L1280-1284] |
| 16 | unknown 1 byte (=0) | |
| 17 | auto-trial duration 1 byte | |
| 18-24 | reserved 7 bytes, all 0 | [B L1315-1316] |

CPAP single pressure = min pressure if non-zero else max pressure [B L1266].

For the unified model: mode maps to `DeviceSettings.therapy_mode` (0=CPAP, 1=APAP, 2=BiPAP);
min/max/set pressure, ramp_time, ramp_pressure, flex_mode/flex_level, humidifier_mode/level,
tube_temp, hose_diameter map directly; the rest is ignored in v1.

Pressure tick size: CONFIRMED 0.1 cmH2O across settings and events
[A #Family-0-Codes "divide by 10"] [B SCALE=0.1 L854].

## 6. Waveform data (.005)

- FileType 1 blocks; per-block signal table in the extra header (section 3.2).
- Family 0 `.005` observed with **one signal: flow** [B PRS1DataLoader.cs:L473-475 asserts
  nsig==1]. If nsig > 1, data is interleaved in groups of `interleave` samples per signal in
  header order, repeating [A #Waveform-Data-Structure]; **v1 policy (SDD decision 2): if
  nsig > 1, decode the interleave structure only to EXTRACT flow and skip other signals.**
  Which signal is flow when nsig > 1 is OPEN (section 8 Q4); with nsig == 1 it is flow.
- Samples: 8-bit signed, value is flow in L/min in range [-127, 127]
  [B BuildFlowSignal L687-705 casts to sbyte, unit "L/min"]. Only 8-bit samples have been
  observed [A #Waveform-Data-Structure].
- Sample interval: 0.2 s (5 Hz) [A "only 5hz" #Data-format-analysis]
  [B SAMPLE_FREQUENCY=0.2 L460, sample-rate assert L529-531]. Derive per block as
  `blockDuration / DataSize` and verify near 0.2 rather than hardcoding.
- Multiple blocks per file. Consecutive blocks may have a time gap = machine-flagged "no
  breathing detected" period [B L480-508]. Our policy: no zero-fill, gaps simply produce no
  samples/summaries (differs from [B] which zero-fills; recorded as deliberate deviation).
  Small negative overlaps (< 1 s) are tolerable clock jitter [B L517-522]; a grossly
  negative first-chunk timestamp means a corrupt block: skip block, count warning
  [B L509-516 discards whole session; we keep the rest].

## 7. Anonymization

The serial number appears in the FOLDER NAME and inside `Properties.txt` only. Neither the
standard header (section 3.1), the signal header (3.2), the event records (4), nor the
summary records (5) contains a serial field [A all header/record tables] [B HeaderRecord
L1466-1539]. Rewriting Properties.txt `SerialNumber` and renaming the folder is therefore
sufficient for fixture anonymization. To be re-verified against a full real card sweep
before the first donated fixture is committed [S pending].

## 8. Open questions / discrepancy log

1. **Timestamp timezone.** [B L1517] treats header timestamps as UTC unix epoch and converts
   to local; whether the machine writes true UTC or local-as-epoch is unverified. Decide on
   real-card evidence (compare with the card owner's known therapy times); until then treat
   as unix epoch UTC.
2. **Which FamilyVersions use table 4.1 vs 4.2.** [B] proves V4 = table 4.2. The wiki table's
   FamilyVersion is unstated (presumed 2/3, System One). A real System One card must confirm
   before sub-V4 event parsing is enabled; until confirmed, sub-V4 `.002` files are counted +
   skipped, not guessed.
3. **Sub-V4 `.001` layout.** Wiki gives only the "6th data position inserts 4 bytes" hint.
   Same gating as Q2: skip-with-warning until a real card provides evidence.
4. **Flow signal position when nsig > 1.** Not documented in either source (only nsig == 1
   observed). Until real evidence, if nsig > 1 take the signal whose derived sample rate is
   ~5 Hz, else count + skip the block.
5. **Discrepancy: event tables differ between sources** (e.g. 0x02 = Pressure in [A] vs
   bilevel-pressure in [B]; 0x0f = CSR in [A] vs Periodic Breathing in [B]; [A] has no 0x10
   Large Leak). Explained as firmware-generation schema difference keyed by FamilyVersion;
   this is Q2's confirmation target.
6. **Discrepancy: gap handling.** [B] zero-fills inter-chunk gaps and emits a synthetic
   "breathing not detected" event; we do neither (no zero-fill, no synthetic event) per SDD
   simplicity. Revisit if consumers want explicit gap flags.
7. **`.004` files** are unparsed in v1 (two timestamps, purpose unclear [A #.004-files]).
