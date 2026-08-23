"""Candidate loudness metrics and what the 5 u/s floor costs us."""
import numpy as np, pandas as pd, rlib
e = pd.read_csv("episodes.csv")

print("=== impact speed histogram (episode peaks, world hits), game units/s ===")
w = e[~e["self_hit"]]["v_peak"]
bins = [5, 10, 20, 40, 80, 160, 320, 640, 1400]
h = pd.cut(w, bins).value_counts().sort_index()
for iv, c in h.items():
    ms = f"{iv.left/rlib.UNITS_PER_METRE:.2f}-{iv.right/rlib.UNITS_PER_METRE:.2f} m/s"
    print(f"  {str(iv):<14} {ms:<18} {c:>5}  {'#'*int(60*c/h.max())}")
print(f"  fraction within 2x of the 5 u/s floor (<10): {100*(w<10).mean():.1f}%")

print("\n=== does manifold_first pick the strongest contact point of the step? ===")
tot = same = 0
for t in rlib.takes():
    df = t.load(kinds=["impact"])
    ts = df["t_ms"].to_numpy(); o = np.argsort(ts, kind="stable")
    fr = np.empty(len(df), int); fr[o] = np.concatenate([[0], np.cumsum(np.diff(ts[o]) > 2.0)])
    df = df.assign(frame=fr)
    for _, g in df.groupby(["frame", "limb_index", "other_body"]):
        if len(g) < 2: continue
        tot += 1
        first = g[g["manifold_first"] == 1]
        if len(first) and first["impact_speed"].max() >= g["impact_speed"].max() - 1e-3:
            same += 1
print(f"  multi-point manifolds: {tot}; manifold_first row is the fastest in {100*same/max(tot,1):.1f}%")

print("\n=== per-knockdown energy budget (world hits inside the ragdoll window) ===")
print(f"{'take':<30} {'eps':>4} {'sum_ke_J':>9} {'max_ke':>7} {'sum_p':>7} {'peak_v_ms':>9}")
for t in rlib.takes():
    s = e[(e["take"] == t.stem) & (~e["self_hit"])]
    if not len(s): continue
    print(f"{t.stem:<30} {len(s):>4} {s['ke'].sum():>9.1f} {s['ke'].max():>7.1f} "
          f"{s['p'].sum():>7.1f} {s['v_ms'].max():>9.2f}")

print("\n=== how well do candidate metrics separate 'big' from 'small'? "
      "(spearman with v_peak) ===")
c = e[["v_peak", "ke", "p", "mass", "body_speed", "ang", "tan_peak", "n_rows", "dur"]].corr(method="spearman")
print(c["v_peak"].round(3).to_string())
