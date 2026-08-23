"""The numbers the write-up quotes, in one place."""
import numpy as np, pandas as pd, rlib

e = pd.read_csv("episodes.csv")
# fillna because the 14 archived takes predate `recording.note` and carry none.
# Without it `short` is NaN on 1572 of 2124 episodes and the mask below throws.
e["short"] = (e["note"].fillna("").str.replace("requested-capture ", "", regex=False)
              .str.split(":").str[0])
# fall-3m and stack-both are both dominated by a scripted impulse that blows the
# solver up - see Research/05-Capture-Pipeline-Issues.md. Everything about
# ordinary magnitudes is quoted off the other takes.
CONTAMINATED = ("fall-3m", "stack-both")
clean = e[~e["short"].str.startswith(CONTAMINATED)]

rows = 0
firsts, peaks, ends = [], [], []
for t in rlib.takes():
    df = t.load()
    imp = df[df["event"] == "impact"]
    rows += len(imp)
    st = df[df["event"] == "state"]
    starts = st[st["state"] == "ragdoll_start"]["t_ms"].to_numpy()
    ends += list(st[st["state"] == "ragdoll_end"]["t_ms"].to_numpy())
    if not len(starts) or not len(imp):
        continue
    s = starts[0]
    firsts.append((imp["t_ms"].min() - s) / 1000)
    peaks.append((imp.loc[imp["impact_speed"].idxmax(), "t_ms"] - s) / 1000)

f, p = np.array(firsts), np.array(peaks)
print(f"takes {len(rlib.takes())}   impact rows {rows}   episodes {len(e)}"
      f"   = {rows/len(e):.2f} rows per episode")
print(f"ragdoll_end rows in the whole set: {len(ends)}"
      f"   <- the run paralyses the subject, so no window ever closes")
print(f"first impact vs ragdoll_start: min={f.min():+.3f}s p50={np.median(f):+.3f}s "
      f"max={f.max():+.3f}s   (negative = the state row lags the contact by up to one tick)")
print(f"loudest impact after ragdoll_start: p10={np.percentile(p,10):.2f}s "
      f"p50={np.median(p):.2f}s p90={np.percentile(p,90):.2f}s")

print(f"\nepisodes {len(e)}: world {(~e['self_hit']).sum()}, self {e['self_hit'].sum()}, "
      f"against the other recorded actor {e['cross_actor'].sum()}")
print(f"peak on the first impact row of the episode: "
      f"{100*(e['v_peak']==e['v_first']).mean():.1f}%   median peak/first "
      f"{(e['v_peak']/e['v_first'].clip(lower=1e-6)).median():.3f}")

w = clean[~clean["self_hit"]]
print(f"\nworld episodes, uncontaminated takes: {len(w)}")
print(f"  >= 100 u/s: {(w['v_peak']>=100).sum()} ({100*(w['v_peak']>=100).mean():.1f}%)"
      f"   >= 300: {(w['v_peak']>=300).sum()} ({100*(w['v_peak']>=300).mean():.1f}%)")
print(f"  within 2x of the 5 u/s floor: {100*(w['v_peak']<10).mean():.1f}%")
print(f"  max: {w['v_peak'].max():.0f} u/s = {w['v_peak'].max()/rlib.UNITS_PER_METRE:.1f} m/s")

print("\nsite share of episodes with peak >= 200 u/s (uncontaminated takes):")
big = clean[clean["v_peak"] >= 200]
print((100*big["site"].value_counts()/len(big)).round(1).to_string())

print("\nper-knockdown budget, one line per uncontaminated take:")
for stem, s in clean.groupby("take"):
    print(f"  {stem.replace('_impacts_log_','#'):<40} {len(s):>4} episodes, "
          f"{(s['v_peak']>=100).sum():>3} above 100 u/s, {(s['v_peak']>=300).sum():>3} above 300")

print("\nthe scrape signal:")
print(f"  tan_peak median {w['tan_peak'].median():.0f} u/s, p90 {w['tan_peak'].quantile(.9):.0f}")
r = w["tan_peak"]/w["v_peak"].clip(lower=1e-6)
print(f"  tan > 2x normal: {100*(r>2).mean():.1f}%   tan < 0.5x normal: {100*(r<0.5).mean():.1f}%")
print(f"  spearman(tan_peak, v_peak) = "
      f"{w[['tan_peak','v_peak']].corr(method='spearman').iloc[0,1]:+.3f}")
