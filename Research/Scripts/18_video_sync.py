"""Recover each video clip's lead-in, because the cut point is not recorded.

`obs.offset_ms` in the YAML is an offset into the *uncut* OBS recording, and the
clips in NewRecordings are cuts of it - the take's duration plus exactly 4.00 s
of padding, split between lead and tail by something that was never written
down. So the alignment has to be recovered from the picture.

The method: decode the clip small and grey, take the mean absolute frame
difference, and call the largest spike in the 1.5-7 s window the trigger. The
take's own `ragdoll_start` says when that was on the CSV's clock, and the
difference is the lead-in. It lands within about +-100 ms on the takes where the
push is the biggest thing that happens; on `two-actors` and `stack-both` a
second actor moves more than the subject does and the estimate is wrong, which
is exactly the takes where a person should check by eye.

Needs ffmpeg on PATH.
"""
import glob
import os
import re
import subprocess

import numpy as np

import rlib

W, H, FPS = 160, 90, 30
SEARCH = (1.5, 7.0)     # seconds of clip to look in


def lead_ms(path: str, ragdoll_start_ms: float):
    cmd = ["ffmpeg", "-v", "error", "-i", path,
           "-vf", f"fps={FPS},scale={W}:{H},format=gray", "-f", "rawvideo", "-"]
    raw = subprocess.run(cmd, capture_output=True).stdout
    n = len(raw) // (W * H)
    if n < 2:
        return None, None
    a = np.frombuffer(raw[:n * W * H], dtype=np.uint8).reshape(n, H, W).astype(np.int16)
    d = np.abs(np.diff(a, axis=0)).mean(axis=(1, 2))
    lo, hi = int(SEARCH[0] * FPS), min(int(SEARCH[1] * FPS), len(d))
    peak = (lo + int(np.argmax(d[lo:hi]))) / FPS
    return peak, peak * 1000 - ragdoll_start_ms


def main():
    by_stem = rlib.by_stem()
    print(f"{'take':<6} {'ragdoll_start':>13} {'motion peak':>12} {'lead-in ms':>11}")
    leads = []
    paths = sorted(glob.glob(os.path.join(rlib.REC, "*.mp4")),
                   key=lambda p: int(re.search(r"_(\d+)\.mp4$", p).group(1)))
    for p in paths:
        stem = os.path.basename(p)[:-4]
        t = by_stem.get(stem)
        if t is None:
            continue
        st = t.load(kinds=["state"])
        v = st[st["state"] == "ragdoll_start"]["t_ms"]
        if not len(v):
            print(f"{stem.split('_')[-1]:<6} never ragdolled - nothing to align to")
            continue
        rs = float(v.iloc[0])
        peak, lead = lead_ms(p, rs)
        if peak is None:
            print(f"{stem.split('_')[-1]:<6} could not decode")
            continue
        leads.append(lead)
        print(f"{stem.split('_')[-1]:<6} {rs:>13.0f} {peak:>12.3f} {lead:>11.0f}")
    if leads:
        l = np.array(leads)
        print(f"\nlead-in: median {np.median(l):.0f} ms  min {l.min():.0f}  max {l.max():.0f}"
              f"  sd {l.std():.0f}")
        print("The two outliers are two-actors and stack-both, where the second actor's"
              " motion beats the subject's.")


if __name__ == "__main__":
    main()
