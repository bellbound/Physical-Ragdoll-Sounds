import pandas as pd, numpy as np
pd.set_option("display.width", 200)
e = pd.read_csv("episodes.csv")

print("=== by body site (all takes) ===")
g = e.groupby("site").agg(n=("v_peak", "size"), v50=("v_peak", "median"),
                          v90=("v_peak", lambda s: s.quantile(.9)), vmax=("v_peak", "max"),
                          ke50=("ke", "median"), ke90=("ke", lambda s: s.quantile(.9)),
                          kemax=("ke", "max"), self_pct=("self_hit", lambda s: 100*s.mean()))
print(g.sort_values("n", ascending=False).round(2).to_string())

print("\n=== by limb ===")
g = e.groupby("limb").agg(n=("v_peak","size"), v50=("v_peak","median"),
                          v90=("v_peak", lambda s: s.quantile(.9)), ke90=("ke", lambda s: s.quantile(.9)))
print(g.sort_values("n", ascending=False).round(2).to_string())

print("\n=== left vs right (same site) ===")
print(e[e.side.isin(["L","R"])].groupby(["site","side"]).agg(
    n=("v_peak","size"), v50=("v_peak","median"), ke50=("ke","median"), mass=("mass","median")).round(2).to_string())

print("\n=== by collision layer ===")
print(e.groupby("layer").agg(n=("v_peak","size"), v50=("v_peak","median"),
                             v90=("v_peak", lambda s: s.quantile(.9))).round(2).to_string())

print("\n=== by surface material (world hits only) ===")
w = e[~e.self_hit]
print(w.groupby("material").agg(n=("v_peak","size"), v50=("v_peak","median"),
                                v90=("v_peak", lambda s: s.quantile(.9)),
                                tan50=("tan_peak","median")).round(2).to_string())

print("\n=== normal-z of the contact (world hits): flat ground vs wall ===")
b = pd.cut(w["nz"].abs(), [0,.2,.5,.8,.95,1.001], labels=["wall","steep","slope","near-flat","flat"])
print(w.groupby(b, observed=False).agg(n=("v_peak","size"), v50=("v_peak","median")).round(2).to_string())

print("\n=== impact speed distribution, world vs self ===")
for name, sub in [("world", e[~e.self_hit]), ("self", e[e.self_hit])]:
    q = sub["v_peak"].quantile([.5,.75,.9,.95,.99]).round(1).tolist()
    print(f"{name:<6} n={len(sub):<5} p50/75/90/95/99 = {q}  max={sub['v_peak'].max():.0f}")

print("\n=== tangential vs normal: which episodes are scrapes ===")
# tan_peak is the relative speed *along* the surface at the contact point, from
# the recorder's own reconstruction. It replaced slide_speed, which measured
# nothing - see Research/05-Capture-Pipeline-Issues.md.
e["ratio"] = e["tan_peak"] / e["v_peak"].clip(lower=.001)
print(e["ratio"].describe(percentiles=[.1,.25,.5,.75,.9]).round(2).to_string())
print(f"  tan > 2x normal (a scrape): {100*(e['ratio']>2).mean():.1f}%   "
      f"tan < 0.5x normal (a thud): {100*(e['ratio']<0.5).mean():.1f}%")
print("  spearman(tan_peak, v_peak) =",
      round(e[["tan_peak","v_peak"]].corr(method="spearman").iloc[0,1], 3))
