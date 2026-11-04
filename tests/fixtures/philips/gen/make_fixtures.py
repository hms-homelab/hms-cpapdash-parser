#!/usr/bin/env python3
"""Deterministic synthetic PRS1 fixture generator (SDD-002 PR-2, SDD-009).

Builds test cards from docs/PHILIPS_FORMAT.md ONLY (no real patient data,
no OSCAR source). Every field below cites the doc section it exercises.
DataFormatVersion 3 cards (DreamStation 2, DreamStation 1 on v3 firmware) follow
doc section 9; their PROP key fields are obviously fake strings.
Regenerate with:  python3 make_fixtures.py   (writes to ../)
Output is byte-for-byte deterministic; reviewers can regenerate and diff.
"""

import math
import os
import struct

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")

BASE_TS = 1750000000  # fixed epoch for determinism (doc 3.1 timestamp)


# ── checksums (doc 3.3) ─────────────────────────────────────────────────────

def crc16_kermit(data: bytes) -> int:
    crc = 0
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0x8408 if crc & 1 else crc >> 1
    return crc & 0xFFFF


def checksum8(data: bytes) -> int:
    return sum(data) & 0xFF


# ── chunk container (doc 3.1-3.4) ───────────────────────────────────────────

def chunk(ext: int, session: int, ts: int, payload: bytes, *,
          family=0, family_version=4, file_type=0, signal_info=b"",
          corrupt_crc=False) -> bytes:
    header = struct.pack("<BHBBBBII",
                         2,                     # DataFormatVersion (doc 3.1)
                         0,                     # BlockLength placeholder
                         file_type, family, family_version, ext,
                         session, ts) + signal_info
    block_length = len(header) + 1 + len(payload) + 2
    header = struct.pack("<BH", 2, block_length) + header[3:]
    crc = crc16_kermit(payload)
    if corrupt_crc:
        crc ^= 0xFFFF
    return header + bytes([checksum8(header)]) + payload + struct.pack("<H", crc)


def signal_header(records: int, secs_per_interval: int, signals) -> bytes:
    """FileType-1 extra header (doc 3.2): records, interval, nsig, per-signal
    (kind, interleave), terminator 0."""
    out = struct.pack("<HBB", records, secs_per_interval, len(signals))
    for kind, interleave in signals:
        out += struct.pack("<BH", kind, interleave)
    return out + b"\x00"


# ── .001 summary payload (doc 5 / 5.1) ──────────────────────────────────────

def settings_block() -> bytes:
    """24-byte settings (doc 5.1): APAP 4.0-12.0, ramp 15min@4.0, A-Flex 2,
    fixed humidifier level 3, 22mm hose."""
    return bytes([
        0x00,        # unknown
        0x40,        # mode: APAP (doc 5.1 row 2)
        40, 120,     # min/max pressure * 0.1 cmH2O
        0, 0,        # min/max PS
        0x00,        # startup mode
        15,          # ramp time (min)
        40,          # ramp pressure 4.0
        0x8A,        # flex: enabled | plus mode | level 2 (doc 5.1 row 10)
        0x03, 0x00,  # humidifier: fixed, level 3, present (rows 11-12)
        0x00,        # resistance flags: 22mm hose (row 13)
        0x01,        # unknown (=1)
        0x00,        # general flags
        0x00,        # unknown (=0)
        0x00,        # auto-trial duration
    ]) + bytes(7)    # reserved


def summary_payload(mask_on_delta: int, therapy_secs: int) -> bytes:
    """Equipment on -> mask on -> mask off -> equipment off (doc 5)."""
    p = b"\x00" + settings_block()                                  # equipment on
    p += b"\x02" + struct.pack("<H", mask_on_delta) + bytes(3) + bytes([0x03, 0x00])  # mask on
    p += b"\x03" + struct.pack("<H", therapy_secs) + bytes(34)      # mask off
    p += b"\x01" + struct.pack("<H", 5) + bytes(5)                  # equipment off
    return p


# ── .002 event payload (doc 4.2, F0 V4) ─────────────────────────────────────

def ev(code: int, delta: int, *fields) -> bytes:
    return bytes([code]) + struct.pack("<H", delta) + bytes(fields)


def events_payload() -> bytes:
    return b"".join([
        ev(0x01, 0, 40),            # pressure set 4.0 (stat)
        ev(0x11, 120, 30, 2, 48),   # early stats: leak 30, snore 2, EPAP 4.8
        ev(0x06, 3480, 20),         # OA at clock=maskOn+3600, onset = clock-20
        ev(0x0A, 600, 15),          # hypopnea type 1
        ev(0x0B, 300, 18, 10),      # hypopnea type 2: duration 18, elapsed 10
        ev(0x0F, 600, 30, 0, 5),    # periodic breathing: dur u16=30 -> 60s, elapsed 5
        ev(0x10, 60, 45, 0, 2),     # large leak: dur u16=45 -> 90s, elapsed 2
        ev(0x11, 60, 25, 3, 52),    # stats: leak 25, snore 3, EPAP 5.2
        ev(0x05, 30, 12),           # RERA
        ev(0x07, 10, 8),            # clear airway
        ev(0x0C, 5, 6),             # flow limitation
        ev(0x0D, 5),                # vibratory snore (no fields)
    ])


# ── .005 waveform payload (doc 6) ───────────────────────────────────────────

def flow_samples(seconds: int, rate: float = 5.0, bpm: float = 15.0,
                 amp: float = 40.0) -> bytes:
    """Signed 8-bit sine flow starting inside inspiration.

    Phase must survive int8 rounding: the breath detector needs a POSITIVE
    first sample to pair cycles correctly (pre-existing parity behavior,
    see tests/test_breath_analysis.cpp)."""
    n = int(seconds * rate)
    period = 60.0 / bpm
    out = bytearray()
    for i in range(n):
        t = i / rate
        v = int(round(amp * math.sin(2.0 * math.pi * t / period + 0.1)))
        out.append(v & 0xFF)
    return bytes(out)


# ── DataFormatVersion 3 container (doc 9.2) ─────────────────────────────────

def _crc32_table():
    table = []
    for i in range(256):
        c = i << 24
        for _ in range(8):
            c = ((c << 1) ^ 0x04C11DB7) & 0xFFFFFFFF if c & 0x80000000 else (c << 1) & 0xFFFFFFFF
        table.append(c)
    return table


_CRC32_TABLE = _crc32_table()


def crc32_wordfed(data: bytes) -> int:
    """MSB-first CRC-32, poly 0x04C11DB7, init 0xFFFFFFFF, no final xor, each byte
    fed as the word 00 00 00 b (doc 9.2)."""
    crc = 0xFFFFFFFF
    for b in data:
        for byte in (0, 0, 0, b):
            crc = ((crc << 8) & 0xFFFFFFFF) ^ _CRC32_TABLE[((crc >> 24) ^ byte) & 0xFF]
    return crc


def chunk_v3(ext: int, session: int, ts: int, payload: bytes, *, sizes=None,
             signal_info=b"", family=0, family_version=6, corrupt_crc=False) -> bytes:
    """One v3 chunk. FileType 0 when `sizes` (code -> record size) is given: the
    header block is count N + N (code, size) pairs. FileType 1 when
    `signal_info` is given. Then sum8, data, LE word-fed CRC-32 (doc 9.2)."""
    file_type = 1 if signal_info else 0
    extra = signal_info
    if sizes is not None:
        extra = bytes([len(sizes)]) + b"".join(bytes([c, s]) for c, s in sizes.items())
    fixed = struct.pack("<BBBB", file_type, family, family_version, ext) + \
        struct.pack("<II", session, ts)
    block_length = 3 + len(fixed) + len(extra) + 1 + len(payload) + 4
    header = struct.pack("<BH", 3, block_length) + fixed + extra
    crc = crc32_wordfed(payload)
    if corrupt_crc:
        crc ^= 0xFFFFFFFF
    return header + bytes([checksum8(header)]) + payload + struct.pack("<I", crc)


def signal_header_v3(records: int, secs_per_interval: int, signals) -> bytes:
    """v3 FileType-1 extra header (doc 9.2): per-signal entries are 4 bytes
    (kind, interleave, one more byte), then terminator 0."""
    out = struct.pack("<HBB", records, secs_per_interval, len(signals))
    for kind, interleave, extra in signals:
        out += struct.pack("<BHB", kind, interleave, extra)
    return out + b"\x00"


# Header-block record sizes as a DreamStation 2 writes them (doc 9.5 / 9.8),
# plus one code the doc does not describe, to exercise skip-by-size.
V3_SUMMARY_SIZES = {0x00: 1, 0x01: 0, 0x02: 9, 0x03: 4, 0x04: 2, 0x05: 4, 0x08: 31,
                    0x09: 2, 0x0e: 6}            # 0x01 filled in from the settings
V3_EVENT_SIZES = {0x01: 3, 0x02: 4, 0x03: 3, 0x04: 3, 0x05: 3, 0x06: 3, 0x07: 3,
                  0x0b: 4, 0x0c: 3, 0x0d: 2, 0x0e: 5, 0x0f: 5, 0x10: 5, 0x11: 5,
                  0x12: 4, 0x14: 3, 0x20: 3}     # 0x20: not in doc 9.8


def settings_v3() -> bytes:
    """Doc 9.7 nested (code, length, value): auto CPAP 5.0-12.0, ramp 20 min from
    4.0, C-Flex+/A-Flex level 2, heated tube level 3 / tube temp 3, 15 mm hose,
    plus 0x4a (seen on [S], meaning unknown) to exercise skip-by-length."""
    triples = [(0x00, [2]), (0x0d, [50, 120]), (0x2c, [20]), (0x2d, [40]),
               (0x2e, [0x90]), (0x30, [2]),
               (0x35, [(2 << 5) | (3 << 2), (3 << 5) | (3 << 2)]),
               (0x46, [1]), (0x4a, [0])]
    return b"".join(bytes([c, len(v)] + v) for c, v in triples)


def stats_v3(oa=0, ca=0, vb_min=0, ll_min=0, pb_min=0, h=0, h_variant=0, rera=0, fl=0):
    """Doc 9.6: fifteen u16 words + one byte."""
    words = [0, oa, 0, ca, vb_min, ll_min, pb_min, 0, h, h_variant, rera, fl, 10000, 70, 0x0e00]
    return struct.pack("<15H", *words) + bytes([50])


def rec(code: int, body: bytes) -> bytes:
    return bytes([code]) + body


def summary_v3(first_on: int, periods, stats) -> (bytes, dict):
    """Equipment on, settings, then per mask-on period: mask on (delta since the
    previous time record + 2 humidifier bytes), pressure summary, mask off,
    statistics; an undescribed 0x09 and the seen-but-unknown 0x0e; equipment off.
    periods = [(gap_before_seconds, length_seconds)]."""
    s = settings_v3()
    sizes = dict(V3_SUMMARY_SIZES)
    sizes[0x01] = len(s)
    p = rec(0x00, b"\x01") + rec(0x01, s)
    for i, (gap, length) in enumerate(periods):
        delta = first_on if i == 0 else gap
        p += rec(0x03, struct.pack("<H", delta) + bytes([0x4c, 0x6c]))
        p += rec(0x05, bytes([50, 90, 80, 70]))
        p += rec(0x04, struct.pack("<H", length))
        p += rec(0x08, stats[i])
    p += rec(0x09, b"\x00\x00") + rec(0x0e, bytes(6))
    p += rec(0x02, struct.pack("<H", 30) + bytes(7))
    return p, sizes


def ev3(code: int, delta: int, *fields) -> bytes:
    return bytes([code]) + struct.pack("<H", delta) + bytes(fields)


def events_v3() -> bytes:
    """Doc 9.8 records; clock starts at the header timestamp. Counts: OA 3, CA 1,
    H (0x14) 2, 0x0b 1, RERA 1, FL 1, plus PB 120 s, a 0x12 (no time field) and
    the undescribed 0x20."""
    return b"".join([
        ev3(0x01, 60, 50),                  # pressure 5.0 at mask-on
        ev3(0x11, 120, 24, 1, 62),          # two-minute stats: leak 24, snore 1, 6.2
        ev3(0x06, 600, 20),                 # OA, ended 20 s ago
        ev3(0x06, 300, 15),
        ev3(0x14, 300, 12),                 # hypopnea, duration 12 s
        ev3(0x0b, 60, 0, 9),                # hypopnea variant: unknown byte, since 9
        bytes([0x12, 0, 60, 3, 0]),         # snores at a pressure: NO time field
        ev3(0x07, 30, 8),                   # CA
        ev3(0x20, 10, 0),                   # undescribed, skipped by size
        ev3(0x05, 30, 12),                  # RERA
        ev3(0x0c, 30, 6),                   # FL
        ev3(0x0d, 10),                      # vibratory snore
        ev3(0x0f, 600, 60, 0, 5),           # periodic breathing: 60 x 2 s, since 5
        ev3(0x06, 2400, 10),                # OA, second mask-on period
        ev3(0x14, 300, 14),
    ])


def prop_bin(serial: str, model: str, *, crlf=True) -> str:
    nl = "\r\n" if crlf else "\n"
    keys = [("CF", "6"), ("SN", serial), ("MN", model), ("PT", "0x8B"), ("DF", "0"),
            ("DFV", "3"), ("F", "0"), ("FV", "6"), ("SV", "V1.0.0.0000"), ("FD", str(BASE_TS)),
            ("LD", str(BASE_TS + 86400)), ("FN", "0"), ("SID", "FAKE-SID"),
            ("BK", "FAKE-BK"), ("SK", "FAKE-SK"), ("EK", "FAKE-EK"), ("DK", "FAKE-DK")]
    return "".join(f"{k}={v}{nl}" for k, v in keys)


def pseudo_random(n: int, seed: int) -> bytes:
    """Deterministic noise standing in for encrypted bytes (LCG)."""
    out = bytearray()
    x = seed
    for _ in range(n):
        x = (1103515245 * x + 12345) & 0x7FFFFFFF
        out.append((x >> 16) & 0xFF)
    return bytes(out)


# ── writers ─────────────────────────────────────────────────────────────────

def write(relpath: str, data) -> None:
    path = os.path.join(OUT, relpath)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    mode = "wb" if isinstance(data, (bytes, bytearray)) else "w"
    with open(path, mode) as f:
        f.write(data)
    print(f"wrote {relpath} ({len(data)} bytes)")


def properties_txt(serial: str, model: str, family_version: int) -> str:
    return (f"SerialNumber={serial}\n"
            f"ModelNumber={model}\n"
            f"ProductType=0x35\n"
            f"Family=0\n"
            f"FamilyVersion={family_version}\n"
            f"DataFormatVersion=2\n"
            f"PatientFolderNum=0\n"
            f"PatientFileNum=1\n"
            f"FirstDate={BASE_TS}\n"
            f"LastDate={BASE_TS + 86400}\n"
            "\n")


def main() -> None:
    # ── ds1/: DreamStation 1 (F0 V4), full happy-path session ──
    mask_on = BASE_TS + 60
    write("ds1/Properties.txt", properties_txt("PXTEST0000DS1", "500X110", 4))
    write("ds1/0000000001.001",
          chunk(1, 1, BASE_TS, summary_payload(60, 6 * 3600)))
    write("ds1/0000000001.002",
          chunk(2, 1, mask_on, events_payload()))
    # Two signal chunks with a 10 s gap (doc 6: gaps produce no samples)
    sig = signal_header(30, 10, [(0, 500)])  # 300 s per chunk
    write("ds1/0000000001.005",
          chunk(5, 1, mask_on, flow_samples(300), file_type=1, signal_info=sig) +
          chunk(5, 1, mask_on + 310, flow_samples(300), file_type=1, signal_info=sig))

    # Second, older group with only a waveform: latest-with-summary selection
    write("ds1/0000000000.005",
          chunk(5, 0, BASE_TS - 86400, flow_samples(60), file_type=1,
                signal_info=signal_header(6, 10, [(0, 300)])))

    # ── systemone/: System One (F0 V3): sub-V4 gating path ──
    # .001/.002 must be counted unsupported + skipped; .005 still parses.
    write("systemone/Properties.txt", properties_txt("PXTEST00560P1", "560P", 3))
    write("systemone/0000000002.001",
          chunk(1, 2, BASE_TS, summary_payload(60, 3600), family_version=3))
    write("systemone/0000000002.002",
          chunk(2, 2, BASE_TS + 60, events_payload(), family_version=3))
    write("systemone/0000000002.005",
          chunk(5, 2, BASE_TS + 60, flow_samples(300), family_version=3,
                file_type=1, signal_info=signal_header(30, 10, [(0, 500)])))

    # ── broken variants ──
    # Bad data CRC: chunk must be skipped, session must survive via .005
    write("badcrc/0000000009.001",
          chunk(1, 9, BASE_TS, summary_payload(60, 3600), corrupt_crc=True))
    write("badcrc/0000000009.005",
          chunk(5, 9, BASE_TS + 60, flow_samples(300), file_type=1,
                signal_info=signal_header(30, 10, [(0, 500)])))

    # Truncated tail: valid chunk followed by a cut-off one
    good = chunk(5, 10, BASE_TS, flow_samples(300), file_type=1,
                 signal_info=signal_header(30, 10, [(0, 500)]))
    cut = chunk(5, 10, BASE_TS + 300, flow_samples(300), file_type=1,
                signal_info=signal_header(30, 10, [(0, 500)]))[:100]
    write("truncated/0000000010.005", good + cut)

    # Unknown event code 0x7F mid-stream: earlier events kept, block abandoned
    payload = ev(0x06, 60, 20) + ev(0x7F, 10, 1, 2, 3) + ev(0x07, 10, 8)
    write("unknowncode/0000000011.001",
          chunk(1, 11, BASE_TS, summary_payload(60, 3600)))
    write("unknowncode/0000000011.002",
          chunk(2, 11, BASE_TS + 60, payload))

    # Unknown DataFormatVersion (4): counted unsupported, never guessed at
    v4 = bytearray(chunk(1, 12, BASE_TS, summary_payload(60, 3600)))
    v4[0] = 4
    write("unknownversion/0000000012.001", bytes(v4))

    # ── ds1_twomask/: v2 summary with two mask-on periods and a 30 min break.
    # Duration must be the SUM of the periods (SDD-009 D12), not the envelope.
    p = b"\x00" + settings_block()
    p += b"\x02" + struct.pack("<H", 60) + bytes(3) + bytes([0x03, 0x00])   # on @60
    p += b"\x03" + struct.pack("<H", 3600) + bytes(34)                       # off @3660
    p += b"\x02" + struct.pack("<H", 1800) + bytes(3) + bytes([0x03, 0x00])  # on @5460
    p += b"\x03" + struct.pack("<H", 1200) + bytes(34)                       # off @6660
    p += b"\x01" + struct.pack("<H", 5) + bytes(5)
    write("ds1_twomask/Properties.txt", properties_txt("PXTEST0002MSK", "500X110", 4))
    write("ds1_twomask/0000000013.001", chunk(1, 13, BASE_TS, p))

    # ── ds2v3/: decrypted DreamStation 2 (v3, F0 FV6, doc 9) ──
    # Session 0x1E in P0: two mask-on periods (60 min, 10 min break, 30 min), one
    # statistics record per period. Session 0x1F straddles folders: summary in P1,
    # events in P0 (doc 9.3).
    write("ds2v3/PROP.BIN", prop_bin("PXTEST0DS2V3", "520X110C"))
    stats = [stats_v3(oa=2, ca=1, h=1, h_variant=1, rera=1, fl=1, pb_min=2),
             stats_v3(oa=1, h=1)]
    summ, sizes = summary_v3(60, [(0, 3600), (600, 1800)], stats)
    write("ds2v3/P0/0000001E.B01", chunk_v3(1, 0x1E, BASE_TS, summ, sizes=sizes))
    write("ds2v3/P0/0000001E.B02", chunk_v3(2, 0x1E, BASE_TS, events_v3(), sizes=V3_EVENT_SIZES))
    sig = signal_header_v3(240, 1, [(0, 5, 8)])          # 240 s at 5 Hz per chunk
    write("ds2v3/P0/0000001E.B05",
          chunk_v3(5, 0x1E, BASE_TS + 60, flow_samples(240), signal_info=sig) +
          chunk_v3(5, 0x1E, BASE_TS + 300, flow_samples(240), signal_info=sig))
    ts2 = BASE_TS + 86400
    summ2, sizes2 = summary_v3(60, [(0, 3600)], [stats_v3(oa=3, ca=1, h=2, h_variant=1,
                                                          rera=1, fl=1, pb_min=2)])
    write("ds2v3/P1/0000001F.B01", chunk_v3(1, 0x1F, ts2, summ2, sizes=sizes2))
    write("ds2v3/P0/0000001F.B02", chunk_v3(2, 0x1F, ts2, events_v3(), sizes=V3_EVENT_SIZES))

    # ── ds1v3/: DreamStation 1 on v3 firmware: PROP.TXT and plain .001/.002/.005
    # names with 8-hex stems, same v3 container (doc 9.1) ──
    write("ds1v3/PROP.TXT", prop_bin("PXTEST0DS1V3", "500X150", crlf=False))
    summ3, sizes3 = summary_v3(60, [(0, 3600)], [stats_v3(oa=3, ca=1, h=2, h_variant=1,
                                                          rera=1, fl=1, pb_min=2)])
    write("ds1v3/P0/00000028.001", chunk_v3(1, 0x28, BASE_TS, summ3, sizes=sizes3))
    write("ds1v3/P0/00000028.002", chunk_v3(2, 0x28, BASE_TS, events_v3(), sizes=V3_EVENT_SIZES))
    write("ds1v3/P0/00000028.005",
          chunk_v3(5, 0x28, BASE_TS + 60, flow_samples(240), signal_info=sig))

    # ── ds2v3_badcrc/: the events chunk fails its CRC; the night still parses ──
    write("ds2v3_badcrc/PROP.BIN", prop_bin("PXTEST0DS2BC", "520X110C"))
    write("ds2v3_badcrc/P0/00000032.B01", chunk_v3(1, 0x32, BASE_TS, summ, sizes=sizes))
    write("ds2v3_badcrc/P0/00000032.B02",
          chunk_v3(2, 0x32, BASE_TS, events_v3(), sizes=V3_EVENT_SIZES, corrupt_crc=True))

    # ── ds2v3_multisession/: one summary file holding sessions 0x3C and 0x3D,
    # plus a byte-identical repeat of 0x3C's chunk (doc 9.3) ──
    write("ds2v3_multisession/PROP.BIN", prop_bin("PXTEST0DS2MS", "520X110C"))
    one = chunk_v3(1, 0x3C, BASE_TS, summ2, sizes=sizes2)
    two = chunk_v3(1, 0x3D, BASE_TS + 86400, summ2, sizes=sizes2)
    write("ds2v3_multisession/P0/0000003C.B01", one + two + one)

    # ── ds2_encrypted/: a still-encrypted card. PROP.BIN and .B01 are noise. ──
    write("ds2_encrypted/PROP.BIN", pseudo_random(400, 7))
    write("ds2_encrypted/P0/00000046.B01", pseudo_random(160, 11))

    # ── ds2_encrypted_data/: headers in the clear, every data CRC fails ──
    write("ds2_encrypted_data/P0/00000050.B01",
          chunk_v3(1, 0x50, BASE_TS, summ2, sizes=sizes2, corrupt_crc=True))
    write("ds2_encrypted_data/P0/00000050.B02",
          chunk_v3(2, 0x50, BASE_TS, events_v3(), sizes=V3_EVENT_SIZES, corrupt_crc=True))


if __name__ == "__main__":
    main()
