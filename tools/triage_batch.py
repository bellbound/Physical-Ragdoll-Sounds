#!/usr/bin/env python3
"""triage_batch.py -- turn a folder of raw generations into staged one-shots.

    python tools/triage_batch.py [src_dir]        # default: ~/Downloads

What it does, in one pass:

  1. Finds every *contact* in each take -- a real attack rise, not decay ripple.
     Onsets must be 46 ms apart, the reference rate floor from
     04-Reference-Analysis.md section 2.
  2. Splits multi-contact takes into one one-shot per contact.
  3. Drops "satellite" contacts more than SAT_DB under the take's hero contact,
     whether they sit before or after it. Generators routinely bolt a quiet
     second event onto an otherwise clean take; that event lands on top of the
     next layer in the stack and reads as a flam.
  4. For the gore / crunch material also writes a LONG cut spanning every
     non-satellite contact, next to the SHORT one-shots, because that family is
     wanted both ways.
  5. Handles the sliding / dragging / rustling family two ways, never splitting it:
       -loop  the 2-2.5 s window with the smallest seam, for whole-file looping
       -long  the whole take, trimmed and faded, as a plain long one-shot
     A scrape is not only a loop. Slots.md section 6 makes `Looping` a per-slot
     attribute of the *assignment*, so the same drag can ship as a one-shot in a
     slot with `Looping = 0` -- and a long one-shot has no seam to give it away.

Cuts land in takes/_split/ with the generator's prompt prefix intact so
`sfx.py eval` can still infer the slot, plus a manifest CSV next to takes/.
Cut bounds are the zero-crossing-snapped sample indices from the onset pass:
starting at the raw onset index opens the file mid-waveform, and that step
discontinuity reads as a click that pushes the measured centroid up several kHz.

Nothing here writes to assets/sfx/. Run `sfx.py eval` on the output, then
`sfx.py make --slot <slot>` on what you keep.
"""
import os, sys, glob, wave, csv, re, hashlib
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import sfx

SRC = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser("~/Downloads")
OUT = os.path.join(ROOT, "takes", "_split")
MANIFEST = os.path.join(ROOT, "takes", "batch-manifest.csv")

MIN_GAP_MS = 46.0    # reference rate floor; below this two contacts stop resolving
RISE_DB    = 9.0     # an attack must climb this much over the preceding window
RISE_WIN   = 24.0    # ms
FLOOR_DB   = 34.0    # ignore contacts more than this far under the file peak
SAT_DB     = 20.0    # ... and treat these as satellites rather than the sound
PAD_MS     = 8.0     # tail padding so a cut does not clip its own decay
TRIM_DB    = 40.0    # long-variant trim: keep everything within this of the peak

WET   = ("wet_slap", "squelch", "crushing", "twisting", "bone_crack", "crunsh_gran")
# textures: judged whole, never split into contacts
LOOPY = {"dragged_slowly": "scrape_loop", "glides_slowly": "scrape_loop",
         "sliding": "scrape_loop", "rustle": "foley_cloth"}


def write_wav(path, x, sr):
    x = np.clip(x, -1.0, 1.0)
    w = wave.open(path, "wb")
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr)
    w.writeframes((x * 32767.0).astype("<i2").tobytes())
    w.close()


def slug(name):
    s = re.sub(r"^Firefly_audio_", "", os.path.splitext(name)[0])
    return re.sub(r"[^A-Za-z0-9]+", "_", s).strip("_")


def contacts(x, sr):
    """Every real contact: (start, end) in samples, plus level under the hero."""
    e, hop = sfx.env_db(x, sr, 2.0)
    if len(e) < 8:
        return []
    top = e.max()
    win = max(1, int(RISE_WIN / 2.0))
    cand = [i for i in range(win, len(e) - 2)
            if e[i] - e[max(0, i - win):i].min() >= RISE_DB
            and e[i] > top - FLOOR_DB and e[i] >= e[i - 1]]
    gap = int(MIN_GAP_MS * sr / 1000.0 / hop)
    groups = []
    for i in cand:
        if groups and i - groups[-1][-1] <= gap:
            groups[-1].append(i)
        else:
            groups.append([i])
    ons = [g[0] for g in groups]
    # Level a contact against the loudest *contact*, not the file peak. A take whose
    # peak sits somewhere no onset was detected -- a slow swell, a tail -- would
    # otherwise read as all-satellites and get dropped whole.
    hero = max((e[o + int(np.argmax(e[o:max(o + 1, (ons[i + 1] if i + 1 < len(ons)
                else len(e) - 1))]))]
                for i, o in enumerate(ons)), default=top)
    out = []
    for n, o in enumerate(ons):
        nxt = ons[n + 1] if n + 1 < len(ons) else len(e) - 1
        pk = o + int(np.argmax(e[o:max(o + 1, nxt)]))
        s = max(0, (o - 2)) * hop
        while s > 0 and not (x[s - 1] <= 0 < x[s] or x[s - 1] >= 0 > x[s]):
            s -= 1
        j, quiet, need = pk, 0, int(30 * sr / 1000.0 / hop)
        while j < nxt - 1:
            j += 1
            quiet = quiet + 1 if e[j] < e[pk] - 40 else 0
            if quiet >= need:
                break
        out.append(dict(s=int(s), e=int(min(j * hop, len(x))),
                        t=o * hop / sr * 1000.0, rel=float(e[pk] - hero)))
    return out


def seam_db(seg, sr):
    k = int(0.05 * sr)
    a = 20 * np.log10(np.sqrt(np.mean(seg[:k] ** 2)) + 1e-12)
    b = 20 * np.log10(np.sqrt(np.mean(seg[-k:] ** 2)) + 1e-12)
    return abs(a - b)


def best_window(x, sr, win_s):
    """Sweep the start for the smallest seam -- how scrape_loop_01 got to 0.02 dB."""
    n = int(win_s * sr)
    if len(x) <= n:
        return 0, len(x)
    best, bd = (0, n), 1e9
    for s in range(0, len(x) - n, int(0.02 * sr)):
        d = seam_db(x[s:s + n], sr)
        if d < bd:
            best, bd = (s, s + n), d
    return best


def trim_span(x, sr):
    """First and last sample within TRIM_DB of the peak, plus 20 ms either side."""
    e, hop = sfx.env_db(x, sr, 2.0)
    if not len(e):
        return 0, len(x)
    live = np.where(e > e.max() - TRIM_DB)[0]
    if not len(live):
        return 0, len(x)
    pad = int(20 * sr / 1000.0)
    return max(0, live[0] * hop - pad), min(len(x), live[-1] * hop + pad)


def norm(seg, sr, fade_out=6.0, fade_in=0.0):
    seg = np.array(seg, dtype=np.float64)
    seg -= seg.mean()                                  # kill the generator's DC
    seg = seg / max(abs(seg).max(), 1e-9) * 0.84       # -1.5 dBFS; make renormalises
    for ms, sl in ((fade_in, "in"), (fade_out, "out")):
        n = int(ms * sr / 1000.0)
        if n and len(seg) > n:
            ramp = np.linspace(0, 1, n)
            if sl == "in":
                seg[:n] *= ramp
            else:
                seg[-n:] *= ramp[::-1]
    return seg


def main():
    os.makedirs(OUT, exist_ok=True)
    rows, seen = [], {}
    for path in sorted(glob.glob(os.path.join(SRC, "*.wav"))):
        name = os.path.basename(path)
        with open(path, "rb") as fh:
            h = hashlib.md5(fh.read()).hexdigest()
        if h in seen:
            print(f"  dup   {name[:62]}  == {seen[h][:44]}, skipped")
            continue
        seen[h] = name
        x, sr, nch, bits, corr = sfx.load(path)
        base, pad = slug(name), int(PAD_MS * sr / 1000.0)
        loop = next((v for k, v in LOOPY.items() if k in name.lower()), None)

        def emit(tag, s, e, kind, note, fo=6.0, fi=0.0):
            s, e = max(0, int(s)), min(len(x), int(e) + pad)
            if e - s < 256:
                return
            p = os.path.join(OUT, f"{base}-{tag}.wav")
            write_wav(p, norm(x[s:e], sr, fo, fi), sr)
            rows.append(dict(cut=os.path.basename(p), source=name, kind=kind,
                             in_ms=round(s / sr * 1000), out_ms=round(e / sr * 1000),
                             len_ms=round((e - s) / sr * 1000),
                             inferred_slot=sfx.infer_slot(os.path.basename(p)) or "",
                             corr=round(corr, 2), note=note))

        if loop:
            # a texture is wanted both ways: as a seamless loop, and as a plain long
            # one-shot for a slot assigned Looping = 0
            win = 2.0 if loop == "scrape_loop" else 2.5
            s, e = best_window(x, sr, win)
            emit("loop", s, e, "loop", f"{loop}; window swept for seam", fo=0.0)
            ls, le = trim_span(x, sr)
            emit("long", ls, le, "long", f"{loop}; full take, not seam-matched",
                 fo=12.0, fi=8.0)
            continue

        cs = contacts(x, sr)
        keep = [c for c in cs if c["rel"] > -SAT_DB]
        drop = [c for c in cs if c["rel"] <= -SAT_DB]
        if not keep:
            print(f"  none  {name[:62]}  no contact above the floor")
            continue
        note = "; ".join(f"satellite @{c['t']:.0f}ms {c['rel']:+.0f}dB" for c in drop)
        if len(keep) == 1:
            emit("s01", keep[0]["s"], keep[0]["e"], "single", note)
        else:
            for i, c in enumerate(keep, 1):
                emit(f"s{i:02d}", c["s"], c["e"], "short", note)
            if any(w in name.lower() for w in WET):
                emit("long", keep[0]["s"], keep[-1]["e"], "long", note)

    with open(MANIFEST, "w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=["cut", "source", "kind", "in_ms", "out_ms",
                                           "len_ms", "inferred_slot", "corr", "note"])
        w.writeheader()
        w.writerows(rows)
    print(f"\n{len(rows)} cuts -> {OUT}")
    print(f"manifest -> {MANIFEST}")
    print(f"next: python tools/sfx.py eval {OUT} --suggest")


if __name__ == "__main__":
    main()
