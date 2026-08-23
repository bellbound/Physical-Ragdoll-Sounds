"""Does frame time bias the impacts? Tested inside each take, not across them."""
import numpy as np, pandas as pd, rlib

rows = []
for t in rlib.takes():
    df = t.load(kinds=["impact", "touch", "separate"])
    ts = df["t_ms"].to_numpy(); o = np.argsort(ts, kind="stable")
    fr = np.empty(len(df), int)
    fr[o] = np.concatenate([[0], np.cumsum(np.diff(ts[o]) > 2.0)])
    df = df.assign(frame=fr)
    ft = df.groupby("frame")["t_ms"].min()
    dt = ft.diff().shift(-0)          # gap to the previous stepped frame
    df["dt"] = df["frame"].map(dt)
    imp = df[(df["event"] == "impact") & df["dt"].between(5, 80)]
    rows.append(imp[["dt", "impact_speed", "angular_speed", "body_speed"]].assign(take=t.stem))

a = pd.concat(rows, ignore_index=True)
b = pd.cut(a["dt"], [5, 12, 16, 20, 25, 33, 45, 80],
           labels=["<12", "12-16", "16-20", "20-25", "25-33", "33-45", "45-80"])
print("impacts binned by the frame gap they landed in (all takes pooled):")
print(a.groupby(b, observed=False).agg(
    n=("impact_speed", "size"), v50=("impact_speed", "median"),
    v90=("impact_speed", lambda s: s.quantile(.9)), vmax=("impact_speed", "max"),
    ang50=("angular_speed", "median")).round(1).to_string())

print("\nspearman(dt, impact_speed) within each take:")
for stem, sub in a.groupby("take"):
    if len(sub) < 60: continue
    r = sub["dt"].rank().corr(sub["impact_speed"].rank())
    print(f"  {stem:<30} n={len(sub):>5} rho={r:+.3f}")
