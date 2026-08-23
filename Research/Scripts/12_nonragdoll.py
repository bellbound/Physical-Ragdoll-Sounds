"""The contacts that are not a ragdoll at all - animated limbs touching the world.

The phase now comes off the recorder's own `phase` column rather than being
reconstructed from state rows and a window join. In this dataset every impact
row is `ragdoll`: the run arms, triggers and closes while the subject is still
paralysed, so no animated or get-up contact was ever recorded. The animated
overlap is a real problem the runtime still has to handle - it is just not
measurable here. See Research/04-Findings.md.
"""
import numpy as np, pandas as pd, rlib
import lib_events

rows = []
for t in rlib.takes():
    df = t.load()
    imp = df[df["event"] == "impact"].copy()
    if not len(imp):
        continue
    imp["site"] = imp["limb"].map(lib_events.SITE)
    rows.append(imp[["phase", "site", "limb", "impact_speed", "body_speed",
                     "angular_speed", "other_layer"]])

a = pd.concat(rows, ignore_index=True)
print(a.groupby("phase").agg(n=("impact_speed","size"), v50=("impact_speed","median"),
                             v90=("impact_speed", lambda s: s.quantile(.9)),
                             vmax=("impact_speed","max")).round(1).to_string())
print("\nsite mix, ragdoll vs animated (% of that phase's rows):")
p = a.pivot_table(index="site", columns="phase", values="impact_speed", aggfunc="size").fillna(0)
print((100 * p / p.sum()).round(1).to_string())
print("\nmedian impact speed by site and phase:")
print(a.pivot_table(index="site", columns="phase", values="impact_speed", aggfunc="median").round(1).to_string())
print("\nlayer mix by phase (%):")
p = a.pivot_table(index="other_layer", columns="phase", values="impact_speed", aggfunc="size").fillna(0)
print((100 * p / p.sum()).round(1).to_string())
print("\nhow far up the speed scale do animated contacts reach?")
for q in [.5, .9, .99, .999]:
    print(f"  animated p{q*100:g} = {a[a.phase=='animated']['impact_speed'].quantile(q):7.1f}   "
          f"ragdoll p{q*100:g} = {a[a.phase=='ragdoll']['impact_speed'].quantile(q):7.1f}")
