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
- **[C section]** mattjoyce/dreamstation, Apache-2.0,
  https://github.com/mattjoyce/dreamstation pinned at commit
  `268d7864ddd23504ea38c0ed92bcbc992f1d20f8`, file `docs/FORMAT.md` only (prose and tables; its
  code was not read). Used for section 9 (DataFormatVersion 3, FamilyVersion 6). Its author
  states that field meanings were learned partly by reading OSCAR's loader and that nothing of
  it was copied. Every [C] fact used in section 9 is checked against [S] before it is relied
  on, and section 9 says where a [C] fact is NOT confirmed. Attribution in `THIRD_PARTY.md`.
- **[S]** Measured on a real card. Section 9's card: a DreamStation 2 Auto CPAP, ModelNumber
  `520X110C`, software `V1.0.2.3590`, 1,897 sessions, files decrypted by the card's owner.
  The same physical SD card also holds a DreamStation 1 Auto CPAP (`500X110`) on v3 firmware,
  150 sessions, unencrypted; it is used where section 9 or section 8 says so. No identifier
  from either machine appears in this document or in any fixture.

Forbidden source reminder: OSCAR / SleepyHead code was NOT consulted for any fact in this
document and must never be. The wiki page is authored by the OSCAR documentation team but
contains only format description; where it defers to their code ("see sleepyhead code"), that
gap is filled from [B] or marked OPEN, never from their code.

Scope note: Family 0 (CPAP/APAP/BiPAP). DataFormatVersion 2: System One + DreamStation 1
(sections 1-8). DataFormatVersion 3 with FamilyVersion 6: DreamStation 2, and DreamStation 1
on later firmware (section 9). Encrypted DreamStation 2 files are out of scope: the parser
reads decrypted files only.

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
  | 410X150C / 420X150C | DreamStation 2 (format v3, section 9) |
  | 520X110C | DreamStation 2 Auto CPAP (format v3; ModelNumber from [S] `PROP.BIN`, not in [B]'s table) |

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
| 0x00 | 1 | DataFormatVersion | 2 for sections 3-6; 3 is a different container, section 9 [A #Standard-PRS1-Header-Example] [B L1524-1527,L875-899] |
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
- DataFormatVersion 3 uses a 4-byte data checksum and a different container
  [B PRS1DataLoader.cs:L875,L893-899]; see section 9.

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
| 0x11 | Graph data | leak 1 byte (total leak, see 4.2), snore 1 byte | |
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
| 0x11 | Statistics | total leak 1 byte (L/min), snore 1 byte, EPAP 1 byte / 10.0 | stats at clock |
| 0x12 | Snore-related stats | u16, skipped | |

**0x11's leak is TOTAL leak (both tables).** The byte is everything leaving the circuit,
the mask's intentional vent flow included: on a DreamStation 2 night at 3.6 to 3.9 cmH2O
it read a steady 11 to 13 L/min in every one of 44 records, while other readers of the
same night showed no leak at all. The parser keeps it as `BreathingSummary::total_leak` and
never as `leak_rate`, which is UNINTENTIONAL leak in the unified model (a ResMed's PLD
leak, the 24 L/min large-leak line). Unintentional leak is NOT derived from it: the vent's
share depends on the mask and the pressure, and neither the card nor [A], [B] or [C] gives
that curve. The v2 table above and [A] call the byte "leak"; it is the same record and is
treated the same way. Large leak comes from the machine's own 0x10 events, unchanged.

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
8. **v3 timestamp timezone.** [C] reports the v3 header timestamp is true UTC, from FAT
   modification times on its card. On a real DreamStation 1 v3 card's original files, the
   FAT wall-clock time of each summary file equals header timestamp + session duration
   (147 of 150, within 2 s, after a constant one-hour shift from how the copy was taken).
   That shows the header and the machine's wall clock agree; it cannot tell "true UTC on a
   machine whose clock is UTC" from "local time stored as epoch". Treat as unix epoch UTC
   (same as Q1) until one night with a known real start time decides it.
9. **v3 event placement bytes.** The "seconds since it ended" readings of the one-byte field
   in RERA/OA/CA/FL/hypopnea records, and the 2-second duration unit of 0x0e/0x0f/0x10, are
   from [C] only. [S] confirms the 0x0f unit indirectly (statistics word 6, section 9.6) but
   cannot confirm the others without an independent clock.
10. **Black-box check against OSCAR (the released app, never its source).** OSCAR's import of
   the same original card agrees with this parser in every session on obstructive,
   clear-airway and RERA counts (1,872 DreamStation 2 sessions, 149 DreamStation 1 v3
   sessions). Two deliberate differences: OSCAR's hypopnea count includes the 0x0b variant
   (OSCAR = ours + 0x0b in every session), which this parser keeps out to match the
   machine's own total (section 9.6); and OSCAR's session duration is first mask-on to last
   mask-off, where ours is the summed mask-on time (section 9.5).
11. **Events file longer than the summary.** In 106 [S] sessions the events file's elapsed
   total exceeds the summary's equipment-off offset by more than a minute (67 by under 10
   minutes, 26 by under an hour, 13 by an hour or more); never the other way. The waveform's
   end agrees with the summary in 62 of the 83 that have a waveform and with the events file
   in 21. The apnea, hypopnea, RERA and flow-limitation counts still equal the machine's own
   in every one of them, so the extra event time carries no index event. The parser takes
   the session's span from the summary. Why 21 waveforms run past the summary is open.
12. **v3 summary codes 0x0a, 0x0c, 0x0e and the `.B03`/`.B04` files** occur on [S] and are
   not described by [C]. They are skipped by size (section 9.4), never decoded by guess.

## 9. DataFormatVersion 3, Family 0, FamilyVersion 6 (DreamStation 2)

Every [C] fact here was checked against [S] unless the text says it was not. Counts are over
the whole [S] card.

### 9.1 Card layout (decrypted DreamStation 2)

| Path | Content | Source |
|---|---|---|
| `PROP.BIN` | device properties, `key=value` lines ending CR LF; decrypted it is plain text | [S]; key names as [C] `PROP.TXT` |
| `LOG.SEQ` | small binary file, not read | [S]; [C] "unknown" |
| `P0` .. `Pn` | session files `<8 hex digits>.B01` (summary), `.B02` (events), `.B05` (waveform) | [S] |
| `D/` | `.B03` files, not read | [S] |
| `E/` | `.B04` files, not read | [S] |

- The `.B0n` number equals the FileExtension header byte (n) and plays the role of the v2
  `.00n` extension: `.B01` summary, `.B02` events, `.B05` waveform [S, every block on the card].
- The file stem is the session number in hexadecimal, eight digits, and equals the header's
  SessionSequenceNumber [C card layout] [S, e.g. stem `0000029F` = session 0x29f].
- A DreamStation 1 on later firmware writes the same v3 container with plain names
  (`PROP.TXT`, `.001`/`.002`/`.005`) [C card layout]. Not on [S].
- `PROP.BIN` keys seen on [S]: `CF`, `SN`, `MN`, `PT`, `DF`, `DFV`, `F`, `FV`, `SV`, `FD`, `LD`,
  `FN`, `PFN`, `EFN`, `DFN`, `SID`, `BK`, `SK`, `EK`, `DK`, `TS`, `DC`, `VC`. Used:
  `MN` ModelNumber, `SN` SerialNumber, `F` Family, `FV` FamilyVersion, `DFV`
  DataFormatVersion, `SV` software version, `FN` current P folder number [C keys table]
  [S: MN=520X110C, F=0, FV=6, DFV=3]. `SID`, `BK`, `SK`, `EK`, `DK` (meaning unknown; they
  look like key material) are kept verbatim on the device-info record for a separate
  decryption step, never copied into the parsed session, never logged, and never taken from
  a real card into a fixture. The remaining keys are not read.

### 9.2 Chunk container

The 15-byte standard header is laid out exactly as section 3.1 [C chunk container]
[S: 161,593 of 161,593 blocks frame end to end with BlockLength; DataFormatVersion 3,
Family 0, FamilyVersion 6 in every block].

**FileType 0 (`.B01`, `.B02`, `.B03`, `.B04`): header block.** Byte 15 is a count N, then N
pairs (code, record size in bytes), then the 8-bit header checksum: sum of every header byte
before it [C chunk container] [S: 3,860 of 3,860 FileType 0 blocks]. Record sizes vary by
device and firmware and must be taken from this table, never assumed [C]; [S] confirms (its
settings record is 48 bytes against [C]'s 54).

**FileType 1 (`.B05`): signal header.** Not described by [C]; measured on [S]:

| Offset | Size | Field | [S] |
|---|---|---|---|
| 15 | 2 | interval records (u16) | 240 typical |
| 17 | 1 | seconds per interval | 1 |
| 18 | 1 | nsig | 1 in all 157,733 blocks |
| 19 | 4 * nsig | per signal: kind (1), interleave (u16), one more byte | kind 0, interleave 5, last byte 8 |
| then | 1 | terminator, 0 | 0 |
| then | 1 | header checksum, sum8 of every byte before it | matches in 157,733 of 157,733 |

The per-signal entry is 4 bytes where v2's is 3 (section 3.2). The meaning of the fourth byte
is OPEN; it is 8 in every block, plausibly bits per sample.

**Data checksum.** The last 4 bytes of the block, little-endian, are a CRC-32 of the data
block alone: MSB-first, polynomial 0x04C11DB7, initial value 0xFFFFFFFF, no final inversion,
each data byte fed as the 32-bit word 0x000000bb, i.e. the bytes 00 00 00 bb [C chunk
container] [S: 3,860 of 3,860 FileType 0 blocks and 3,000 of 3,000 sampled FileType 1
blocks]. Standard CRC-32 (reflected, final xor) does not match [S].

**Data block** = from after the header checksum to 4 bytes before the end.

### 9.3 Session grouping

[C] reports cards where one file holds chunks of several sessions and says to group chunks by
the header's session number, not the file name, treating a byte-identical repeat as a
duplicate. On [S] every file's chunks carry the file stem's session number. Group by the
header (as v2 already does, section 1).

A session can also straddle patient folders: when the machine opens the next `Pn` folder
mid-session, part of the session's files land in one folder and part in its neighbour [C
mentions this for waveforms]. On [S], 8 sessions are split this way, 4 of them with the
summary in one folder and the events file in the other. A reader that looks at one folder
only reports those nights with no events; it must collect the session's files from every
`Pn` folder of the card.

### 9.4 Record stream (FileType 0 data blocks)

The data block is a sequence of records: one code byte, then exactly the size the header
block gives for that code [C summary file] [S: every data block of every `.B01`, `.B02`,
`.B03`, `.B04` walks to exactly its end]. An unknown code can therefore be SKIPPED by size,
unlike v2 (section 4, which must abandon the block): count it and continue.

### 9.5 Summary (`.B01`)

Header-block sizes on [S]: 0x00:1 0x01:48 0x02:9 0x03:4 0x04:2 0x05:4 0x06:1 0x07:4 0x08:31
0x09:2 0x0a:4 0x0b:11 0x0c:1 0x0d:2 0x0e:6.

| Code | [S] size | Record | Source / check |
|---|---|---|---|
| 0x00 | 1 | equipment on, first record | [C]; [S] once per summary |
| 0x01 | 48 | settings, 9.7 | [C]; [S] once per summary |
| 0x03 | 4 | mask on: u16 seconds since the previous time record, then 2 humidifier status bytes (9.7) | [C] |
| 0x04 | 2 | mask off: u16 seconds since the previous time record | [C] |
| 0x05 | 4 | pressure summary: minimum, maximum, 90th percentile, mean, one byte each, 0.1 cmH2O | [C]; [S] one per mask-on (5,063 each of 0x03/0x04/0x05/0x08) |
| 0x08 | 31 | statistics, 9.6; ONE PER MASK-ON PERIOD on [S] | [C]; [S] |
| 0x02 | 9 | equipment off: u16 seconds since the previous time record, 7 bytes unknown | [C] |
| 0x0a, 0x0c, 0x0e | 4, 1, 6 | unknown, skipped by size | [S] (24, 51 and 1,896 records); not in [C] |

Time records 0x02/0x03/0x04 summed in order give each one's offset from the header
timestamp; the equipment-off offset is the session duration [C]. The THERAPY time used as
the AHI and usage denominator is the sum of the mask-on to mask-off periods, not the
equipment-on to equipment-off span (a mask-off break is not therapy). A summary with no
mask-on record at all is the machine switched on and off without therapy: 25 such on [S],
and they yield no session. [S]: that duration equals
the events file's summed elapsed time exactly in 1,783 of 1,897 sessions, within a minute in
8, and differs by more in 106 (section 8 Q11). The waveform's total seconds equal the summed
mask-on time within 1% in 1,823 of 1,870 sessions that have a waveform.

### 9.6 Statistics record (summary code 0x08)

Fifteen u16 words and one byte [C]. [S] holds one record per mask-on period (1 to 10 per
summary); **sum the words across a summary's records** before comparing with the session's
events. With that sum, on [S], in every session where the value is non-zero:

| Word | Meaning | [S] check |
|---|---|---|
| 1 | obstructive apneas | = count of event 0x06: 1,756 of 1,756 |
| 3 | clear-airway apneas | = count of event 0x07: 1,617 of 1,617 |
| 4 | variable breathing, minutes, floored | = event 0x0e minutes in 995 of 1,416 (approximate, as [C]) |
| 5 | large leak, minutes, floored | = event 0x10 minutes in 552 of 630 (approximate, as [C]) |
| 6 | periodic breathing, minutes, floored | = event 0x0f minutes: 84 of 84 |
| 7 | snore count | [C]; not confirmed on [S] |
| 8 | hypopneas | = count of event 0x14: 1,710 of 1,710 |
| 9 | hypopneas, the 0x0b variant | = count of event 0x0b: 107 of 107. [C] saw 0 here; [S] counts 0x0b in this word |
| 10 | RERAs | = count of event 0x05: 1,454 of 1,454 |
| 11 | flow limitations | = count of event 0x0c: 1,686 of 1,686 |
| 0, 2, 12-14, byte 30 | unknown | not used |

These are the machine's own counts. The parser's counts come from the event records; the
statistics words are a reference beside them, never a replacement, and a disagreement between
the two is worth reporting, not resolving silently.

### 9.7 Settings record (summary code 0x01)

A nested sequence of (code, length, value) triples [C settings] [S: all 1,897 settings
records walk to exactly 48 bytes]. Codes seen on [S] with their lengths: 0x00/1 0x01/2
0x0d/2 0x2c/1 0x2d/1 0x2e/1 0x30/1 0x35/2 0x38/1 0x3e/1 0x3f/1 0x42/1 0x46/1 0x4a/1 0x4b/1.
Meanings per [C] (lengths match [S] where both list the code):

| Code | Meaning [C] |
|---|---|
| 0x00 | therapy mode: 0 CPAP, 1 bi-level, 2 auto CPAP, 3 auto bi-level, 4 CPAP-Check |
| 0x01 | comfort flags (second byte: 0x10 EZ-Start, 0x20 Opti-Start, 0x40 Auto-Trial, 0x80 CPAP-Check+) |
| 0x0a | CPAP pressure, 0.1 cmH2O |
| 0x0d | auto CPAP minimum and maximum pressure, 0.1 cmH2O |
| 0x0e | bi-level EPAP and IPAP |
| 0x0f | auto bi-level EPAP min, IPAP max, PS min, PS max |
| 0x2c | ramp time, minutes, 0 off |
| 0x2d | ramp starting pressure, 0.1 cmH2O |
| 0x2e | pressure relief: 0 off, 0x80 C-Flex/Bi-Flex, 0x90 C-Flex+/A-Flex, 0xa0 rise time, 0xb0 P-Flex |
| 0x30 | pressure relief level |
| 0x35 | humidifier, two status bytes (below) |
| 0x38 | mask resistance, 0 off or 1-5 |
| 0x3e / 0x3f | auto on / auto off, 0 or 0x80 |
| 0x42 | EZ-Start on auto CPAP |
| 0x46 | tubing type: 0 22 mm, 1 15 mm, 2 15 mm heated, 3 12 mm, 4 12 mm heated |
| 0x4a, 0x4b | unknown ([S] only) |

Humidifier status bytes (settings 0x35 and the two bytes after a mask-on time) [C]: byte 1
bits 7-5 mode (0 fixed, 1 adaptive, 2 heated tube, 3 passover, 4 no humidifier, 5 error),
bits 4-2 humidity level 0-5; byte 2 bits 7-5 tube temperature 0-5, bits 4-2 humidity level.

Mapping to `DeviceSettings.therapy_mode` (0 CPAP, 1 APAP, 2 BiPAP): mode 0 and 4 → 0, 2 → 1,
1 and 3 → 2.

### 9.8 Events (`.B02`)

Header-block sizes on [S]: 0x00:2 0x01:3 0x02:4 0x03:3 0x04:3 0x05:3 0x06:3 0x07:3 0x08:3
0x09:2 0x0a:3 0x0b:4 0x0c:3 0x0d:2 0x0e:5 0x0f:5 0x10:5 0x11:5 0x12:4 0x13:3 0x14:3 0x15:3.
Where [C] lists a code, its size agrees.

Every record except 0x12 starts with a u16 of seconds since the previous record; summing them
gives the record's time from the header timestamp [C] ([S] totals: section 9.5).

| Code | Fields after the u16 [C] | Placement (onset, section 4 rule) | [S] records |
|---|---|---|---|
| 0x01 | pressure now delivered, 0.1 cmH2O | stat at clock | 307,736 |
| 0x02 | EPAP, IPAP | stat at clock | 0 |
| 0x03 | auto CPAP starting pressure | ignored | 10,344 |
| 0x04 | pressure pulse, 1 byte unknown | ignored | 58,895 |
| 0x05 | RERA; byte = seconds since it ended | clock - byte | 7,699 |
| 0x06 | obstructive apnea; byte = seconds since | clock - byte | 30,526 |
| 0x07 | clear-airway apnea; byte = seconds since | clock - byte | 12,091 |
| 0x0b | hypopnea variant: 1 byte unknown, then seconds since it ended | clock - second byte | 146 |
| 0x0c | flow limitation; seconds since | clock - byte | 20,366 |
| 0x0d | vibratory snore, no data | clock | 15,840 |
| 0x0e | variable breathing: u16 duration in 2-second units, then seconds since it ended | clock - since - duration | 4,577 |
| 0x0f | periodic breathing: same layout | clock - since - duration | 92 |
| 0x10 | large leak: same layout | clock - since - duration | 1,500 |
| 0x11 | two-minute statistics: TOTAL leak L/min (vent flow included; never unintentional leak, see 4.2), snore count, average pressure 0.1 cmH2O | stat at clock | 312,696 |
| 0x12 | NO time field: pressure kind (0 CPAP, 1 EPAP, 2 IPAP), pressure, u16 snore count | ignored | 12,598 |
| 0x14 | hypopnea; byte = its duration in seconds | clock - byte | 22,665 |
| 0x0a, 0x15 | hypopnea variants on other models [C] | clock - byte | 0 |

Every other code in the header block is skipped by size and counted. The 0x12 exception is
load-bearing: a reader that consumes a u16 delta from it desynchronizes the clock.
Placement bytes are [C]-only, see section 8 Q9.

### 9.9 Waveform (`.B05`)

Same sample encoding as section 6: one signal, flow, signed 8-bit L/min. [S]: every block has
nsig 1, kind 0, interleave 5 and a data block of exactly interval records x seconds per
interval x interleave bytes (5 samples per second); samples span -128..127 and trace breath
cycles. Consecutive blocks in a file abut exactly (152,664 times on [S]); the other gaps are
whole minutes plus one second (61, 121, 181 s ...), consistent with mask-off periods. [C]
does not read waveforms.
