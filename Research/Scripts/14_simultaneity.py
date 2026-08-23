"""How many sounds a naive trigger would want at once."""
import numpy as np, pandas as pd, rlib

per_frame, hot = [], []
for t in rlib.takes():
    df = t.load(kinds=["impact"])
    ts = df["t_ms"].to_numpy(); o = np.argsort(ts, kind="stable")
    fr = np.empty(len(df), int); fr[o] = np.concatenate([[0], np.cumsum(np.diff(ts[o]) > 2.0)])
    df = df.assign(frame=fr)
    g = df.groupby("frame").agg(limbs=("limb_index", "nunique"), rows=("limb_index", "size"),
                                vmax=("impact_speed", "max"),
                                loud=("impact_speed", lambda s: (s > 100).sum()))
    per_frame.append(g.assign(take=t.stem))
a = pd.concat(per_frame, ignore_index=True)
print("distinct limbs impacting in the same frame:")
print(a["limbs"].value_counts().sort_index().to_string())
print(f"\nframes with >=3 distinct limbs: {(a['limbs']>=3).sum()} of {len(a)} "
      f"({100*(a['limbs']>=3).mean():.1f}%)")
print("\nrows per frame:")
print(a["rows"].describe(percentiles=[.5,.9,.99]).round(2).to_string())
print("\nloud (>100 u/s) rows in the same frame:")
print(a["loud"].value_counts().sort_index().head(10).to_string())

print("\n=== rolling density: impacts within a 100 ms window ===")
for t in rlib.takes():
    df = t.load(kinds=["impact"]).sort_values("t_ms")
    v = df["t_ms"].to_numpy()
    if len(v) < 5: continue
    counts = np.searchsorted(v, v + 100) - np.arange(len(v))
    limbs = df["limb_index"].to_numpy()
    uniq = [len(set(limbs[i:i + counts[i]])) for i in range(len(v))]
    print(f"{t.stem:<30} max rows/100ms={counts.max():>4} max distinct limbs/100ms={max(uniq):>3} "
          f"p95 limbs={np.percentile(uniq,95):.0f}")
