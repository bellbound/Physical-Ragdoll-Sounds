#!/usr/bin/env python3
"""
sfx.py — evaluate and post-process generated SFX takes for Physical Ragdoll Sounds.

Specs come from 02-SFX-Generation-Prompts.md, which derives them from the measurements
in 04-Reference-Analysis.md. Two subcommands:

    python sfx.py eval  <file|dir> [--slot SLOT] [--detail] [--csv OUT]
    python sfx.py make  <file> --slot SLOT --var N [--out DIR] [--start MS] [--len MS]

`eval` judges the *event inside* the file, not the raw container. Pre-roll, tails, sample
rate and stereo are reported as FIX (the post-pass handles them) and never sink a take.
Character — band tilt, decay, transient density — is what decides KEEP vs RETRY.

`make` applies the post-pass and writes a shippable 44.1 kHz / mono / 16-bit file.

Requires numpy and ffmpeg on PATH. No scipy.
"""

import argparse
import csv
import datetime
import hashlib
import os
import shutil
import re
import subprocess
import sys
import wave

import numpy as np

BANDS = [
    ("sub", 20, 80),
    ("low", 80, 250),
    ("lomid", 250, 800),
    ("mid", 800, 2500),
    ("high", 2500, 8000),
    ("air", 8000, 20000),
]

# ---------------------------------------------------------------- slot specs

# len:      usable event length the slot wants, ms
# hpf:      post-pass high-pass, Hz (0 = none)
# decay20:  ms from peak to -20 dB, allowed range
# centroid: Hz, allowed range for the event window
# peaks_lomid / peaks_high: transient count in that band across the event
# peak_dbfs: normalisation target
# loop:     evaluated as a sustained texture rather than a one-shot
# tilt:     required (low bands rms) - (high bands rms) in dB, min
# NOTE ON CENTROID: it is a weak discriminator and generous limits here are deliberate.
# The four Skate 3 reference hero hits measure 1950-3306 Hz centroid while being clearly
# bass-led (tilt +6.5 to +21.6 dB) -- there are simply more FFT bins up top, the trap
# 04-Reference-Analysis.md §7 flags for the scrape. Tilt is what actually separates a body
# layer from a transient; centroid only catches gross errors. Ranges below are calibrated
# against Example/*.wav, so a spec must never reject the references themselves.
SPEC = {
    "imp_transient": dict(len=(60, 120), hpf=400, decay20=(5, 90), centroid=(1500, 9000),
                          peaks_lomid=(0, 6), tilt_max=-6, hf_loss=8, peak_dbfs=-1.5),
    "imp_body":      dict(len=(150, 250), hpf=120, decay20=(15, 200), centroid=(800, 4200),
                          peaks_lomid=(0, 5), tilt=4, peak_dbfs=-1.5),
    # Declared with the slot rather than after it, so limb takes are never graded
    # against the torso's numbers -- which is exactly what happened to scrape_limb
    # (see its note below) and is the reason this row exists before a single file
    # does. Shorter and higher than imp_body, and the tilt is the load-bearing
    # difference: imp_body wants tilt >= +4 because a torso is bass-led, so this
    # takes the same number as a *ceiling*. A take that passes imp_body's bass
    # test is a torso take, whatever it was recorded for.
    #
    # UNMEASURED. Every other row here was calibrated against Example/*.wav or
    # against real takes; this one is read off the brief in Slots.md and the
    # manifest's own length, and should be re-derived from the first three real
    # recordings rather than trusted.
    "imp_body_limb": dict(len=(120, 200), hpf=120, decay20=(10, 160), centroid=(1200, 5200),
                          peaks_lomid=(0, 5), tilt_max=4, peak_dbfs=-1.5),
    "imp_sub":       dict(len=(250, 400), hpf=0, decay20=(15, 120), centroid=(20, 400),
                          peak_dbfs=-1.0),
    "surf_wood":     dict(len=(120, 200), hpf=120, decay20=(20, 160), centroid=(400, 3000),
                          peak_dbfs=-1.5),
    "surf_stone":    dict(len=(100, 160), hpf=120, decay20=(5, 90), centroid=(1200, 9000),
                          tilt_max=-2, peak_dbfs=-1.5),
    "surf_soft":     dict(len=(150, 250), hpf=120, decay20=(15, 200), centroid=(300, 3200),
                          tilt=3, peak_dbfs=-1.5),
    # Dirt is the one surface whose numbers came from a reference rather than from
    # the brief, and the reference contradicted the brief: a real dirt contact
    # measures -9.3 dB tilt at a 5010 Hz centroid, -20 dB in 26 ms. That is
    # *brighter* than surf_soft and only a few dB off surf_stone - packed earth is
    # grain, and the "duller than soft, no grain on top" line in Slots.md had it
    # backwards. Judged against soft's `tilt=3` the reference itself is a RETRY,
    # so dirt needs its own row or a correct take can never pass.
    "surf_dirt":     dict(len=(150, 250), hpf=120, decay20=(10, 60), centroid=(3000, 7000),
                          tilt_max=-5, peak_dbfs=-1.5),
    # The armour skins. No reference measurement behind these yet - the bands are
    # the manifest's lengths and the character line's intent - so `eval` judges a
    # new armour file against a brief rather than against a reference, and the
    # numbers should be revisited the first time one of these is recorded.
    "armor_bare":    dict(len=(80, 200), hpf=120, decay20=(10, 150), centroid=(300, 3000),
                          peak_dbfs=-1.5),
    "armor_cloth":   dict(len=(100, 250), hpf=120, decay20=(15, 200), centroid=(300, 4000),
                          peak_dbfs=-1.5),
    "armor_light":   dict(len=(100, 250), hpf=150, decay20=(15, 250), centroid=(800, 6000),
                          peak_dbfs=-1.5),
    "armor_heavy":   dict(len=(120, 300), hpf=150, decay20=(20, 300), centroid=(1200, 8000),
                          tilt=-2, peak_dbfs=-1.5),
    "limb_tap":      dict(len=(40, 100), hpf=200, decay20=(3, 60), centroid=(600, 9000),
                          peaks_lomid=(0, 4), peak_dbfs=-1.5),
    "scrape_grain":  dict(len=(150, 500), hpf=120, decay20=(40, 400), centroid=(300, 6000),
                          peaks_lomid=(6, 40), peak_dbfs=-1.5),
    "crunch_gran":   dict(len=(250, 400), hpf=120, decay20=(30, 400), centroid=(500, 4500),
                          peaks_lomid=(15, 99), peak_dbfs=-1.5),
    "gore_wet":      dict(len=(200, 400), hpf=120, decay20=(20, 350), centroid=(400, 4000),
                          peak_dbfs=-1.5),
    "head_impact":   dict(len=(300, 500), hpf=120, decay20=(20, 300), centroid=(400, 3500),
                          peaks_lomid=(4, 14), tilt=2, peak_dbfs=-1.5),
    "settle_rest":   dict(len=(200, 400), hpf=120, decay20=(20, 400), centroid=(300, 3200),
                          soft_attack=10, peak_dbfs=-1.5),
    # Calibrated against the 2.1 s stone slide at the end of
    # Example/heavy-impact-tumble-and-slide.wav: tilt +11.2, 17 grains/s, 5.0 dB steadiness,
    # bands sub +0 low -3 lomid -4 mid -4 high -7 air -18. Note how FLAT that is -- only the
    # air band is really down. "Low-tilted" in 04-Reference-Analysis.md §7 means "not a
    # hiss", not "no mid content"; a pure low rumble with lomid 20+ dB down is as wrong as
    # a hiss, in the other direction. The old grains=(35,95) came from the analysis's prose
    # count of 65/s, which used a different threshold than count_peaks here.
    "scrape_loop":   dict(len=(1500, 3000), hpf=0, loop=True, tilt=5, tilt_max=19,
                          grains=(8, 40), steady=(2.5, 8.0), peak_dbfs=-1.5),
    # Slots.md section 3, Loops. scrape_limb is NOT scrape_loop turned down: a small
    # contact patch, so the low shelf comes off (tilt ceiling, no floor) and the grit
    # rate roughly doubles. It was declared in SlotManifest.cpp and Slots.md but had no
    # SPEC entry, so every limb-drag take could only be graded against scrape_loop --
    # whose +5 tilt floor and 8-40 grain band a limb scrape is designed to fail.
    "scrape_limb":   dict(len=(1500, 3000), hpf=0, loop=True, tilt_max=8,
                          grains=(25, 120), steady=(2.0, 8.0), peak_dbfs=-1.5),
    # The surface variants take their base slot's numbers unchanged; only the colour
    # differs, and each declares its base as a fallback in SlotManifest.cpp.
    "scrape_body_wood":  dict(len=(1500, 3000), hpf=0, loop=True, tilt=5, tilt_max=19,
                              grains=(8, 40), steady=(2.5, 8.0), peak_dbfs=-1.5),
    "scrape_body_stone": dict(len=(1500, 3000), hpf=0, loop=True, tilt=5, tilt_max=19,
                              grains=(8, 40), steady=(2.5, 8.0), peak_dbfs=-1.5),
    "scrape_limb_wood":  dict(len=(1500, 3000), hpf=0, loop=True, tilt_max=8,
                              grains=(25, 120), steady=(2.0, 8.0), peak_dbfs=-1.5),
    "scrape_limb_stone": dict(len=(1500, 3000), hpf=0, loop=True, tilt_max=8,
                              grains=(25, 120), steady=(2.0, 8.0), peak_dbfs=-1.5),
    # The bed, and the one loop here graded against the *opposite* of the grinds' tilt
    # window. A grind is asked to have a low shelf at all (+5 to +19, "not a hiss"); this
    # is asked to have almost nothing else, which is only coherent because it never plays
    # alone -- it is one voice under a grind that supplies the mid and the top, and the
    # pair is what should land on GTA 4's +10 to +21.
    #
    # No `grains`. Counting grit peaks in the layer whose entire definition is having none
    # would fail every correct file, and the density it would be measuring belongs to the
    # grind. `steady` is air_whoosh's band rather than a grind's, for the same reason the
    # whoosh has it: a bump in a featureless loop becomes a pulse the moment it repeats,
    # and there is no grit here to hide one under.
    #
    # Longer than 3 s is allowed, unlike every other loop, because the bed rides on its own
    # voice under the grinds and a length that shares a period with theirs stacks the two
    # seams -- see the note on VARIANTS in tools/make_rumble.py.
    "scrape_loop_rumble": dict(len=(1500, 4000), hpf=0, loop=True, tilt=20,
                               centroid=(20, 1200), steady=(0.5, 6.0), peak_dbfs=-1.5),
}

# First 20 characters of each prompt in 02-SFX-Generation-Prompts.md, which is what the
# generators use as the download filename. Lets `eval <dir>` infer slots with no flags.
PROMPT_HEADS = {
    "sharp dry slap of a": "imp_transient", "hard knuckle crack a": "imp_transient",
    "one hard dry knock o": "imp_transient",
    "one hard dry crack o": "imp_transient",
    "heavy side of raw me": "imp_body", "juicy heavy side of": "imp_body", "heavy punching bag f": "imp_body",
    "large sack of wet fl": "imp_body", "heavy booming thud o": "imp_body", "sandbag full of wet": "imp_body",
    "weighted leather duf": "imp_body",
    "single hollow knock": "surf_wood", "heavy thud on old cr": "surf_wood",
    "hard flat slap again": "surf_stone", "heavy object strikin": "surf_stone",
    "dull muffled thump i": "surf_soft", "heavy weight landing": "surf_soft",
    "light quick tap of a": "limb_tap", "small dull knock of": "limb_tap",
    "soft scuff tap of a": "limb_tap", "quiet slap of loose": "limb_tap",
    "slow crushing of a b": "crunch_gran", "twisting a handful o": "crunch_gran",
    "wet squelch of raw m": "gore_wet", "heavy wet slap of so": "gore_wet",
    # 2026-08-23 Firefly batch. The two stone heads name the *prompt's* slot, not the
    # take's: all four stone takes came back bass-led (tilt +7.8 to +17.7 against
    # surf_stone's -2 ceiling) and are staged for imp_body / head_impact instead --
    # see 03-Asset-Status.md section 7. `make --slot` overrides the inference anyway.
    "a heavy wet slap of ": "gore_wet", "a wet thick squelch ": "gore_wet",
    "hard flat impact aga": "surf_stone", "hard flat impact of ": "surf_stone",
    "quick impact crushin": "crunch_gran",
    "a heavy object glide": "scrape_loop",
    "a dull heavy thump a": "imp_body",
    # 17:19-17:30 arrivals
    "single hard knuckle ": "imp_transient",
    "a quick light forear": "limb_tap", "a soft short boot he": "limb_tap",
    "naked body sliding a": "scrape_loop", "heavy body violently": "scrape_loop",
    # "hitmarker" is not a slot; the prompt asks for a short bright crack, which is
    # imp_transient's job. Graded there so it is judged rather than skipped.
    "hitmarker sound sing": "imp_transient",
    # 22:03-22:15 arrivals
    "series of light limb": "limb_tap", "quick light tap of a": "limb_tap",
    "heavy forearm droppe": "limb_tap",
    "side of raw meat dro": "imp_body", "leg of raw meat drop": "imp_body",
    "a dry meaty damp cut": "imp_body",
    "small sandbag of dam": "imp_body", "small sandbag of dro": "imp_body",
    # "fish slap" is the classic wet-slap reference, but all four takes measure
    # bright (4923-6813 Hz, tilt -10 to -14) -- that is a contact, not a body.
    "fish slap variation": "imp_transient",
    # hand-named files with no prompt behind them; graded to where they measure
    "bone crack impact co": "imp_body",
    "heavy body dragged s": "scrape_loop", "body dragged slowly": "scrape_loop",
    "heavy limp body drag": "scrape_loop",
    "human body dragged s": "scrape_loop",
    "low soft air movemen": "air_whoosh",
    "heavy melon wrapped": "head_impact", "hard blunt blow to a": "head_impact",
    "heavy limp body sett": "settle_rest", "loose fabric and a h": "settle_rest",
    # 23:36-23:39 arrivals. The damped-wood prompt from 02-SFX-Generation-Prompts.md
    # section 4 - solid/rotten timber with no cavity behind it, which is meant to
    # measure between surf_wood's hollow knock and surf_soft's dead thump.
    "dull impact on damp ": "surf_wood", "dull thump on damp r": "surf_wood",
}


def infer_slot(path):
    """Slot from a `<slot>_NN.wav` name, else from the generator's prompt-prefix name."""
    stem = os.path.splitext(os.path.basename(path))[0]
    # Longest match wins: scrape_limb is a prefix of scrape_limb_wood/_stone, so a
    # first-match scan files every surface variant under the base slot.
    hit = max((s for s in SPEC if stem.startswith(s)), key=len, default=None)
    if hit:
        return hit
    key = re.sub(r"[^a-z0-9]+", " ", stem.lower()).strip()[:20].rstrip()
    for head, slot in PROMPT_HEADS.items():
        if key.startswith(head[:len(key)]) or head.startswith(key[:len(head)]):
            return slot
    return None


# ---------------------------------------------------------------- dsp helpers

def load(path):
    w = wave.open(path, "rb")
    nch, sw, sr, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
    raw = w.readframes(n)
    w.close()
    if sw not in (2, 4):
        raise ValueError(f"unsupported sample width {sw*8}-bit")
    x = np.frombuffer(raw, dtype={2: np.int16, 4: np.int32}[sw]).astype(np.float64)
    x /= float(2 ** (8 * sw - 1))
    x = x.reshape(-1, nch)
    corr = float(np.corrcoef(x[:, 0], x[:, 1])[0, 1]) if nch == 2 else 1.0
    # Uncorrelated channels comb-filter when summed. Take the left channel instead: the
    # references are point sources (0.95-0.97 correlation) so nothing is lost by it.
    mono = x[:, 0] if (nch == 2 and corr < 0.6) else x.mean(axis=1)
    return mono, sr, nch, sw * 8, corr


def env_db(x, sr, ms=2.0):
    hop = max(1, int(sr * ms / 1000.0))
    n = (len(x) - hop) // hop
    e = np.sqrt(np.array([np.mean(x[i * hop:i * hop + hop] ** 2) for i in range(max(n, 1))]))
    return 20 * np.log10(np.maximum(e, 1e-9)), hop


def band(x, sr, lo, hi):
    """Zero-phase band split, matching the method in 04-Reference-Analysis.md."""
    n = len(x)
    F = np.fft.rfft(x)
    f = np.fft.rfftfreq(n, 1.0 / sr)
    F[(f < lo) | (f >= hi)] = 0
    return np.fft.irfft(F, n)


def hpf(x, sr, fc):
    """Zero-phase 2-pole Butterworth magnitude. Judge character on the shipped signal."""
    if not fc:
        return x
    n = len(x)
    F = np.fft.rfft(x)
    r = np.fft.rfftfreq(n, 1.0 / sr) / float(fc)
    F *= (r ** 2) / np.sqrt(1.0 + r ** 4)
    return np.fft.irfft(F, n)


def count_peaks(y, sr, floor_db=20.0, min_gap_ms=10.0):
    e, hop = env_db(y, sr)
    if not len(e):
        return 0
    thr = e.max() - floor_db
    gap = min_gap_ms * sr / 1000.0 / hop
    cnt, last = 0, -1e9
    for i in range(1, len(e) - 1):
        if e[i] > thr and e[i] >= e[i - 1] and e[i] > e[i + 1] and (i - last) > gap:
            cnt += 1
            last = i
    return cnt


def find_event(x, sr):
    """Attack / peak / end of the loudest event, ignoring pre-roll and room tail."""
    e, hop = env_db(x, sr)
    pk = int(np.argmax(e))
    peak = e[pk]
    i = pk
    while i > 0 and e[i] > peak - 35:
        i -= 1
    # snap the start to a zero crossing so the file opens without a click
    s = i * hop
    while s > 0 and not (x[s - 1] <= 0 < x[s] or x[s - 1] >= 0 > x[s]):
        s -= 1
    j = pk
    quiet = 0
    need = int(30 * sr / 1000.0 / hop)
    while j < len(e) - 1:
        j += 1
        quiet = quiet + 1 if e[j] < peak - 40 else 0
        if quiet >= need:
            break
    return s, pk * hop, min(j * hop, len(x))


# ---------------------------------------------------------------- measurement

def measure(path, slot):
    x, sr, nch, bits, corr = load(path)
    spec = SPEC.get(slot, {})
    m = dict(file=os.path.basename(path), path=path, slot=slot, sr=sr, nch=nch, bits=bits,
             corr=corr, dur=len(x) / sr * 1000.0, dc=float(x.mean()),
             peak=20 * np.log10(max(np.abs(x).max(), 1e-9)))

    if spec.get("loop"):
        seg = x
        m["attack"] = 0.0
        m["usable"] = m["dur"]
        e, _ = env_db(seg, sr, 10.0)
        live = e[e > e.max() - 40]
        m["steady"] = float(np.std(live)) if len(live) else 0.0
        m["grains"] = count_peaks(seg, sr, 12.0, 8.0) / (len(seg) / sr)
        # seam: level difference between the first and last 50 ms
        k = int(0.05 * sr)
        a = 20 * np.log10(np.sqrt(np.mean(seg[:k] ** 2)) + 1e-12)
        b = 20 * np.log10(np.sqrt(np.mean(seg[-k:] ** 2)) + 1e-12)
        m["seam"] = abs(a - b)
        m["max_transient"] = float(e.max() - np.median(live)) if len(live) else 0.0
    else:
        s, p, en = find_event(x, sr)
        seg = x[s:en]
        m["attack"] = s / sr * 1000.0
        m["usable"] = (en - s) / sr * 1000.0
        m["rise"] = (p - s) / sr * 1000.0
        e, hop = env_db(seg, sr)
        pk = int(np.argmax(e))
        m["decay20"] = next((((i - pk) * hop / sr * 1000.0)
                             for i in range(pk, len(e)) if e[i] <= e[pk] - 20), None)
        m["decay40"] = next((((i - pk) * hop / sr * 1000.0)
                             for i in range(pk, len(e)) if e[i] <= e[pk] - 40), None)
        # noise floor: the quietest 100 ms anywhere before the attack
        pre = x[:s]
        if len(pre) > int(0.1 * sr):
            k = int(0.05 * sr)
            fl = min(np.sqrt(np.mean(pre[i:i + k] ** 2))
                     for i in range(0, len(pre) - k, k))
            m["snr"] = m["peak"] - 20 * np.log10(max(fl, 1e-12))
        else:
            m["snr"] = 99.0
        # a second contact after the main one, which would collide with imp_sub at +55-75 ms
        after = e[pk + int(25 * sr / 1000.0 / hop):]
        if len(after):
            k = int(np.argmax(after))
            m["second"] = (25 + k * hop / sr * 1000.0, float(after[k] - e[pk]))
        else:
            m["second"] = None

    # Character is judged on what will actually ship, i.e. after the slot's high-pass.
    # Centroid alone cannot tell a bright contact from a bass-led thud (01-Reference
    # -Analysis.md §7: there are simply more bins up top), so tilt is measured too.
    raw_pk = max(np.abs(seg).max(), 1e-9) if len(seg) else 1e-9
    # A built pack file has already been high-passed by `make`; filtering again would
    # drop its low end a second time and read as a tilt failure it does not have.
    already = bool(re.match(r"^[a-z_]+_\d{2}\.wav$", m["file"]))
    seg = seg if already else hpf(seg, sr, spec.get("hpf", 0))
    m["hf_loss"] = 20 * np.log10(raw_pk / max(np.abs(seg).max(), 1e-9)) if len(seg) else 0.0

    if len(seg) > 64:
        e2, hop2 = env_db(seg, sr)
        if not spec.get("loop") and len(e2):
            p2 = int(np.argmax(e2))
            m["decay20"] = next((((i - p2) * hop2 / sr * 1000.0)
                                 for i in range(p2, len(e2)) if e2[i] <= e2[p2] - 20), None)
        S = np.abs(np.fft.rfft(seg))
        f = np.fft.rfftfreq(len(seg), 1.0 / sr)
        m["centroid"] = float(np.sum(S * f) / max(np.sum(S), 1e-12))
        lv = {}
        for nm, lo, hi in BANDS:
            y = band(seg, sr, lo, min(hi, sr // 2 - 1))
            lv[nm] = 20 * np.log10(np.sqrt(np.mean(y ** 2)) + 1e-12)
            if nm in ("lomid", "high"):
                m["peaks_" + nm] = count_peaks(y, sr)
        m["bands"] = lv
        m["tilt"] = (lv["sub"] + lv["low"]) / 2 - (lv["high"] + lv["air"]) / 2
    return m


# ---------------------------------------------------------------- judgement

def check(m):
    """-> (verdict, [(level, text)]). FAIL sinks a take; FIX is the post-pass's job."""
    spec = SPEC.get(m["slot"], {})
    out = []

    def fix(c, t):
        out.append(("FIX" if c else "OK", t))

    def fail(c, t):
        out.append(("FAIL" if c else "OK", t))

    fix(m["sr"] != 44100, f"{m['sr']} Hz, resample to 44100")
    fix(m["nch"] != 1, f"{m['nch']} ch, mono-sum (corr {m['corr']:.3f})")
    fail(m["nch"] == 2 and m["corr"] < 0.6,
         f"stereo corr {m['corr']:.2f} too low, mono-sum will phase-cancel")
    fix(abs(m["dc"]) > 1e-4, f"DC offset {m['dc']:+.5f}")
    fix(m["attack"] > 5, f"{m['attack']:.0f} ms of pre-roll to trim")
    fix(m["peak"] < spec.get("peak_dbfs", -1.5) - 0.5,
        f"peak {m['peak']:.1f} dBFS, normalise to {spec.get('peak_dbfs', -1.5)}")

    lo, hi = spec.get("len", (0, 1e9))
    if spec.get("loop"):
        fail(m["usable"] < lo * 0.6, f"only {m['usable']:.0f} ms, need {lo:.0f}+ for a loop")
        fix(m["seam"] > 6, f"loop seam mismatch {m['seam']:.1f} dB, crossfade needed")
        if "steady" in spec:
            s0, s1 = spec["steady"]
            fail(m["steady"] > s1, f"envelope std {m['steady']:.1f} dB, too lumpy for a bed")
            fail(m["steady"] < s0, f"envelope std {m['steady']:.1f} dB, too flat/synthetic")
        if "grains" in spec:
            g0, g1 = spec["grains"]
            fail(not (g0 <= m["grains"] <= g1),
                 f"{m['grains']:.0f} grains/s, want {g0}-{g1}")
        if "max_transient" in spec:
            fail(m["max_transient"] > spec["max_transient"],
                 f"transient {m['max_transient']:.1f} dB over the bed, must be smooth")
    else:
        fail(m["usable"] < lo * 0.75,
             f"only {m['usable']:.0f} ms of event, slot wants {lo:.0f}-{hi:.0f}")
        fix(m["usable"] > hi, f"{m['usable']:.0f} ms of event, trim to {hi:.0f}")
        fail(m.get("snr", 99) < 30, f"noise floor only {m['snr']:.0f} dB down")
        d = m.get("decay20")
        if d is not None and "decay20" in spec:
            d0, d1 = spec["decay20"]
            fail(not (d0 <= d <= d1), f"-20 dB in {d:.0f} ms, slot wants {d0}-{d1}")
        if spec.get("soft_attack") and m.get("rise", 99) < spec["soft_attack"]:
            fail(True, f"attack rises in {m['rise']:.0f} ms, this slot must be soft")
        sec = m.get("second")
        if sec and sec[1] > -22 and 40 < sec[0] < 90 and m["slot"].startswith("imp_"):
            out.append(("WARN", f"second contact at +{sec[0]:.0f} ms ({sec[1]:.0f} dB), "
                                f"collides with imp_sub"))

    if "centroid" in spec and "centroid" in m:
        c0, c1 = spec["centroid"]
        fail(not (c0 <= m["centroid"] <= c1),
             f"centroid {m['centroid']:.0f} Hz, slot wants {c0}-{c1}")
    if "tilt" in spec and "tilt" in m:
        fail(m["tilt"] < spec["tilt"],
             f"tilt {m['tilt']:+.1f} dB, must lean low by {spec['tilt']}+ (not a hiss)")
    if "tilt_max" in spec and "tilt" in m:
        fail(m["tilt"] > spec["tilt_max"],
             f"tilt {m['tilt']:+.1f} dB, too bass-led (max {spec['tilt_max']:+.0f}) — "
             + ("hollow, no mid content" if spec.get("loop") else "a body take, not a transient"))
    if "hf_loss" in spec and m.get("hf_loss", 0) > spec["hf_loss"]:
        fail(True, f"loses {m['hf_loss']:.0f} dB to the {spec['hpf']} Hz high-pass, "
                   f"not enough top end to survive the post-pass")
    for key in ("peaks_lomid", "peaks_high"):
        if key in spec and key in m:
            p0, p1 = spec[key]
            fail(not (p0 <= m[key] <= p1),
                 f"{m[key]} transients in {key.split('_')[1]}, slot wants {p0}-{p1}")

    bad = [t for lv, t in out if lv == "FAIL"]
    warn = [t for lv, t in out if lv == "WARN"]
    if bad:
        return "RETRY", out
    return ("KEEP*" if warn else "KEEP"), out


# ---------------------------------------------------------------- subcommands

LEDGER_COLS = ["md5", "slot", "verdict", "take", "archived", "used_as", "seen",
               "usable", "decay20", "centroid", "tilt", "hf_loss",
               "peaks_lomid", "peaks_high", "peak", "snr", "sr", "nch", "corr"]


def root():
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def md5(path):
    h = hashlib.md5()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def ledger_path():
    return os.path.join(root(), "takes", "ledger.csv")


def ledger_read():
    p = ledger_path()
    if not os.path.exists(p):
        return {}
    with open(p, newline="", encoding="utf-8") as fh:
        return {r["md5"]: r for r in csv.DictReader(fh)}


def ledger_write(rows):
    p = ledger_path()
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "w", newline="", encoding="utf-8") as fh:
        w = csv.DictWriter(fh, LEDGER_COLS, extrasaction="ignore")
        w.writeheader()
        for r in sorted(rows.values(), key=lambda r: (r["slot"], r["take"])):
            w.writerow(r)


def archive(m, verdict):
    """Copy a passing take into takes/<slot>/ and record its metrics. Idempotent:
    keyed on content hash, so re-running over the same folder never duplicates."""
    # Built pack files are outputs, not takes -- never file them as source material.
    if re.match(r"^[a-z_]+_\d{2}(_|\.)", m["file"]):
        return "output"
    led = ledger_read()
    h = md5(m["path"])
    prior = led.get(h)
    if prior and os.path.exists(os.path.join(root(), prior["archived"])):
        return "dup" if prior["take"] != m["file"] else "known"
    dest_dir = os.path.join(root(), "takes", m["slot"])
    os.makedirs(dest_dir, exist_ok=True)
    dest = os.path.join(dest_dir, m["file"])
    shutil.copy2(m["path"], dest)
    row = {k: m.get(k, "") for k in LEDGER_COLS}
    row.update(md5=h, slot=m["slot"], verdict=verdict, take=m["file"],
               archived=os.path.relpath(dest, root()).replace("\\", "/"),
               used_as=(prior or {}).get("used_as", ""),
               seen=datetime.date.today().isoformat())
    for k in ("usable", "decay20", "centroid", "tilt", "hf_loss", "peak", "snr", "corr"):
        if isinstance(row.get(k), float):
            row[k] = round(row[k], 2)
    led[h] = row
    ledger_write(led)
    return "new"


def best_fit(path, exclude):
    """Slots this take would pass if it were relabelled. Salvage for a failed take."""
    hits = []
    for slot in SPEC:
        if slot == exclude:
            continue
        try:
            v, _ = check(measure(path, slot))
        except Exception:
            continue
        if v.startswith("KEEP"):
            hits.append(slot)
    return hits


def cmd_eval(args):
    paths = []
    if os.path.isdir(args.target):
        for f in sorted(os.listdir(args.target)):
            if f.lower().endswith(".wav"):
                paths.append(os.path.join(args.target, f))
    else:
        paths = [args.target]

    rows = []
    for p in paths:
        slot = args.slot or infer_slot(p)
        if not slot:
            print(f"  ??   {os.path.basename(p)[:44]:44s}  unknown slot, pass --slot")
            continue
        try:
            m = measure(p, slot)
        except Exception as exc:
            print(f"  ERR  {os.path.basename(p)[:44]:44s}  {exc}")
            continue
        v, notes = check(m)
        m["verdict"] = v
        # Only archive under a slot the take was actually generated for. A forced
        # --slot is for probing fit, and must not file takes under the wrong slot.
        if args.archive and v.startswith("KEEP") and (
                not args.slot or infer_slot(p) == args.slot):
            m["archived_now"] = archive(m, v)
        rows.append((m, notes))

    if not rows:
        return 1
    rows.sort(key=lambda r: (r[0]["slot"], r[0]["verdict"] != "KEEP", r[0]["file"]))

    print(f"\n{'verdict':8s}{'slot':14s}{'file':40s}"
          f"{'event':>8s}{'-20dB':>8s}{'cent':>7s}{'hpfloss':>8s}{'pk lm/hi':>10s}")
    print("-" * 100)
    for m, _ in rows:
        d = m.get("decay20")
        print(f"{m['verdict']:8s}{m['slot']:14s}{m['file'][:38]:40s}"
              f"{m['usable']:7.0f}m{(f'{d:.0f}m' if d else '   -'):>8s}"
              f"{m.get('centroid', 0):7.0f}{m.get('hf_loss', 0):7.1f}d"
              f"{str(m.get('peaks_lomid', '-')) + '/' + str(m.get('peaks_high', '-')):>10s}")

    for m, notes in rows:
        issues = [(lv, t) for lv, t in notes if lv != "OK"]
        if not issues or (not args.detail and m["verdict"] == "KEEP"):
            continue
        print(f"\n{m['file']}  [{m['slot']}]  -> {m['verdict']}")
        for lv, t in issues:
            print(f"    {lv:5s} {t}")
        if m["verdict"] == "RETRY" and args.suggest:
            alt = best_fit(m["path"], m["slot"])
            if alt:
                print(f"    ALT   would pass as: {', '.join(alt)}")
        if args.detail and "bands" in m:
            b = m["bands"]
            top = max(b.values())
            print("    bands " + "  ".join(f"{k} {b[k]-top:+.0f}" for k, _, _ in BANDS))

    if args.csv:
        keys = ["file", "slot", "verdict", "usable", "decay20", "centroid", "tilt",
                "peaks_lomid", "peaks_high", "peak", "snr", "sr", "nch"]
        with open(args.csv, "w", newline="", encoding="utf-8") as fh:
            w = csv.writer(fh)
            w.writerow(keys)
            for m, _ in rows:
                w.writerow([m.get(k, "") for k in keys])
        print(f"\nwrote {args.csv}")

    keep = sum(1 for m, _ in rows if m["verdict"].startswith("KEEP"))
    print(f"\n{keep}/{len(rows)} keepable")
    return 0


def onsets(x, sr, rise_db=8.0, min_gap_ms=46.0, floor_db=32.0):
    """Onset times in samples. The 46 ms minimum gap is the reference rate floor from
    04-Reference-Analysis.md §2 -- below it two contacts stop resolving as separate
    events, so anything closer is one onset, not two."""
    e, hop = env_db(x, sr)
    if not len(e):
        return []
    thr = e.max() - floor_db
    look = max(1, int(12 * sr / 1000.0 / hop))
    gap = min_gap_ms * sr / 1000.0 / hop
    hits, last = [], -1e9
    for i in range(look, len(e) - 1):
        if e[i] < thr or e[i] < e[i + 1]:
            continue
        if e[i] - e[i - look:i].min() < rise_db:
            continue
        if i - last < gap:
            # keep the louder of two contacts inside the resolution floor
            if hits and e[i] > e[hits[-1]]:
                hits[-1], last = i, i
            continue
        hits.append(i)
        last = i
    out = []
    for i in hits:
        # walk back to the foot of the rise, then to a zero crossing
        j = i
        while j > 0 and e[j - 1] < e[j] and e[j] > thr - 12:
            j -= 1
        k = j * hop
        while k > 0 and not (x[k - 1] <= 0 < x[k] or x[k - 1] >= 0 > x[k]):
            k -= 1
        out.append(k)
    return out


def cmd_split(args):
    slot = args.slot or infer_slot(args.file)
    if not slot:
        print("cannot infer slot, pass --slot")
        return 1
    x, sr, nch, bits, corr = load(args.file)
    hits = onsets(x, sr)
    if len(hits) < 2:
        print(f"{os.path.basename(args.file)}: {len(hits)} onset(s), nothing to split")
        return 1
    out = args.out or os.path.join(root(), "takes", "_split")
    os.makedirs(out, exist_ok=True)
    stem = re.sub(r"[^A-Za-z0-9_#-]+", "_", os.path.splitext(os.path.basename(args.file))[0])
    maxlen = int(SPEC[slot]["len"][1] / 1000.0 * sr)

    made = []
    for n, s0 in enumerate(hits, 1):
        end = min(s0 + maxlen, hits[hits.index(s0) + 1] if s0 != hits[-1] else len(x), len(x))
        seg = x[s0:end]
        if len(seg) < int(0.02 * sr):
            continue
        seg = seg - seg.mean()
        fade = min(int(0.005 * sr), len(seg) // 4)
        seg[-fade:] *= np.cos(np.linspace(0, np.pi / 2, fade)) ** 2
        seg = seg / max(np.abs(seg).max(), 1e-9) * 0.9
        # Keep the generator's prompt prefix so slot inference still works on the pieces.
        dest = os.path.join(out, f"{stem}-s{n:02d}.wav")
        w = wave.open(dest, "wb")
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(sr)
        w.writeframes((seg * 32767).astype(np.int16).tobytes())
        w.close()
        made.append((dest, len(seg) / sr * 1000.0, s0 / sr * 1000.0))

    print(f"{os.path.basename(args.file)}: {len(made)} piece(s) from {len(hits)} onsets")
    for d, ln, at in made:
        print(f"  {os.path.basename(d):48s} +{at:6.0f} ms  {ln:5.0f} ms")
    return 0


def cmd_make(args):
    slot = args.slot or infer_slot(args.file)
    if not slot:
        print("cannot infer slot, pass --slot")
        return 1
    spec = SPEC[slot]
    x, sr, nch, bits, corr = load(args.file)

    ev_s, _, ev_e = (0, 0, len(x)) if spec.get("loop") else find_event(x, sr)
    if spec.get("loop") and args.start is None:
        # Pick the smoothest window. Score on the biggest transient above the bed rather
        # than on level drift: a bed sits 30-36 dB under the impacts, so a stray contact
        # pokes out exactly where nothing else is playing. Steadiness breaks ties.
        want = int(spec["len"][1] / 1000.0 * sr)
        e, hop = env_db(x, sr, 10.0)
        w = max(1, want // hop)
        best, ev_s = (1e9, 1e9), 0
        for i in range(0, max(1, len(e) - w - int(0.2 * sr / hop)), max(1, w // 40)):
            seg = e[i:i + w]
            live = seg[seg > seg.max() - 40]
            if not len(live):
                continue
            score = (round(float(seg.max() - np.median(live)), 1), float(np.std(live)))
            if score < best:
                best, ev_s = score, i * hop
    s = int(args.start / 1000.0 * sr) if args.start is not None else ev_s
    if args.len is not None:
        length = args.len
    else:
        # Follow the event's own decay rather than always cutting at the slot maximum,
        # so a fast take does not drag 50 ms of room tone behind it.
        length = min(max((ev_e - s) / sr * 1000.0, spec["len"][0]), spec["len"][1])
    seg = x[s:s + int(length / 1000.0 * sr)].copy()
    if len(seg) < 16:
        print("nothing to cut at that start point")
        return 1

    seg -= seg.mean()
    if spec.get("loop"):
        # The engine loops whole files, so the seam is the asset's problem. Fold the tail
        # back over the head with an equal-power crossfade: the result is exactly `length`
        # long and its end already matches its start, so it repeats without a pulse.
        xf = min(int(0.150 * sr), len(seg) // 4)
        extra = x[s + len(seg):s + len(seg) + xf]
        if len(extra) == xf:
            t = np.linspace(0, np.pi / 2, xf)
            seg[:xf] = seg[:xf] * np.sin(t) ** 2 + extra * np.cos(t) ** 2
    else:
        fade = min(int(0.006 * sr), len(seg) // 4)
        seg[-fade:] *= np.cos(np.linspace(0, np.pi / 2, fade)) ** 2
    if args.tilt_eq:
        # Zero-phase shelf lifting everything above `fc`, for a take whose lo-mid and mid
        # sit far below the reference band curve. Corrective only -- it cannot invent
        # content that is not there, so check the result rather than trusting the number.
        F = np.fft.rfft(seg)
        f = np.fft.rfftfreq(len(seg), 1.0 / sr)
        # Wide bell over 250 Hz - 2.5 kHz, which is where a hollow scrape is missing its
        # grit. A high shelf matches the tilt number while leaving the middle empty, since
        # it lifts the air band just as much.
        g = np.exp(-((np.log2(np.maximum(f, 1.0) / 800.0)) ** 2) / (2 * 1.35 ** 2))
        F *= 10 ** (args.tilt_eq * g / 20.0)
        seg = np.fft.irfft(F, len(seg))
    seg /= max(np.abs(seg).max(), 1e-9)

    out_dir = args.out or os.path.join(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__))), "assets", "sfx")
    os.makedirs(out_dir, exist_ok=True)
    final = os.path.join(out_dir, f"{slot}_{args.var:02d}.wav")
    tmp = final + ".tmp.wav"

    w = wave.open(tmp, "wb")
    w.setnchannels(1)
    w.setsampwidth(2)
    w.setframerate(sr)
    w.writeframes((seg * 32767 * 0.9).astype(np.int16).tobytes())
    w.close()

    chain = []
    if spec.get("hpf"):
        chain.append(f"highpass=f={spec['hpf']}:poles=2")
    chain.append(f"aresample={args.rate}:resampler=soxr:precision=28")
    subprocess.run(["ffmpeg", "-y", "-v", "error", "-i", tmp, "-af", ",".join(chain),
                    "-c:a", "pcm_s16le", final], check=True)

    y, ysr, _, _, _ = load(final)
    gain = spec.get("peak_dbfs", -1.5) - 20 * np.log10(max(np.abs(y).max(), 1e-9))
    subprocess.run(["ffmpeg", "-y", "-v", "error", "-i", final, "-af", f"volume={gain:.2f}dB",
                    "-c:a", "pcm_s16le", tmp], check=True)
    os.replace(tmp, final)

    led = ledger_read()
    src_hash = md5(args.file)
    if src_hash in led:
        led[src_hash]["used_as"] = f"{slot}_{args.var:02d}"
        ledger_write(led)
    else:
        # A take can fail as a whole file and still yield a good asset once windowed --
        # a loop take is judged end to end, but `make` ships a chosen slice of it. File
        # the source anyway: anything a shipped asset came from has to be kept.
        m0 = measure(args.file, slot)
        m0["verdict"] = "SOURCE"
        archive(m0, "SOURCE")
        led = ledger_read()
        if src_hash in led:
            led[src_hash]["used_as"] = f"{slot}_{args.var:02d}"
            ledger_write(led)

    m = measure(final, slot)
    print(f"{final}\n  {m['dur']:.0f} ms, {m['sr']} Hz mono, peak {m['peak']:.2f} dBFS, "
          f"centroid {m.get('centroid', 0):.0f} Hz, "
          f"-20 dB in {m['decay20']:.0f} ms" if m.get("decay20") else "")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    e = sub.add_parser("eval", help="judge takes against their slot spec")
    e.add_argument("target", help="wav file or directory of takes")
    e.add_argument("--slot", choices=sorted(SPEC), help="override slot inference")
    e.add_argument("--detail", action="store_true", help="show notes for passing takes too")
    e.add_argument("--csv", help="also write the table here")
    e.add_argument("--archive", action="store_true",
                   help="copy KEEP takes into takes/<slot>/ and record them in takes/ledger.csv")
    e.add_argument("--suggest", action="store_true",
                   help="for RETRY takes, name any slot they would pass as instead")
    e.set_defaults(fn=cmd_eval)

    m = sub.add_parser("make", help="post-process a take into the pack")
    m.add_argument("file")
    m.add_argument("--slot", choices=sorted(SPEC))
    m.add_argument("--var", type=int, required=True, help="variant number, 1-based")
    m.add_argument("--out", help="output dir (default: ../assets/sfx)")
    m.add_argument("--start", type=float, help="cut start in ms (default: detected attack)")
    m.add_argument("--tilt-eq", type=float, default=0.0, dest="tilt_eq",
                   help="dB of corrective lift above 250 Hz (loops with too little mid)")
    m.add_argument("--len", type=float, help="cut length in ms (default: slot maximum)")
    m.add_argument("--rate", type=int, default=44100,
                   help="output sample rate (default 44100, the format 02-SFX-Generation-"
                        "Prompts.md specifies; 48000 for a pack the game loads directly)")
    m.set_defaults(fn=cmd_make)

    sp = sub.add_parser("split", help="cut a multi-contact take into separate one-shots")
    sp.add_argument("file")
    sp.add_argument("--slot", choices=sorted(SPEC))
    sp.add_argument("--out", help="output dir (default: ../takes/_split)")
    sp.set_defaults(fn=cmd_split)

    args = ap.parse_args()
    sys.exit(args.fn(args))


if __name__ == "__main__":
    main()
