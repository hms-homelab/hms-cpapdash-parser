"""Census of every annotation label ResMed actually writes into EVE files.

Decides whether the AHI numerator is reconstructable from the columns we
persist (obstructive/central/clear_airway/hypopnea) or whether the parser's
unpersisted `apnea_other` bucket carries real events.
"""
import os, glob, collections

CARD = "/Users/aamat/cool_shit/cpap_card_backup_20260827"


def read_header(b):
    ns = int(b[252:256]); nrec = int(b[236:244]); hdr = int(b[184:192])
    off = 256
    labels = [b[off + i * 16:off + (i + 1) * 16].decode('latin1').strip() for i in range(ns)]
    off += ns * 16 + ns * 80 + ns * 8 + ns * 8 * 4 + ns * 80
    spr = [int(b[off + i * 8:off + (i + 1) * 8]) for i in range(ns)]
    return ns, nrec, hdr, labels, spr


counts = collections.Counter()
files = 0
for day in sorted(os.listdir(os.path.join(CARD, "DATALOG"))):
    for path in glob.glob(os.path.join(CARD, "DATALOG", day, "*_EVE.edf")):
        b = open(path, 'rb').read()
        ns, nrec, hdr, labels, spr = read_header(b)
        idx = [i for i, l in enumerate(labels) if l.startswith("EDF Annotations")]
        if not idx:
            continue
        files += 1
        recsize = sum(s * 2 for s in spr)
        for r in range(nrec):
            for i in idx:
                soff = sum(spr[j] * 2 for j in range(i))
                st = hdr + r * recsize + soff
                raw = b[st:st + spr[i] * 2]
                for tal in raw.split(b'\x00'):
                    if not tal.strip():
                        continue
                    for note in tal.split(b'\x14')[1:]:
                        s = note.decode('latin1').strip()
                        if s:
                            counts[s] += 1

print(f"{files} EVE files scanned\n")
for k, v in counts.most_common():
    print(f"{v:8d}  {k!r}")
