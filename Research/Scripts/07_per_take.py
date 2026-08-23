"""One fingerprint per take, read against what the run said it was recording."""
import numpy as np
import pandas as pd

import rlib

pd.set_option("display.width", 250)
e = pd.read_csv("episodes.csv")

for t in rlib.takes():
    s = e[e["take"] == t.stem]
    note = t.note.replace("requested-capture ", "")
    print(f"-- {t.stem.replace('_impacts_log_', '#')}")
    print(f"      {note}")
    if not len(s):
        st = t.load(kinds=["state"])
        rebuilds = int((st["state"] == "ragdoll_rebuilt").sum())
        print(f"      NO EVENTS - {rebuilds} ragdoll_rebuilt states, so the listeners were "
              f"detached for most of the take")
        continue
    w = s[~s["self_hit"]]
    mats = w["material"].value_counts().head(4).to_dict()
    walls = int((w["nz"].abs() < 0.5).sum())
    print(f"      eps={len(s)} world={len(w)} self={len(s)-len(w)} cross-actor="
          f"{int(s['cross_actor'].sum())} wall-ish={walls} armour={t.armour_kind()} mats={mats}")
    top = s.nlargest(4, "v_peak")[["t0", "limb", "v_peak", "tan_peak", "ke", "material",
                                   "layer", "nz", "cover"]]
    print("      peaks: " + " | ".join(
        f"{r.t0/1000:.1f}s {r.limb.replace('NPC ', '').replace('[', '').split(']')[0]:<10} "
        f"v={r.v_peak:.0f} tan={r.tan_peak:.0f} ke={r.ke:.0f} {r.material}/{r.layer} "
        f"nz={r.nz:+.2f} {r.cover}"
        for r in top.itertuples()))
