"""The controlled comparisons the scripted run makes possible.

The run holds the input fixed - one `PushActorAway` magnitude, one distance, one
heading - so the only thing varying between comparable takes is the one thing the
take changed. What it does *not* vary is the room, the frame rate, the actor's sex
or the surface, so those axes have no comparison here at all. See
Research/06-Gaps-and-Requested-Captures.md.
"""
import numpy as np
import pandas as pd

import rlib

e = pd.read_csv("episodes.csv")
e["short"] = (e["note"].str.replace("requested-capture ", "", regex=False)
              .str.split(":").str[0].str.split(" (", regex=False).str[0])

# fall-3m and stack-both are dominated by a scripted impulse that blew the solver
# up - see Research/05-Capture-Pipeline-Issues.md. They are shown, and marked.
CONTAMINATED = {"fall-3m", "stack-both"}


def prof(short, actor=None):
    s = e[e["short"] == short]
    if actor:
        s = s[s["actor"].str.startswith(actor)]
    w = s[~s["self_hit"]]
    if not len(w):
        return None
    return dict(eps=len(s), world=len(w),
                v50=w["v_peak"].median(), v90=w["v_peak"].quantile(.9),
                vmax=w["v_peak"].max(), ke_sum=w["ke"].sum(),
                tan50=w["tan_peak"].median(),
                ge100=int((w["v_peak"] >= 100).sum()), ge300=int((w["v_peak"] >= 300).sum()),
                self_pct=100*s["self_hit"].mean())


def show(title, shorts, actor=None):
    print(f"\n=== {title} ===")
    print(f"{'take':<22} {'eps':>4} {'world':>5} {'v50':>7} {'v90':>7} {'vmax':>7} "
          f"{'sum_ke_J':>9} {'tan50':>6} {'>=100':>5} {'>=300':>5} {'self%':>6}")
    for short in shorts:
        p = prof(short, actor)
        if p is None:
            print(f"{short:<22} no world contacts")
            continue
        mark = " !" if short in CONTAMINATED else ""
        print(f"{short + mark:<22} {p['eps']:>4} {p['world']:>5} {p['v50']:>7.1f} "
              f"{p['v90']:>7.1f} {p['vmax']:>7.1f} {p['ke_sum']:>9.1f} {p['tan50']:>6.0f} "
              f"{p['ge100']:>5} {p['ge300']:>5} {p['self_pct']:>6.1f}")


print("=== armour does not touch the physics: limb masses per take ===")
print("(a sum of 0 is the sidecar snapshotting a keyframed body, not a real mass -")
print(" motion_type says which; the CSV's own mass column is always right)")
for t in rlib.takes():
    v = tuple(round(l["mass"], 3) for l in t.limbs)
    print(f"{t.stem:<48} armour={t.armour_kind():<9} motion={t.limbs[0]['motion_type']:<15} "
          f"COM={v[0]:>7.3f} head={v[13]:>6.3f} sum={sum(v):>8.2f}")

show("run-to-run variance on a byte-identical input",
     ["repeat-1", "repeat-2", "repeat-3"])
show("the armour axis: same shove, three outfits",
     ["repeat-1", "light-armour", "heavy-armour"])
show("the armour axis again, on a 3 m drop",
     ["light-armour-fall", "heavy-armour-fall"])
show("fall height", ["repeat-1", "light-armour-fall", "fall-3m", "fall-10m"])
show("scrape versus thud", ["slide", "repeat-1", "fall-10m"])
show("the two-actor takes (all three missed - no cross-actor contact)",
     ["two-actors", "stack-both", "stack-onto-standing"])

print("\n=== what was on the limb that hit (uncontaminated takes) ===")
clean = e[~e["short"].isin(CONTAMINATED)]
print(clean.groupby("cover", observed=True).agg(
    n=("v_peak", "size"), v50=("v_peak", "median"),
    v90=("v_peak", lambda s: s.quantile(.9))).round(1).to_string())

print("\n=== axes this dataset does not have ===")
print(f"  actor sex:      {sorted(e['sex'].unique())}")
print(f"  cells:          {sorted({t.meta.get('environment.cell_name') for t in rlib.takes()})}")
print(f"  cross-actor:    {int(e['cross_actor'].sum())} episodes")
fps = []
for t in rlib.takes():
    d = t.load()
    g = np.diff(np.sort(d["t_ms"].dropna().to_numpy()))
    g = g[g > 2]
    # Lennald_3 has almost no rows, so its "frame gaps" are the half-second
    # reattach throttle rather than frames - it is not a frame-rate sample.
    if len(g) > 100:
        fps.append(1000/np.median(g))
print(f"  frame rate:     {min(fps):.0f}-{max(fps):.0f} fps across the 15 takes with events")
