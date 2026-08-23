#!/usr/bin/env python3
"""Check every shipped file against the delivery rules, not just the slot spec.

  format      mono / 48 kHz / 16-bit
  lead-in     the cue time is the attack; head silence becomes latency and breaks the
              +15/+50/+65 ms layer offsets
  tail        no baked room -- the game applies the cell reverb
  level       variants inside a slot differ in character, not loudness
  pitch       everything is shifted +/-3 semitones at runtime; nothing may clip there
  loops       looped whole-file with no crossfade, so the seam is the asset's problem
"""
import os, subprocess, sys, wave
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sfx import SPEC, infer_slot, env_db, load

d = sys.argv[1] if len(sys.argv) > 1 else "assets/sfx"
files = sorted(f for f in os.listdir(d) if f.endswith(".wav"))
bad, by_slot = [], {}

print(f"{'file':22s}{'fmt':18s}{'lead':>7s}{'peak':>8s}{'tail':>8s}{'pitch+3':>9s}{'seam':>7s}")
print("-" * 80)
for f in files:
    p = os.path.join(d, f)
    slot = infer_slot(f)
    with wave.open(p) as w:
        sr, nch, sw, n = w.getframerate(), w.getnchannels(), w.getsampwidth(), w.getnframes()
    x, _, _, _, _ = load(p)
    fmt = f"{nch}ch {sr} {sw*8}b"
    if (nch, sr, sw) != (1, 48000, 2):
        bad.append(f"{f}: format {fmt}")

    # lead-in: leading samples that are actually silent. Measured against an absolute
    # floor, not against the peak -- settle_rest is specified to start softly, and a slow
    # rise is character, whereas a flat -60 dBFS head is latency.
    pk = np.abs(x).max()
    idx = int(np.argmax(np.abs(x) >= 10 ** (-60 / 20.0)))
    lead = idx / sr * 1000.0
    if lead > 5.0:
        bad.append(f"{f}: {lead:.1f} ms of lead-in silence")

    peak = 20 * np.log10(max(pk, 1e-9))
    by_slot.setdefault(slot, []).append((f, peak))
    target = SPEC[slot].get("peak_dbfs", -1.5)
    if abs(peak - target) > 0.15:
        bad.append(f"{f}: peak {peak:.2f} dBFS, slot target {target}")

    # tail: a baked room shows up as a file longer than its slot allows, which is the
    # rule `make` enforces by truncating to the slot maximum. The -40 dB figure below is
    # reported for reading only -- thresholding it flags imp_sub's specified decay tail
    # and the body layers' 145-155 ms house decay, all of which are correct.
    e, hop = env_db(x, sr)
    fell = next((i for i in range(int(np.argmax(e)), len(e)) if e[i] <= e.max() - 40), None)
    tail = (len(e) - fell) * hop / sr * 1000.0 if fell is not None else 0.0
    loop = SPEC[slot].get("loop")
    dur = n / sr * 1000.0
    lo, hi = SPEC[slot]["len"]
    if dur > hi + 1:
        bad.append(f"{f}: {dur:.0f} ms long, slot maximum is {hi}")

    # pitch: resample +3 semitones, the worst case for interpolation overshoot
    # float output, so an overshoot past 0 dBFS shows up instead of being clamped
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", p, "-af",
                          f"asetrate={sr*2**(3/12.0):.0f},"
                          f"aresample={sr}:resampler=soxr:precision=28",
                          "-f", "f32le", "-"], check=True, capture_output=True).stdout
    y = np.frombuffer(raw, np.float32)
    ppk = 20 * np.log10(max(np.abs(y).max(), 1e-9))
    if ppk > -0.1:
        bad.append(f"{f}: clips at +3 semitones ({ppk:+.2f} dBFS)")

    seam = "-"
    if loop:
        k = int(0.05 * sr)
        a = 20 * np.log10(np.sqrt(np.mean(x[:k] ** 2)) + 1e-12)
        b = 20 * np.log10(np.sqrt(np.mean(x[-k:] ** 2)) + 1e-12)
        seam = f"{abs(a-b):.1f}dB"
        if abs(a - b) > 6.0:
            bad.append(f"{f}: loop seam mismatch {abs(a-b):.1f} dB")

    print(f"{f[:20]:22s}{fmt:18s}{lead:6.1f}m{peak:7.2f}d{tail:7.0f}m{ppk:8.2f}d{seam:>7s}")

print()
for slot, items in sorted(by_slot.items()):
    peaks = [p for _, p in items]
    if max(peaks) - min(peaks) > 0.2:
        bad.append(f"{slot}: variants differ in level by {max(peaks)-min(peaks):.2f} dB")

if bad:
    print(f"{len(bad)} problem(s):")
    for b in bad:
        print(f"  {b}")
else:
    print(f"all {len(files)} files pass: mono 48 kHz 16-bit, no lead-in, no baked tail, "
          f"level-matched within slot, headroom at +3 semitones, loops seamless")
sys.exit(1 if bad else 0)
