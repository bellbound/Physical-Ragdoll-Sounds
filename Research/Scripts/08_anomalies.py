"""Where the numbers stop being trustworthy."""
import numpy as np, pandas as pd, rlib

# The guards, at the values this dataset supports. `disagree` is the one that
# needs no threshold picked from a distribution: impact_speed is the solver's
# closing speed, normal_speed is the same quantity recomputed from both bodies'
# motion state, and a row where they differ is one the rigid-body arithmetic
# cannot reproduce. Compare their *magnitudes* - normal_speed's sign is
# unreliable (see Research/05-Capture-Pipeline-Issues.md).
SPIN = 200.0     # rad/s; 25 would throw away 9% of genuine rows
HUGE = 1000.0    # units/s; a clean 10 m drop lands at 960
tot = dict(n=0, gt=0, disagree=0, spin=0, huge=0, dupbody=0, mf1=0)
per = []
for t in rlib.takes():
    df = t.load(kinds=["impact"])
    n = len(df)
    gt = (df["impact_speed"] > df["body_speed"] + 1e-3).sum()
    rel = ((df["impact_speed"] - df["normal_speed"].abs()).abs()
           / df["impact_speed"].clip(lower=1e-6))
    disagree = (rel > 0.10).sum()
    spin = (df["angular_speed"] > SPIN).sum()
    huge = (df["impact_speed"] > HUGE).sum()
    # Same limb, same frame, more than one other_body. NOT the same thing as a
    # coincident collider: most of these are a limb touching the floor and its
    # own thigh in one frame. Coincident world colliders - two static bodies at
    # the same contact *position* - are separately zero in this dataset, see
    # Research/03-Reduction-and-Cleaning.md.
    ts = df["t_ms"].to_numpy(); o = np.argsort(ts, kind="stable")
    fr = np.empty(n, int); fr[o] = np.concatenate([[0], np.cumsum(np.diff(ts[o]) > 2.0)])
    d = df.assign(frame=fr)
    g = d.groupby(["frame", "limb_index"])["other_body"].nunique()
    dupbody = int((g > 1).sum())
    per.append((t.stem, n, gt, disagree, spin, huge, dupbody, len(g)))
    for k, v in zip(["n","gt","disagree","spin","huge","dupbody"], [n,gt,disagree,spin,huge,dupbody]):
        tot[k] += int(v)

print(f"{'take':<30} {'imp':>5} {'v>body':>7} {'disagree':>9} {'spin>200':>9} {'v>1000':>7} "
      f"{'multi-other frames':>22} {'limb-frames':>12}")
for r in per:
    print(f"{r[0]:<30} {r[1]:>5} {r[2]:>7} {r[3]:>9} {r[4]:>9} {r[5]:>7} {r[6]:>22} {r[7]:>12}")
print(f"\ntotals: {tot}")
print(f"impact_speed exceeds the body's COM linear speed in "
      f"{100*tot['gt']/tot['n']:.1f}% of impact rows - that is the spin term, not an error")
print(f"impact_speed and |normal_speed| disagree by >10% in "
      f"{100*tot['disagree']/tot['n']:.1f}% of impact rows")
