"""Compare the STR.edf AHI channel against AHI computed from EVE annotations.

Shows whether the second decimal Rohan sees (0.47 / 0.49) exists in the event
data and is destroyed by STR's 0.1 quantisation.
"""
import struct, os, glob, datetime, collections

CARD = "/Users/aamat/cool_shit/cpap_card_backup_20260827"


def read_header(b):
    ns = int(b[252:256])
    nrec = int(b[236:244])
    rdur = float(b[244:252])
    start = b[168:184].decode('latin1')          # dd.mm.yy hh.mm.ss
    off = 256
    labels = [b[off + i * 16:off + (i + 1) * 16].decode('latin1').strip() for i in range(ns)]
    off += ns * 16 + ns * 80
    off += ns * 8                                 # units
    def f8():
        nonlocal off
        v = [float(b[off + i * 8:off + (i + 1) * 8]) for i in range(ns)]
        off += ns * 8
        return v
    pmin, pmax, dmin, dmax = f8(), f8(), f8(), f8()
    off += ns * 80                                # prefiltering
    spr = [int(b[off + i * 8:off + (i + 1) * 8]) for i in range(ns)]
    hdr = int(b[184:192])
    return dict(ns=ns, nrec=nrec, rdur=rdur, start=start, labels=labels,
                pmin=pmin, pmax=pmax, dmin=dmin, dmax=dmax, spr=spr, hdr=hdr)


def str_daily():
    b = open(os.path.join(CARD, "STR.edf"), 'rb').read()
    h = read_header(b)
    d = h['start'][:8]          # dd.mm.yy, no separator before the time
    dd, mm, yy = d.split('.')
    yy = int(yy)
    yy += 2000 if yy < 85 else 1900
    day0 = datetime.date(yy, int(mm), int(dd))
    recsize = sum(s * 2 for s in h['spr'])
    out = {}
    def chan(name):
        i = h['labels'].index(name)
        soff = sum(h['spr'][j] * 2 for j in range(i))
        sc = (h['pmax'][i] - h['pmin'][i]) / (h['dmax'][i] - h['dmin'][i])
        of = h['pmin'][i] - h['dmin'][i] * sc
        return [struct.unpack_from('<h', b, h['hdr'] + r * recsize + soff)[0] * sc + of
                for r in range(h['nrec'])]
    ahi, dur, hi, ai = chan("AHI"), chan("Duration"), chan("HI"), chan("AI")
    for r in range(h['nrec']):
        out[(day0 + datetime.timedelta(days=r)).strftime("%Y%m%d")] = \
            dict(ahi=ahi[r], dur_min=dur[r], hi=hi[r], ai=ai[r])
    return out


APNEA = ("Obstructive", "Central", "Apnea", "ClearAirway", "Unclassified", "Mixed")


def eve_counts(path):
    b = open(path, 'rb').read()
    h = read_header(b)
    idx = [i for i, l in enumerate(h['labels']) if l.startswith("EDF Annotations")]
    if not idx:
        return None
    recsize = sum(s * 2 for s in h['spr'])
    counts = collections.Counter()
    for r in range(h['nrec']):
        for i in idx:
            soff = sum(h['spr'][j] * 2 for j in range(i))
            raw = b[h['hdr'] + r * recsize + soff: h['hdr'] + r * recsize + soff + h['spr'][i] * 2]
            for tal in raw.split(b'\x00'):
                if not tal.strip():
                    continue
                parts = tal.split(b'\x14')
                for note in parts[1:]:
                    s = note.decode('latin1').strip()
                    if s:
                        counts[s] += 1
    return counts


str_map = str_daily()
print(f"{'date':10s} {'STR AHI':>8s} {'events':>7s} {'hours':>7s} {'computed AHI':>13s} {'floor(.1)':>10s}")
rows = 0
for day in sorted(os.listdir(os.path.join(CARD, "DATALOG"))):
    eves = sorted(glob.glob(os.path.join(CARD, "DATALOG", day, "*_EVE.edf")))
    if not eves or day not in str_map:
        continue
    s = str_map[day]
    if s['dur_min'] <= 0:
        continue
    total = collections.Counter()
    for e in eves:
        c = eve_counts(e)
        if c:
            total.update(c)
    ah = sum(v for k, v in total.items()
             if any(k.startswith(p) for p in APNEA) or k.startswith("Hypopnea"))
    if ah == 0:
        continue
    hours = s['dur_min'] / 60.0
    comp = ah / hours
    import math
    print(f"{day:10s} {s['ahi']:8.2f} {ah:7d} {hours:7.3f} {comp:13.4f} "
          f"{math.floor(comp * 10) / 10:10.1f}")
    rows += 1
    if rows >= 15:
        break
