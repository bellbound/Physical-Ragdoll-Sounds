"""Collision episodes: touch -> impacts -> separate, per limb/other pair.

An episode is the unit a sound would be triggered on. This measures how long
they last, how many impact rows fall inside one, where the peak sits, and how
much chatter (touch/separate flicker) there is around them.
"""
import numpy as np, pandas as pd, rlib

rows = []
for t in rlib.takes():
    df = t.load(kinds=["impact", "touch", "separate"]).sort_values(["t_ms", "seq"])
    body_of = {l["limb_index"]: l["body"] for l in t.limbs}
    df["self_body"] = df["limb_index"].map(body_of)
    df["pair"] = [tuple(sorted((a, b))) for a, b in zip(df["self_body"], df["other_body"])]

    for pair, sub in df.groupby("pair"):
        depth, start, peak, n, first_v = 0, None, 0.0, 0, None
        for ev, ts, v in zip(sub["event"], sub["t_ms"], sub["impact_speed"]):
            if ev == "touch":
                if depth == 0:
                    start, peak, n, first_v = ts, 0.0, 0, None
                depth += 1
            elif ev == "separate":
                depth = max(depth - 1, 0)
                if depth == 0 and start is not None:
                    rows.append(dict(take=t.stem, dur=ts - start, n=n, peak=peak,
                                     first=first_v if first_v is not None else 0.0))
                    start = None
            elif start is not None:
                n += 1
                peak = max(peak, v)
                if first_v is None:
                    first_v = v

e = pd.DataFrame(rows)
inside = e[e["n"] > 0]
print(f"episodes total {len(e)}, with >=1 impact {len(inside)} ({100*len(inside)/len(e):.0f}%)")
print("\nepisode duration ms (with impacts):")
print(inside["dur"].describe(percentiles=[.1, .25, .5, .75, .9, .99]).round(2).to_string())
print("\nimpact rows per episode:")
print(inside["n"].describe(percentiles=[.5, .75, .9, .99]).round(2).to_string())
print(f"\nepisodes where the first impact IS the peak: "
      f"{100*(inside['first'] >= inside['peak'] - 1e-3).mean():.1f}%")
print(f"peak/first ratio median {(inside['peak']/inside['first'].clip(lower=1e-3)).median():.2f}")
print("\npeak impact speed per episode:")
print(inside["peak"].describe(percentiles=[.5, .75, .9, .95, .99]).round(1).to_string())
print("\nepisodes with no impact at all - duration ms:")
print(e[e["n"] == 0]["dur"].describe(percentiles=[.5, .9]).round(2).to_string())
