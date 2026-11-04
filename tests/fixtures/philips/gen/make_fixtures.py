#!/usr/bin/env python3
"""Deterministic synthetic PRS1 fixture generator (SDD-002 PR-2).

Builds test cards from docs/PHILIPS_FORMAT.md ONLY (no real patient data,
no OSCAR source). Every field below cites the doc section it exercises.
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

    # DreamStation 2 container: DataFormatVersion 3 chunk must be rejected
    v3 = bytearray(chunk(1, 12, BASE_TS, summary_payload(60, 3600)))
    v3[0] = 3
    write("ds2/0000000012.001", bytes(v3))
    write("ds2/Properties.txt", properties_txt("PXTEST000DS21", "410X150C", 0))


if __name__ == "__main__":
    main()
