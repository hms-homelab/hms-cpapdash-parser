"""Resolution of every STR.edf channel: what the file can physically carry.

scale = (phys_max - phys_min) / (dig_max - dig_min) is the smallest step the
channel can represent. Anything finer than that was destroyed by the machine
before we ever opened the file.
"""
import struct, collections

CARD = "/Users/aamat/cool_shit/cpap_card_backup_20260827/STR.edf"
b = open(CARD, 'rb').read()

ns = int(b[252:256]); nrec = int(b[236:244]); hdr = int(b[184:192])
off = 256
labels = [b[off + i * 16:off + (i + 1) * 16].decode('latin1').strip() for i in range(ns)]
off += ns * 16 + ns * 80
units = [b[off + i * 8:off + (i + 1) * 8].decode('latin1').strip() for i in range(ns)]
off += ns * 8
def f8():
    global off
    v = [float(b[off + i * 8:off + (i + 1) * 8]) for i in range(ns)]
    off += ns * 8
    return v
pmin, pmax, dmin, dmax = f8(), f8(), f8(), f8()
off += ns * 80
spr = [int(b[off + i * 8:off + (i + 1) * 8]) for i in range(ns)]
recsize = sum(s * 2 for s in spr)

# Only the channels the API/parser actually consumes.
USED = ["Duration", "AHI", "HI", "AI", "OAI", "CAI", "UAI", "RIN", "CSR",
        "Leak.50", "Leak.70", "Leak.95", "Leak.Max",
        "MaskPress.50", "MaskPress.95", "MaskPress.Max",
        "BlowPress.5", "BlowPress.95",
        "SpO2.50", "SpO2.95", "RespRate.50", "RespRate.95",
        "TidVol.50", "MinVent.50", "MaskEvents"]

print(f"{'channel':16s} {'unit':8s} {'step':>10s}  {'distinct values seen':>20s}")
print("-" * 62)
for name in USED:
    if name not in labels:
        continue
    i = labels.index(name)
    sc = (pmax[i] - pmin[i]) / (dmax[i] - dmin[i])
    of = pmin[i] - dmin[i] * sc
    soff = sum(spr[j] * 2 for j in range(i))
    vals = [struct.unpack_from('<h', b, hdr + r * recsize + soff)[0] * sc + of
            for r in range(nrec)]
    nz = [v for v in vals if v > 0]
    print(f"{name:16s} {units[i]:8s} {sc:10.4g}  {len(set(round(v,6) for v in nz)):20d}")
