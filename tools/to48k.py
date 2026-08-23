#!/usr/bin/env python3
"""Resample already-built pack files to 48 kHz in place, then renormalise.

For the files built before the pack moved to 48 kHz. Re-rendering them from their takes
is not faithful -- several were cut or EQ'd with `make` overrides the ledger does not
record -- so the shipped result is resampled rather than rebuilt, which keeps the exact
editorial decision and only changes the container rate. Renormalising afterwards matters:
soxr can overshoot the pre-resample peak by a few tenths of a dB.
"""
import os, subprocess, sys, wave
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sfx import SPEC, infer_slot

RATE = 48000
for path in sorted(sys.argv[1:]):
    with wave.open(path) as w:
        if w.getframerate() == RATE:
            print(f"  {os.path.basename(path)}: already {RATE}")
            continue
    slot = infer_slot(path)
    target = SPEC[slot].get("peak_dbfs", -1.5)
    tmp = path + ".tmp.wav"
    subprocess.run(["ffmpeg", "-y", "-v", "error", "-i", path, "-af",
                    f"aresample={RATE}:resampler=soxr:precision=28",
                    "-c:a", "pcm_s16le", tmp], check=True)
    with wave.open(tmp) as w:
        y = np.frombuffer(w.readframes(w.getnframes()), np.int16) / 32768.0
    gain = target - 20 * np.log10(max(np.abs(y).max(), 1e-9))
    subprocess.run(["ffmpeg", "-y", "-v", "error", "-i", tmp, "-af", f"volume={gain:.2f}dB",
                    "-c:a", "pcm_s16le", path], check=True)
    os.remove(tmp)
    print(f"  {os.path.basename(path)}: 44100 -> {RATE}, {gain:+.2f} dB to {target} dBFS")
