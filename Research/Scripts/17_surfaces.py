"""Everything in 08-Audio-Surfaces.md.

Reads the six official masters directly (no Recordings/ involved) and reports:
  - the MATERIAL_ID hash function, brute-forced and verified against CommonLibSSE
  - the 79 MATT records and the 10 enum IDs with no record
  - how many distinct sounds each IPDS actually resolves to
  - the body impact sets (the ragdoll path) and the footstep sets (the 13-surface palette)
  - LTEX -> MATT, the correct terrain resolver

Usage:  python 17_surfaces.py [--data <SkyrimVR Data dir>]

Writes matt.json next to this script.
"""
import argparse
import json
import os
import re
import struct
from collections import Counter, defaultdict

from esm import scan, zs

MASTERS = ['Skyrim.esm', 'Update.esm', 'Dawnguard.esm',
           'HearthFires.esm', 'Dragonborn.esm', 'SkyrimVR.esm']

DEFAULT_DATA = r"C:\games\steamapps\common\SkyrimVR\Data"
DEFAULT_ENUM = (r"C:\games\skyrim\papyrus\reference\SpellWheelVR 1.5.5 Source"
                r"\skse\3DUI\build\vcpkg_installed\x64-windows-skse\include\RE\M\MaterialIDs.h")

HERE = os.path.dirname(os.path.abspath(__file__))


# ---------------------------------------------------------------- CRC machinery

def _table(poly, refl):
    t = []
    for i in range(256):
        if refl:
            c = i
            for _ in range(8):
                c = (c >> 1) ^ (poly if c & 1 else 0)
        else:
            c = i << 24
            for _ in range(8):
                c = ((c << 1) ^ poly) & 0xffffffff if c & 0x80000000 else (c << 1) & 0xffffffff
        t.append(c & 0xffffffff)
    return t


def _crc(data, poly, refl, init, xorout):
    t = _table(poly, refl)
    c = init
    for b in data:
        if refl:
            c = t[(c ^ b) & 0xff] ^ (c >> 8)
        else:
            c = t[((c >> 24) ^ b) & 0xff] ^ ((c << 8) & 0xffffffff)
    return (c ^ xorout) & 0xffffffff


def material_id(mnam):
    """The one parameterisation that reproduces RE::MATERIAL_ID: reflected CRC32,
    init 0, no final XOR, over the lowercased MNAM."""
    return _crc(mnam.lower().encode('latin-1'), 0xEDB88320, True, 0, 0)


CRC_VARIANTS = {
    'crc32':        (0xEDB88320, True,  0xFFFFFFFF, 0xFFFFFFFF),
    'crc32_noxor':  (0xEDB88320, True,  0xFFFFFFFF, 0),
    'crc32_init0':  (0xEDB88320, True,  0,          0),
    'bzip2':        (0x04C11DB7, False, 0xFFFFFFFF, 0xFFFFFFFF),
    'bzip2_noxor':  (0x04C11DB7, False, 0xFFFFFFFF, 0),
    'mpeg2_init0':  (0x04C11DB7, False, 0,          0),
}


# ---------------------------------------------------------------- record loading

def masters_of(path):
    with open(path, 'rb') as f:
        buf = f.read(400000)
    hs = struct.unpack('<I', buf[4:8])[0]
    i, end, ms = 24, 24 + hs, []
    while i + 6 <= end:
        t = buf[i:i + 4].decode('latin-1')
        sz = struct.unpack('<H', buf[i + 4:i + 6])[0]
        i += 6
        if t == 'MAST':
            ms.append(buf[i:i + sz].split(b'\x00')[0].decode('latin-1'))
        i += sz
    return ms


class World:
    """All the records we care about, keyed by (defining master, local FormID)."""

    SIGS = {'MATT', 'IPDS', 'IPCT', 'SNDR', 'FSTP', 'FSTS', 'LTEX'}

    def __init__(self, data_dir):
        self.edid = {}
        self.recs = defaultdict(dict)
        for f in MASTERS:
            p = os.path.join(data_dir, f)
            if not os.path.exists(p):
                continue
            ms = masters_of(p)
            for sig, lst in scan(p, self.SIGS).items():
                for rec in lst:
                    k = self.g(rec.formid, f, ms)
                    e = zs(rec.get('EDID'))
                    if e:
                        self.edid[k] = e
                    # later masters override earlier ones at the same global id
                    self.recs[sig][k] = (f, ms, rec)

    @staticmethod
    def g(fid, src, ms):
        mi = fid >> 24
        return (ms[mi] if mi < len(ms) else src, fid & 0xffffff)

    def name(self, k):
        return self.edid.get(k, "%s:%06X" % k)

    def by_name(self):
        return {v: k for k, v in self.edid.items()}

    def fid(self, rec_tuple, sub):
        f, ms, rec = rec_tuple
        d = rec.get(sub)
        return self.g(struct.unpack('<I', d)[0], f, ms) if d else None


# ---------------------------------------------------------------- reports

def load_enum(path):
    enum = {}
    if not os.path.exists(path):
        return enum
    with open(path) as fh:
        for line in fh:
            m = re.match(r'\s*k(\w+)\s*=\s*(\d+)', line)
            if m:
                enum[int(m.group(2))] = m.group(1)
    return enum


def report_hash(w, enum):
    print("=== 1. Recovering the MATERIAL_ID hash ===")
    if not enum:
        print("  (MaterialIDs.h not found - skipping verification)")
        return
    strings = [(zs(r[2].get('EDID')), zs(r[2].get('MNAM'))) for r in w.recs['MATT'].values()]
    target = set(enum)
    for vname, (p, rf, i0, xo) in CRC_VARIANTS.items():
        for which in ('EDID', 'MNAM', 'MNAM_lower', 'EDID_lower'):
            hits = 0
            for e, mn in strings:
                s = {'EDID': e, 'MNAM': mn,
                     'MNAM_lower': mn.lower(), 'EDID_lower': e.lower()}[which]
                if _crc(s.encode('latin-1'), p, rf, i0, xo) in target:
                    hits += 1
            if hits:
                print("  %-14s %-12s %d/%d" % (vname, which, hits, len(strings)))


def report_matt(w, enum):
    print("\n=== 2. MATT records ===")
    rows = []
    for k, (f, ms, rec) in w.recs['MATT'].items():
        e, mn = zs(rec.get('EDID')), zs(rec.get('MNAM'))
        hid = material_id(mn) if mn else 0
        pk = w.fid((f, ms, rec), 'PNAM')
        hk = w.fid((f, ms, rec), 'HNAM')
        bn, fn = rec.get('BNAM'), rec.get('FNAM')
        rows.append(dict(src=f, fid=k[1], edid=e, mnam=mn, hash=hid,
                         enum=enum.get(hid), parent=w.name(pk) if pk else None,
                         buoy=struct.unpack('<f', bn)[0] if bn else None,
                         flags=struct.unpack('<I', fn)[0] if fn else 0,
                         hset=w.name(hk) if hk else None))
    rows.sort(key=lambda x: x['edid'] or '')
    print("  %-30s%-26s%>11s  %-24s%6s %3s %s"
          .replace('%>11s', '%11s') % ('EDID', 'MNAM', 'MATERIAL_ID', 'parent', 'buoy', 'flg', 'HNAM'))
    for r in rows:
        print("  %-30s%-26s%11d  %-24s%6.2f %3d %s" % (
            r['edid'], r['mnam'], r['hash'], r['parent'] or '-',
            r['buoy'] if r['buoy'] is not None else 0.0, r['flags'], r['hset'] or '-'))
    have = {r['hash'] for r in rows}
    print("\n  unique MATT: %d" % len(rows))
    print("  stairs-flagged (FNAM bit 0): %d" % sum(1 for r in rows if r['flags'] & 1))
    print("  no havok impact set: %d" % sum(1 for r in rows if not r['hset']))
    print("  enum IDs with NO MATT record: %s"
          % sorted(enum[k] for k in enum if k not in have))
    with open(os.path.join(HERE, 'matt.json'), 'w') as fh:
        json.dump(rows, fh, indent=1)
    return rows


def ipds_pairs(w, k):
    f, ms, rec = w.recs['IPDS'][k]
    out = []
    for d in rec.all('PNAM'):
        a, b = struct.unpack('<II', d)
        out.append((w.g(a, f, ms), w.g(b, f, ms) if b else None))
    return out


def report_discrimination(w):
    print("\n=== 3. How many distinct sounds does each IPDS actually resolve to? ===")
    res = []
    usage = Counter()
    for k in w.recs['IPDS']:
        pairs = ipds_pairs(w, k)
        if not pairs:
            continue
        impacts = {i for _, i in pairs if i}
        for m, i in pairs:
            if i:
                usage[w.name(m)] += 1
        res.append((len(impacts), sum(1 for _, i in pairs if i), w.name(k)))
    res.sort(reverse=True)
    print("  most discriminating:")
    for n, m, nm in res[:12]:
        print("    %3d distinct / %3d materials   %s" % (n, m, nm))
    c = Counter(n for n, _, _ in res)
    print("  distribution over %d sets:" % len(res))
    for n in sorted(c):
        print("    %3d distinct sounds : %4d sets" % (n, c[n]))
    print("  materials never given an impact anywhere: %s"
          % [v for v in w.edid.values() if v.startswith('Material') and v not in usage])


def sndr_wavs(w, k):
    if k not in w.recs['SNDR']:
        return []
    return [zs(d) for d in w.recs['SNDR'][k][2].all('ANAM')]


def ipct_sounds(w, k):
    if k not in w.recs['IPCT']:
        return ["(missing IPCT)"]
    out = []
    for t in ('SNAM', 'NAM1'):
        sk = w.fid(w.recs['IPCT'][k], t)
        if sk:
            wav = sndr_wavs(w, sk)
            out.append(w.name(sk) + (" (%d wav, e.g. %s)" % (len(wav), wav[0]) if wav else ""))
    return out or ["(no sound)"]


def report_sets(w, names, header):
    print("\n=== %s ===" % header)
    bn = w.by_name()
    for setname in names:
        k = bn.get(setname)
        if not k or k not in w.recs['IPDS']:
            print("  %s: NOT FOUND" % setname)
            continue
        pairs = ipds_pairs(w, k)
        grouped = defaultdict(list)
        for m, i in pairs:
            if i:
                grouped[w.name(i)].append(w.name(m))
        print("  %s - %d materials -> %d distinct impacts"
              % (setname, sum(1 for _, i in pairs if i), len(grouped)))
        for impact, mats in sorted(grouped.items(), key=lambda x: -len(x[1])):
            print("     %-34s <- %2d  %s" % (impact, len(mats), ", ".join(sorted(mats)[:6])
                                             + (" ..." if len(mats) > 6 else "")))
            for s in ipct_sounds(w, w.by_name()[impact]):
                print("         %s" % s)


def report_ltex(w):
    print("\n=== 6. LTEX (landscape texture) -> MATT : the terrain resolver ===")
    c = Counter()
    rows = []
    for k, t in w.recs['LTEX'].items():
        mk = w.fid(t, 'MNAM')
        mat = w.name(mk) if mk else None
        rows.append((w.edid.get(k, '?'), mat))
        c[mat] += 1
    print("  total LTEX: %d -> %d distinct materials" % (len(rows), len(c)))
    for m, n in c.most_common():
        print("    %4d  %s" % (n, m))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--data', default=DEFAULT_DATA)
    ap.add_argument('--enum', default=DEFAULT_ENUM)
    a = ap.parse_args()

    w = World(a.data)
    print("loaded: " + ", ".join("%s=%d" % (s, len(w.recs[s])) for s in sorted(World.SIGS)))
    enum = load_enum(a.enum)

    report_hash(w, enum)
    report_matt(w, enum)
    report_discrimination(w)
    report_sets(w, ['PHYBodyMedium', 'PHYBodyLargeImpactSet', 'PHYBodySmallImpactSet',
                    'PHYBodyBones', 'PHYBodyMetalLargeImpactSet', 'PHYBodyMetalSmallImpactSet'],
                "4. The ragdoll path")
    report_sets(w, ['DefaultFootstepWalkLImpactset', 'FSTWalkArmorHeavyLImpactSet'],
                "5. The footstep path - the real 13-surface palette")
    report_ltex(w)


if __name__ == '__main__':
    main()
