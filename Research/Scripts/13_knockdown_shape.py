"""The shape of one knockdown on the time axis, take by take."""
import numpy as np, pandas as pd, rlib
for t in rlib.takes():
    df = t.load()
    st = df[df["event"] == "state"]
    marks = [(r.t_ms/1000, r.state) for r in st.itertuples() if r.state]
    s0 = st[st["state"] == "ragdoll_start"]["t_ms"]
    if s0.empty: continue
    s0 = s0.iloc[0]
    imp = df[(df["event"] == "impact")]
    rel = (imp["t_ms"] - s0) / 1000.0
    bins = [-1, 0, .1, .25, .5, 1, 1.5, 2, 3, 5, 8, 100]
    h = pd.cut(rel, bins).value_counts().sort_index()
    peak = imp.loc[imp["impact_speed"].idxmax()]
    print(f"-- {t.stem}")
    print("   marks: " + " ".join(f"{v:.2f}s:{n}" for v, n in marks if n not in ("",)))
    print("   impacts rel. to ragdoll_start: " +
          " ".join(f"[{iv.left:g},{iv.right:g})={c}" for iv, c in h.items() if c))
    print(f"   loudest row: t={rel.loc[peak.name]:+.2f}s v={peak['impact_speed']:.0f} "
          f"limb={peak['limb']} ang={peak['angular_speed']:.1f}")
