"""Validate every computed index against ResMed's own STR channel, per type.

If our per-type counts are right, each computed index should floor to the STR
channel for that type. This is the proof that the whole index group is safe to
compute, not just AHI.
"""
import struct, os, glob, datetime, collections, math

CARD = "/Users/aamat/cool_shit/cpap_card_backup_20260827"


def read_header(b):
    ns = int(b[252:256]); nrec = int(b[236:244]); hdr = int(b[184:192])
    start = b[168:184].decode('latin1')
    off = 256
    labels = [b[off + i * 16:off + (i + 1) * 16].decode('latin1').strip() for i in range(ns)]
    off += ns * 16 + ns * 80 + ns * 8
    def f8():
        nonlocal off
        v = [float(b[off + i * 8:off + (i + 1) * 8]) for i in range(ns)]
        off += ns * 8
        return v
    pmin, pmax, dmin, dmax = f8(), f8(), f8(), f8()
    off += ns * 80
    spr = [int(b[off + i * 8:off + (i + 1) * 8]) for i in range(ns)]
    return dict(ns=ns, nrec=nrec, hdr=hdr, start=start, labels=labels,
                pmin=pmin, pmax=pmax, dmin=dmin, dmax=dmax, spr=spr)


def str_daily():
    b = open(os.path.join(CARD, "STR.edf"), 'rb').read()
    h = read_header(b)
    dd, mm, yy = h['start'][:8].split('.')
    yy = int(yy); yy += 2000 if yy < 85 else 1900
    day0 = datetime.date(yy, int(mm), int(dd))
    recsize = sum(s * 2 for s in h['spr'])
    def chan(name):
        i = h['labels'].index(name)
        soff = sum(h['spr'][j] * 2 for j in range(i))
        sc = (h['pmax'][i] - h['pmin'][i]) / (h['dmax'][i] - h['dmin'][i])
        of = h['pmin'][i] - h['dmin'][i] * sc
        return [struct.unpack_from('<h', b, h['hdr'] + r * recsize + soff)[0] * sc + of
                for r in range(h['nrec'])]
    ch = {n: chan(n) for n in ("AHI", "AI", "HI", "OAI", "CAI", "UAI", "RIN", "Duration")}
    return {(day0 + datetime.timedelta(days=r)).strftime("%Y%m%d"):
            {n: ch[n][r] for n in ch} for r in range(h['nrec'])}


def eve_labels(path):
    b = open(path, 'rb').read()
    h = read_header(b)
    idx = [i for i, l in enumerate(h['labels']) if l.startswith("EDF Annotations")]
    recsize = sum(s * 2 for s in h['spr'])
    out = collections.Counter()
    for r in range(h['nrec']):
        for i in idx:
            soff = sum(h['spr'][j] * 2 for j in range(i))
            st = h['hdr'] + r * recsize + soff
            for tal in b[st:st + h['spr'][i] * 2].split(b'\x00'):
                if not tal.strip():
                    continue
                for note in tal.split(b'\x14')[1:]:
                    s = note.decode('latin1').strip()
                    if s:
                        out[s] += 1
    return out


strm = str_daily()
# index name -> (STR channel, predicate over the EVE label)
INDEX = {
    "OAI": ("OAI", lambda k: k == "Obstructive Apnea"),
    "CAI": ("CAI", lambda k: k == "Central Apnea"),
    "UAI": ("UAI", lambda k: k == "Apnea"),
    "HI":  ("HI",  lambda k: k == "Hypopnea"),
    "AI":  ("AI",  lambda k: k in ("Obstructive Apnea", "Central Apnea", "Apnea")),
    "AHI": ("AHI", lambda k: k in ("Obstructive Apnea", "Central Apnea", "Apnea", "Hypopnea")),
    "RIN": ("RIN", lambda k: k == "Arousal"),
}
agree = collections.Counter(); total = collections.Counter(); examples = {}

for day in sorted(os.listdir(os.path.join(CARD, "DATALOG"))):
    eves = sorted(glob.glob(os.path.join(CARD, "DATALOG", day, "*_EVE.edf")))
    if not eves or day not in strm:
        continue
    s = strm[day]
    if s["Duration"] <= 0:
        continue
    lab = collections.Counter()
    for e in eves:
        lab.update(eve_labels(e))
    hours = s["Duration"] / 60.0
    for name, (chan, pred) in INDEX.items():
        n = sum(v for k, v in lab.items() if pred(k))
        comp = n / hours
        total[name] += 1
        if abs(math.floor(comp * 10) / 10 - s[chan]) < 1e-6:
            agree[name] += 1
        elif name not in examples:
            examples[name] = (day, n, round(hours, 3), round(comp, 4), s[chan])

print(f"{'index':6s} {'floor(computed) == STR':>24s}   first disagreement")
for name in ("AHI", "AI", "HI", "OAI", "CAI", "UAI", "RIN"):
    ex = examples.get(name)
    ex = "" if not ex else f"{ex[0]} n={ex[1]} h={ex[2]} comp={ex[3]} STR={ex[4]}"
    print(f"{name:6s} {agree[name]:>10d} / {total[name]:<10d}   {ex}")
