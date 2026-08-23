"""One line per take: what it is, how long, how much of it is usable."""
from __future__ import annotations
import os, numpy as np, rlib

rows = []
for t in rlib.takes():
    df = t.load()
    dur = df["t_ms"].max() / 1000.0
    imp = df[df["event"] == "impact"]
    own = set(t.bodies)
    self_hit = imp["other_body"].isin(own).sum()
    rag = df[(df["event"] == "state") & (df["state"].isin(["ragdoll_start", "ragdoll_end"]))]
    starts = rag[rag["state"] == "ragdoll_start"]["t_ms"].to_numpy()
    ends = rag[rag["state"] == "ragdoll_end"]["t_ms"].to_numpy()
    win = 0.0
    for s in starts:
        e = ends[ends > s]
        win += (e[0] - s) / 1000.0 if len(e) else (df["t_ms"].max() - s) / 1000.0
    mtime = os.path.getmtime(t.csv_path)
    rows.append(dict(
        stem=t.stem, mtime=mtime, sex=t.sex, armour=t.armour_kind(),
        interior="in" if t.interior else "out",
        cell=t.meta.get("environment.cell_name") or t.meta.get("environment.cell", "")[:0] or "-",
        dur=dur, rag_s=win, n_ragdoll=len(starts),
        impacts=len(imp), first=int(imp["manifold_first"].sum()),
        self_pct=100.0 * self_hit / max(len(imp), 1),
        touch=(df["event"] == "touch").sum(), sep=(df["event"] == "separate").sum(),
        vmax=imp["impact_speed"].max(), vp95=imp["impact_speed"].quantile(0.95),
        vmed=imp["impact_speed"].median(),
        states="|".join(df[df["event"] == "state"]["state"].unique()),
    ))

rows.sort(key=lambda r: r["mtime"])
import datetime
hdr = f"{'#':>2} {'take':<28} {'clock':<8} {'sex':<6} {'armour':<8} {'io':<3} {'dur_s':>6} {'rag_s':>6} {'nrag':>4} {'imp':>5} {'1st':>5} {'self%':>6} {'vmax':>7} {'v95':>6} {'vmed':>6}"
print(hdr); print("-" * len(hdr))
for i, r in enumerate(rows, 1):
    clock = datetime.datetime.fromtimestamp(r["mtime"]).strftime("%H:%M:%S")
    print(f"{i:>2} {r['stem']:<28} {clock:<8} {r['sex']:<6} {r['armour']:<8} {r['interior']:<3} "
          f"{r['dur']:>6.1f} {r['rag_s']:>6.1f} {r['n_ragdoll']:>4} {r['impacts']:>5} {r['first']:>5} "
          f"{r['self_pct']:>6.1f} {r['vmax']:>7.1f} {r['vp95']:>6.1f} {r['vmed']:>6.1f}")
print()
for i, r in enumerate(rows, 1):
    print(f"{i:>2} {r['stem']:<28} states: {r['states']}")
