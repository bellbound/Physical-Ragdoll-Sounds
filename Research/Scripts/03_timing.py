"""Frame structure: what the timestamps actually resolve.

Every contact callback is stamped with steady_clock at the moment Havok calls
it, and Havok is called from the game thread's step. So the stamps do not
resolve physics substeps - they cluster into bursts, one burst per stepped
frame. The gap between bursts is the frame time; the spread inside a burst is
how long the solver took, which is not physical time at all.
"""
import numpy as np, rlib

print(f"{'take':<30} {'ev':>5} {'burst':>6} {'gapP50':>7} {'gapP10':>7} {'gapP90':>7} "
      f"{'fps50':>6} {'inburst_ms_p50':>14} {'ev/burst':>8} {'maxburst':>8}")
for t in rlib.takes():
    df = t.load(kinds=["impact", "touch", "separate"])
    ts = np.sort(df["t_ms"].to_numpy())
    if len(ts) < 20:
        continue
    d = np.diff(ts)
    # A gap over 2ms is another frame; inside a step the callbacks land within
    # tens of microseconds of each other.
    cut = 2.0
    idx = np.flatnonzero(d > cut)
    gaps = d[idx]
    burst_id = np.concatenate([[0], np.cumsum(d > cut)])
    sizes = np.bincount(burst_id)
    inb = d[d <= cut]
    print(f"{t.stem:<30} {len(ts):>5} {len(sizes):>6} {np.median(gaps):>7.2f} "
          f"{np.percentile(gaps,10):>7.2f} {np.percentile(gaps,90):>7.2f} "
          f"{1000/np.median(gaps):>6.1f} {np.median(inb)*1000:>14.1f} "
          f"{sizes.mean():>8.2f} {sizes.max():>8}")
