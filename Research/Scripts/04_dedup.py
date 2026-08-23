"""How many rows one real collision produces, and how to collapse them.

Three separate multiplications sit on top of each other:

  manifold points  - one callback per contact point, several per manifold per
                     step. `manifold_first` marks the first of each.
  substeps/frames  - the same pair keeps generating contacts on later steps
                     while it is still pressed together.
  mirrored pairs   - a limb hitting another limb of the *same* recorded actor
                     is reported twice, once by each limb's listener.
"""
import numpy as np, rlib

print(f"{'take':<30} {'imp':>5} {'mf1':>5} {'self':>5} {'mirror_pairs':>12} {'after_dedup':>11} "
      f"{'events_frame':>12} {'ratio':>6}")
tot = dict(imp=0, mf=0, mir=0, dd=0)
for t in rlib.takes():
    df = t.load(kinds=["impact"]).reset_index(drop=True)
    own = set(t.bodies)
    body_of = {l["limb_index"]: l["body"] for l in t.limbs}
    df["self_body"] = df["limb_index"].map(body_of)
    df["is_self"] = df["other_body"].isin(own)

    # frame bucket: bursts separated by >2ms
    ts = df["t_ms"].to_numpy()
    order = np.argsort(ts, kind="stable")
    frame = np.empty(len(df), dtype=int)
    frame[order] = np.concatenate([[0], np.cumsum(np.diff(ts[order]) > 2.0)])
    df["frame"] = frame

    mf = df[df["manifold_first"] == 1]

    # mirrored: within a frame, rows (a->b) and (b->a) both present
    keyed = mf.copy()
    keyed["pair"] = [tuple(sorted((a, b))) for a, b in zip(keyed["self_body"], keyed["other_body"])]
    g = keyed[keyed["is_self"]].groupby(["frame", "pair"]).size()
    mirror = int((g >= 2).sum())

    # dedup: one row per (frame, unordered pair), keeping the fastest
    dd = keyed.sort_values("impact_speed", ascending=False).drop_duplicates(["frame", "pair"])
    frames_with_impact = df["frame"].nunique()
    print(f"{t.stem:<30} {len(df):>5} {len(mf):>5} {int(df['is_self'].sum()):>5} {mirror:>12} "
          f"{len(dd):>11} {frames_with_impact:>12} {len(df)/max(len(dd),1):>6.2f}")
    tot["imp"] += len(df); tot["mf"] += len(mf); tot["mir"] += mirror; tot["dd"] += len(dd)
print(f"{'TOTAL':<30} {tot['imp']:>5} {tot['mf']:>5} {'':>5} {tot['mir']:>12} {tot['dd']:>11} "
      f"{'':>12} {tot['imp']/tot['dd']:>6.2f}")
