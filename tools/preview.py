#!/usr/bin/env python3
"""
preview.py -- render the impact composite so the stack can be judged as one sound.

The layer offsets and level balance come from 01-Reference-Analysis.md section 1: the bands
of a reference hero hit do not arrive together. Transient at 0 ms and quietest, body at
+10-30 ms, sub at +55-75 ms and loudest. This is the check the asset list exists to serve --
the individual files can all pass and still not fuse.

    python tools/preview.py [--var N] [--out DIR]
"""

import argparse
import os
import subprocess
import wave

import numpy as np

# (slot, offset ms, gain dB). Offsets are the measured arrival order. The gains are solved
# against the mean band curve of the four reference hero hits (sub -1, low -6, lomid -6,
# mid -8, high -12, air -23), and land within 1.9 dB RMS of it.
#
# The sub sits 8 dB UNDER the transient, which looks backwards next to the analysis calling
# the sub band the loudest element -- but that is a statement about the composite's band
# balance, not about layer gain. imp_sub is almost pure sub-band energy while imp_transient
# spreads across mid and high, so the sub file has to be the quietest layer for the sub band
# to come out on top. Setting it loudest instead buries everything else and drove the
# composite to +29 dB of tilt against the references' +6.5 to +16.5.
LAYERS = [("imp_transient", 0.0, 0.0), ("imp_body", 20.0, -3.0), ("imp_sub", 65.0, -8.0)]


def read(path):
    w = wave.open(path, "rb")
    x = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(np.float64) / 32768.0
    sr = w.getframerate()
    w.close()
    return x, sr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--var", type=int, default=1)
    ap.add_argument("--out")
    args = ap.parse_args()
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    sfxdir = os.path.join(root, "assets", "sfx")
    out = args.out or os.path.join(root, "assets", "preview")
    os.makedirs(out, exist_ok=True)

    parts, sr = [], None
    for slot, off, gain in LAYERS:
        # imp_sub only has two variants; fall back rather than fail, which is what the
        # runtime's slot resolution does too.
        v = args.var if os.path.exists(
            os.path.join(sfxdir, f"{slot}_{args.var:02d}.wav")) else 1
        x, sr = read(os.path.join(sfxdir, f"{slot}_{v:02d}.wav"))
        parts.append((x * 10 ** (gain / 20.0), int(off / 1000.0 * sr)))

    n = max(len(x) + o for x, o in parts)
    mix = np.zeros(n)
    for x, o in parts:
        mix[o:o + len(x)] += x
    mix /= max(np.abs(mix).max(), 1e-9) / 10 ** (-1.5 / 20.0)

    final = os.path.join(out, f"composite_{args.var:02d}.wav")
    tmp = final + ".tmp.wav"
    w = wave.open(tmp, "wb")
    w.setnchannels(1)
    w.setsampwidth(2)
    w.setframerate(sr)
    w.writeframes((mix * 32767).astype(np.int16).tobytes())
    w.close()
    subprocess.run(["ffmpeg", "-y", "-v", "error", "-i", tmp, "-c:a", "pcm_s16le", final],
                   check=True)
    os.remove(tmp)
    print(f"{os.path.basename(final)}  {len(mix)/sr*1000:.0f} ms")


if __name__ == "__main__":
    main()
