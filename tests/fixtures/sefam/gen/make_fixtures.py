#!/usr/bin/env python3
"""Generate the Sefam S.Box test fixtures.

These cards are SYNTHETIC. No donor card has been received yet (SDD-005), and the
only two write-ups of this format that exist are GPL and unlicensed respectively,
so nothing here is copied from either: the layout below is what the parser is
built to *discover*, written out so the discovery can be tested.

That is the point of them. The header length, the descrambling key and the
channel table are all worked out at parse time, and these fixtures are what prove
that logic works -- including the cases where it must refuse rather than guess.
When a real card arrives it validates the same code against reality; until then
this is what stands between the library and untested assumptions.

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

# The header length the parser has to derive. Chosen to be a value nothing in the
# code knows: if anyone ever hard-codes an offset, these fixtures stop passing.
HEADER_BYTES = 101

# The card-wide XOR the scrambled fixture uses. Likewise never named in the code.
SCRAMBLE_KEY = 0x9F

DURATION_S = 120


def header() -> bytes:
    """A deterministic stand-in for the per-file header.

    The parser never interprets these bytes, it only skips them, so their content
    is irrelevant and only the length matters.
    """
    body = b"SEFAM\x00"
    return body + bytes((i * 7) % 251 for i in range(HEADER_BYTES - len(body)))


def scramble(payload: bytes, key: int) -> bytes:
    return bytes(b ^ key for b in payload)


def write_channel(path: str, samples, key: int) -> None:
    payload = bytes(max(0, min(255, int(round(s)))) for s in samples)
    with open(path, "wb") as f:
        f.write(header())
        f.write(scramble(payload, key))


def write_stub(path: str) -> None:
    """A channel that was declared but never recorded: header, no samples."""
    with open(path, "wb") as f:
        f.write(header())


# ── Waveforms ────────────────────────────────────────────────────────────────
#
# Raw codes, not physical values. The INI's Min/Max turn these into L/min and
# cmH2O; see SefamChannel::toPhysical.


def flow(freq: int, seconds: int):
    # Centred on the code that maps to about zero flow through the declared
    # -180..280 range, so the fixture looks like breathing rather than a offset.
    n = freq * seconds
    return [100 + 60 * math.sin(2 * math.pi * i / (freq * 4)) for i in range(n)]


def pressure(freq: int, seconds: int):
    # Declared 0..25.5, so a code of 100 is 10.0 cmH2O.
    n = freq * seconds
    return [100 + 4 * math.sin(2 * math.pi * i / (freq * 8)) for i in range(n)]


def leak(freq: int, seconds: int):
    # Declared 0..153, so a code of 20 is 12.0 L/min.
    n = freq * seconds
    return [20 + 3 * math.sin(2 * math.pi * i / (freq * 30)) for i in range(n)]


def spo2(freq: int, seconds: int):
    n = freq * seconds
    return [96 + 2 * math.sin(2 * math.pi * i / (freq * 40)) for i in range(n)]


def heart_rate(freq: int, seconds: int):
    # Declared 25..280, so a code of 35 is 60 bpm.
    n = freq * seconds
    return [35 + 5 * math.sin(2 * math.pi * i / (freq * 50)) for i in range(n)]


def effort(freq: int, seconds: int):
    n = freq * seconds
    return [128 + 40 * math.sin(2 * math.pi * i / (freq * 3)) for i in range(n)]


def detections(freq: int, seconds: int):
    """Two events long enough to count, and one blip that must not.

    The blip is there because a detection channel is sampled, not annotated: a
    single sample at 25 Hz is 40 ms, and anything that treats one as an event
    turns noise into a clinical count.
    """
    n = freq * seconds
    codes = [0] * n

    def mark(start_s, end_index, code):
        for i in range(start_s * freq, min(end_index, n)):
            codes[i] = code

    mark(30, 50 * freq, 2)                  # 20 s of code 2
    mark(70, 85 * freq, 7)                  # 15 s of code 7
    mark(seconds - 1, (seconds - 1) * freq + 10, 3)  # 0.4 s of code 3

    return codes


# ── INI ──────────────────────────────────────────────────────────────────────


def ini_text(created_by, serial, channels, oximeter="NONE"):
    lines = [
        "[Create Info]",
        f"Created By={created_by}",
        f"Serial Number={serial}",
        "Version=VER :A020400",
        "Date=13/12/25 23:45:50",
        "[Oximeter]",
        f"TYPE={oximeter}",
        "BDA=  :  :  :  :  :",
        "[BLE Device]",
        "Local BDA=000000000000",
        "Distant BDA=000000000000",
        "[Start Record]",
        "Hour=23",
        "Min=45",
        "Sec=50",
        "Day=13",
        "Month=12",
        "Year=2025",
        "Programmed Record Duration=28800",
        f"Real Record Duration={DURATION_S}",
    ]

    for idx, ch in enumerate(channels):
        lines += [
            f"[Chan{idx}]",
            f"Name={ch['name']}",
            "Description=NO",
            "Type=4",
            f"Unit={ch['unit']}",
            f"Min={ch['min']}",
            f"Max={ch['max']}",
            f"Freq={ch['freq']}",
            f"Bit={ch['bit']}",
        ]

    # CRLF, because the file is written by a Windows toolchain and the reader has
    # to cope with it.
    return "\r\n".join(lines) + "\r\n"


FLW = {"name": "FLW", "unit": "lpm", "min": -180, "max": 280, "freq": 25, "bit": 8}
PRE = {"name": "PRE", "unit": "cmH2O", "min": 0, "max": 25.5, "freq": 5, "bit": 8}
LK = {"name": "LK", "unit": "lpm", "min": 0, "max": 153, "freq": 1, "bit": 8}
DET = {"name": "DET", "unit": "", "min": 0, "max": 255, "freq": 25, "bit": 8}
SPO = {"name": "SPO", "unit": "%", "min": 0, "max": 255, "freq": 1, "bit": 8}
HRT = {"name": "HRT", "unit": "bpm", "min": 25, "max": 280, "freq": 1, "bit": 8}
POS = {"name": "POS", "unit": "", "min": 0, "max": 255, "freq": 1, "bit": 8}
THO = {"name": "THO", "unit": "", "min": 0, "max": 255, "freq": 10, "bit": 8}


def session_dir(*parts):
    path = os.path.join(ROOT, *parts)
    os.makedirs(path, exist_ok=True)
    return path


def write_ini(folder, name, text):
    with open(os.path.join(folder, f"{name}.INI"), "w", newline="") as f:
        f.write(text)


# ── The fixtures ─────────────────────────────────────────────────────────────


def make_card():
    """A scrambled CPAP-only session, inside the two-level card tree.

    Also the detection fixture: the parser has to find DATA_0 under
    <model>/<serial>/ rather than in the folder it was handed.
    """
    folder = session_dir("card", "1263R", "24462543", "DATA_0")
    name = "DATA_0"

    channels = [FLW, PRE, LK, DET, SPO, HRT, POS]
    write_ini(folder, name, ini_text("S.Box_AUTO", "1263R24462543", channels))

    write_channel(os.path.join(folder, f"{name}.FLW"), flow(25, DURATION_S), SCRAMBLE_KEY)
    write_channel(os.path.join(folder, f"{name}.PRE"), pressure(5, DURATION_S), SCRAMBLE_KEY)
    write_channel(os.path.join(folder, f"{name}.LK"), leak(1, DURATION_S), SCRAMBLE_KEY)
    write_channel(os.path.join(folder, f"{name}.DET"), detections(25, DURATION_S), SCRAMBLE_KEY)

    # No oximeter and no position sensor on this night.
    for ext in ("SPO", "HRT", "POS"):
        write_stub(os.path.join(folder, f"{name}.{ext}"))


def make_plain():
    """An unscrambled session with oximetry, an unmapped channel, and a file the
    INI never declares."""
    folder = session_dir("plain", "DATA_1")
    name = "DATA_1"

    channels = [FLW, PRE, LK, DET, SPO, HRT, THO]
    write_ini(folder, name, ini_text("S.Box_AUTO", "1263R24462543", channels,
                                     oximeter="INTEGRATED"))

    write_channel(os.path.join(folder, f"{name}.FLW"), flow(25, DURATION_S), 0)
    write_channel(os.path.join(folder, f"{name}.PRE"), pressure(5, DURATION_S), 0)
    write_channel(os.path.join(folder, f"{name}.LK"), leak(1, DURATION_S), 0)
    write_channel(os.path.join(folder, f"{name}.DET"), detections(25, DURATION_S), 0)
    write_channel(os.path.join(folder, f"{name}.SPO"), spo2(1, DURATION_S), 0)
    write_channel(os.path.join(folder, f"{name}.HRT"), heart_rate(1, DURATION_S), 0)
    write_channel(os.path.join(folder, f"{name}.THO"), effort(10, DURATION_S), 0)

    # Present on the card, absent from the INI. Ignored, and reported as ignored.
    write_channel(os.path.join(folder, f"{name}.Y17"), flow(25, DURATION_S), 0)


def make_stubs_only():
    """Every declared channel is a stub. The header length is still derivable;
    the session holds no data and has to be refused on that basis, not on a
    failure to read it."""
    folder = session_dir("stubs", "DATA_2")
    name = "DATA_2"

    channels = [FLW, PRE, LK]
    write_ini(folder, name, ini_text("S.Box_AUTO", "1263R24462543", channels))
    for ch in channels:
        write_stub(os.path.join(folder, f"{name}.{ch['name']}"))


def make_ambiguous():
    """Two populated channels at the same rate and no stubs.

    Neither derivation can run: there is no pair of differing rates to solve
    from, and no repeated minimum size to read a stub length off. The parser must
    refuse the session rather than assume an offset.
    """
    folder = session_dir("ambiguous", "DATA_3")
    name = "DATA_3"

    channels = [FLW, DET]
    write_ini(folder, name, ini_text("S.Box_AUTO", "1263R24462543", channels))

    write_channel(os.path.join(folder, f"{name}.FLW"), flow(25, DURATION_S), 0)
    write_channel(os.path.join(folder, f"{name}.DET"), detections(25, DURATION_S - 10), 0)


def main():
    for sub in ("card", "plain", "stubs", "ambiguous"):
        shutil.rmtree(os.path.join(ROOT, sub), ignore_errors=True)

    make_card()
    make_plain()
    make_stubs_only()
    make_ambiguous()

    for dirpath, _, filenames in sorted(os.walk(ROOT)):
        if "gen" in dirpath.split(os.sep):
            continue
        for fn in sorted(filenames):
            full = os.path.join(dirpath, fn)
            print(f"{os.path.relpath(full, ROOT):55s} {os.path.getsize(full):>7d}")


if __name__ == "__main__":
    main()
