#!/usr/bin/env python3
"""Generate the Sefam S.Box test fixtures.

Written to the format a real 242-session donor card actually uses (SDD-005,
docs/SEFAM_FORMAT.md), not to a guess:

    bytes 0..37   38-byte ASCII header, every byte XOR 0xBF, reading
                  "#02/<model><serial>      /<YYMMDDhhmmss>/"
    then          samples in ten-second blocks, each followed by three bytes:
                  the sum of the block's bytes mod 256, then a big-endian
                  uint16 block index

These are synthetic, and they exist to exercise the paths a single donor card
cannot: a corrupt checksum, channels that disagree about how long the recording
was, an identity that does not match the INI. The real card is the accuracy
check; these are the refusal checks.

Everything is deterministic: fixed waveforms, no randomness, so regenerating
produces byte-identical files.

Run from the repository root:

    python3 tests/fixtures/sefam/gen/make_fixtures.py
"""

import math
import os
import shutil

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)  # tests/fixtures/sefam

HEADER_BYTES = 38
HEADER_XOR = 0xBF
BLOCK_SECONDS = 10

IDENTITY = "1263R24337476"
STAMP = "251110222516"  # 2025-11-10 22:25:16, as [Start Record] below
DURATION_S = 120


def header(identity=IDENTITY, stamp=STAMP) -> bytes:
    """The 38-byte file header, obfuscated the way the device writes it."""
    pad = HEADER_BYTES - (4 + 1 + len(stamp) + 1) - len(identity)
    text = f"#02/{identity}{' ' * pad}/{stamp}/"
    assert len(text) == HEADER_BYTES, f"{len(text)} != {HEADER_BYTES}: {text!r}"
    return bytes(ord(c) ^ HEADER_XOR for c in text)


def frame(samples, freq, bits=8, corrupt_block=None, first_index=1) -> bytes:
    """Wrap raw sample bytes in the ten-second block framing."""
    b = (bits + 7) // 8
    payload = bytearray()
    for s in samples:
        v = max(0, min((1 << bits) - 1, int(round(s))))
        payload += v.to_bytes(b, "little")

    per_block = BLOCK_SECONDS * freq * b
    out = bytearray()
    index = first_index
    for at in range(0, len(payload), per_block):
        chunk = payload[at:at + per_block]
        out += chunk
        checksum = sum(chunk) & 0xFF
        if corrupt_block is not None and index == corrupt_block:
            checksum ^= 0x5A  # a trailer that does not describe its block
        out += bytes([checksum]) + index.to_bytes(2, "big")
        index += 1
    return bytes(out)


def write_channel(path, samples, freq, bits=8, **kw):
    with open(path, "wb") as f:
        f.write(header(**{k: v for k, v in kw.items() if k in ("identity", "stamp")}))
        f.write(frame(samples, freq, bits,
                      corrupt_block=kw.get("corrupt_block"),
                      first_index=kw.get("first_index", 1)))


def write_stub(path, **kw):
    """A channel that was declared but never recorded: the header, nothing else."""
    with open(path, "wb") as f:
        f.write(header(**{k: v for k, v in kw.items() if k in ("identity", "stamp")}))


# ── Waveforms, in raw codes ─────────────────────────────────────────────────


def flow(freq, seconds):
    # Centred on 128, which is where the donor card's flow sits: a night has to
    # integrate to nothing.
    return [128 + 60 * math.sin(2 * math.pi * i / (freq * 4))
            for i in range(freq * seconds)]


def pressure(freq, seconds):
    # Codes near 110, which read as 11.0 cmH2O.
    return [110 + 4 * math.sin(2 * math.pi * i / (freq * 8))
            for i in range(freq * seconds)]


def leak(freq, seconds):
    # Codes near 122, which read as 12.2 L/min.
    return [122 + 3 * math.sin(2 * math.pi * i / (freq * 30))
            for i in range(freq * seconds)]


def spo2(freq, seconds):
    return [96 + 2 * math.sin(2 * math.pi * i / (freq * 40))
            for i in range(freq * seconds)]


def heart_rate(freq, seconds):
    return [60 + 5 * math.sin(2 * math.pi * i / (freq * 50))
            for i in range(freq * seconds)]


def effort(freq, seconds):
    return [128 + 40 * math.sin(2 * math.pi * i / (freq * 3))
            for i in range(freq * seconds)]


def detections(freq, seconds):
    """A bitfield, the way the donor card writes it: several flags at once, each
    with its own prevalence. Bit 1 is set for most of the recording, bit 3 for
    almost none of it."""
    n = freq * seconds
    out = []
    for i in range(n):
        v = 0
        if (i // freq) % 3 != 0:
            v |= 0x02
        if 30 * freq <= i < 50 * freq:
            v |= 0x20
        if 70 * freq <= i < 85 * freq:
            v |= 0x80
        if 100 * freq <= i < 100 * freq + freq // 2:
            v |= 0x08
        out.append(v)
    return out


# ── INI ─────────────────────────────────────────────────────────────────────


def ini_text(channels, oximeter="NONE", serial=IDENTITY):
    lines = [
        "[Create Info]",
        "Created By=S.Box_AUTO ",
        f"Serial Number={serial}",
        "Version=VER :A020400",
        "Date=10/11/25 22:25:16",
        "[Oximeter]",
        f"TYPE={oximeter}",
        "BDA=  :  :  :  :  :  ",
        "[BLE Device]",
        "Local BDA=000000000000",
        "Distant BDA=000000000000",
        "[Start Record]",
        "Hour=22",
        "Min=25",
        "Sec=16",
        "Day=10",
        "Month=11",
        "Year=2025",
        # Eight hours declared against a two-minute recording, which is what the
        # donor card does on every single session.
        "Programmed Record Duration=28800",
        "Real Record Duration=28800",
    ]
    for idx, ch in enumerate(channels):
        lines += [
            f"[Chan{idx}]",
            f"Name={ch['name']}",
            "Description=NO",
            f"Type={ch['type']}",
            f"Unit={ch['unit']}",
            f"Min={ch['min']}",
            f"Max={ch['max']}",
            f"Freq={ch['freq']}",
            f"Bit={ch['bit']}",
        ]
    # CRLF, because the device writes it and the reader has to cope.
    return "\r\n".join(lines) + "\r\n"


def C(name, type_, unit, lo, hi, freq, bit=8):
    return {"name": name, "type": type_, "unit": unit,
            "min": lo, "max": hi, "freq": freq, "bit": bit}


FLW = C("FLW", 4, "lpm", -180, 280, 25)
PRE = C("PRE", 11, "cmH20", 0, 255, 5)
LK = C("LK", 4, "lpm", 0, 153, 1)
DET = C("DET", 13, "", 0, 255, 25)
SPO = C("SPO", 16, "%", 0, 255, 1)
HRT = C("HRT", 17, "bpm", 25, 280, 1)
POS = C("POS", 13, "", 0, 255, 1)
THO = C("THO", 13, "", 0, 255, 10)


def session_dir(*parts):
    path = os.path.join(ROOT, *parts)
    os.makedirs(path, exist_ok=True)
    return path


def write_ini(folder, name, text):
    with open(os.path.join(folder, f"{name}.INI"), "w", newline="") as f:
        f.write(text)


# ── The fixtures ────────────────────────────────────────────────────────────


def make_card():
    """A CPAP-only night inside the two-level card tree. Also the detection
    fixture: the session sits under <model>/<serial>/, not in the folder handed
    to the parser."""
    folder = session_dir("card", "1263R", "24337476", "DATA_0")
    name = "DATA_0"

    write_ini(folder, name, ini_text([FLW, PRE, LK, DET, SPO, HRT, POS]))
    write_channel(os.path.join(folder, f"{name}.FLW"), flow(25, DURATION_S), 25)
    write_channel(os.path.join(folder, f"{name}.PRE"), pressure(5, DURATION_S), 5)
    write_channel(os.path.join(folder, f"{name}.LK"), leak(1, DURATION_S), 1)
    write_channel(os.path.join(folder, f"{name}.DET"), detections(25, DURATION_S), 25)

    # No oximeter and no position sensor on this night.
    for ext in ("SPO", "HRT", "POS"):
        write_stub(os.path.join(folder, f"{name}.{ext}"))


def make_oximetry():
    """Oximetry attached, an unmapped channel, and a file the INI never declares.

    Its blocks are numbered from 2, which two sessions on the donor card do with
    every checksum correct -- so a parser that insists on a first index of 1
    throws away good nights."""
    folder = session_dir("oximetry", "DATA_1")
    name = "DATA_1"

    write_ini(folder, name, ini_text([FLW, PRE, LK, DET, SPO, HRT, THO],
                                     oximeter="INTEGRATED"))
    write_channel(os.path.join(folder, f"{name}.FLW"), flow(25, DURATION_S), 25,
                  first_index=2)
    write_channel(os.path.join(folder, f"{name}.PRE"), pressure(5, DURATION_S), 5,
                  first_index=2)
    write_channel(os.path.join(folder, f"{name}.LK"), leak(1, DURATION_S), 1,
                  first_index=2)
    write_channel(os.path.join(folder, f"{name}.DET"), detections(25, DURATION_S), 25,
                  first_index=2)
    write_channel(os.path.join(folder, f"{name}.SPO"), spo2(1, DURATION_S), 1,
                  first_index=2)
    write_channel(os.path.join(folder, f"{name}.HRT"), heart_rate(1, DURATION_S), 1,
                  first_index=2)
    write_channel(os.path.join(folder, f"{name}.THO"), effort(10, DURATION_S), 10,
                  first_index=2)

    # Present on the card, absent from the INI. Ignored, and reported as ignored.
    write_channel(os.path.join(folder, f"{name}.Y17"), flow(25, DURATION_S), 25,
                  first_index=2)


def make_stubs_only():
    """Every declared channel is a stub. Refused for holding no data, which is a
    different thing from being unreadable."""
    folder = session_dir("stubs", "DATA_2")
    name = "DATA_2"

    channels = [FLW, PRE, LK]
    write_ini(folder, name, ini_text(channels))
    for ch in channels:
        write_stub(os.path.join(folder, f"{name}.{ch['name']}"))


def make_bad_checksum():
    """One block trailer that does not describe its block.

    The checksums are the whole reason to trust a reading of this format, so a
    failure is refused rather than reported around."""
    folder = session_dir("badsum", "DATA_3")
    name = "DATA_3"

    write_ini(folder, name, ini_text([FLW, PRE, LK]))
    write_channel(os.path.join(folder, f"{name}.FLW"), flow(25, DURATION_S), 25)
    write_channel(os.path.join(folder, f"{name}.PRE"), pressure(5, DURATION_S), 5,
                  corrupt_block=4)
    write_channel(os.path.join(folder, f"{name}.LK"), leak(1, DURATION_S), 1)


def make_span_mismatch():
    """Channels that disagree about how long the recording was."""
    folder = session_dir("spans", "DATA_4")
    name = "DATA_4"

    write_ini(folder, name, ini_text([FLW, PRE, LK]))
    write_channel(os.path.join(folder, f"{name}.FLW"), flow(25, DURATION_S), 25)
    write_channel(os.path.join(folder, f"{name}.PRE"), pressure(5, DURATION_S), 5)
    write_channel(os.path.join(folder, f"{name}.LK"), leak(1, DURATION_S - 60), 1)


def make_identity_mismatch():
    """A data header naming a different device from the INI."""
    folder = session_dir("mismatch", "DATA_5")
    name = "DATA_5"

    write_ini(folder, name, ini_text([FLW, PRE, LK]))
    other = "1263R99999999"
    write_channel(os.path.join(folder, f"{name}.FLW"), flow(25, DURATION_S), 25,
                  identity=other)
    write_channel(os.path.join(folder, f"{name}.PRE"), pressure(5, DURATION_S), 5,
                  identity=other)
    write_channel(os.path.join(folder, f"{name}.LK"), leak(1, DURATION_S), 1,
                  identity=other)


def main():
    for sub in ("card", "oximetry", "stubs", "badsum", "spans", "mismatch",
                "plain", "ambiguous"):
        shutil.rmtree(os.path.join(ROOT, sub), ignore_errors=True)

    make_card()
    make_oximetry()
    make_stubs_only()
    make_bad_checksum()
    make_span_mismatch()
    make_identity_mismatch()

    for dirpath, _, filenames in sorted(os.walk(ROOT)):
        if "gen" in dirpath.split(os.sep):
            continue
        for fn in sorted(filenames):
            full = os.path.join(dirpath, fn)
            print(f"{os.path.relpath(full, ROOT):55s} {os.path.getsize(full):>7d}")


if __name__ == "__main__":
    main()
