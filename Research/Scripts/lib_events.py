"""Shared reduction: raw rows -> one row per collision episode.

Every later script wants the same thing - the first (and peak) impact of each
touch/separate episode, with the limb, what was hit, and the energy proxies -
so it is built once here.
"""
import numpy as np, pandas as pd, rlib

SITE = {
    "NPC Head [Head]": "head", "NPC Neck [Neck]": "head",
    "NPC Spine [Spn0]": "torso", "NPC Spine1 [Spn1]": "torso",
    "NPC Spine2 [Spn2]": "torso", "NPC COM [COM ]": "torso",
    "NPC L UpperArm [LUar]": "arm", "NPC R UpperArm [RUar]": "arm",
    "NPC L Forearm [LLar]": "arm", "NPC R Forearm [RLar]": "arm",
    "NPC L Hand [LHnd]": "hand", "NPC R Hand [RHnd]": "hand",
    "NPC L Thigh [LThg]": "leg", "NPC R Thigh [RThg]": "leg",
    "NPC L Calf [LClf]": "leg", "NPC R Calf [RClf]": "leg",
    "NPC L Foot [Lft ]": "foot", "NPC R Foot [Rft ]": "foot",
}

# Which armour `coverage` site each ragdoll limb sits under, so an episode can
# say what was on the limb that hit rather than what the actor was wearing
# overall - the two differ, and that is the whole point of the coverage map.
LIMB_COVER = {
    "head": "head", "torso": "torso", "hand": "hands", "foot": "feet",
    "arm": "forearms", "leg": "calves",
}


def _body_registry() -> dict:
    """Havok body pointer -> (actor, limb name), across every take in the set.

    Pointers are per-session and the whole scripted run is one session, so a
    contact against the *other* recorded actor can be named. Without this a
    cross-actor hit is indistinguishable from a self-hit: both land on the
    `DeadBip` layer and neither fills `other_limb`, which only ever resolves
    the recorded actor's own limbs.
    """
    reg = {}
    for t in rlib.takes():
        for l in t.limbs:
            reg[l["body"]] = (t.actor, l["name"])
    return reg


def episodes(t: rlib.Take, registry: dict | None = None) -> pd.DataFrame:
    registry = _body_registry() if registry is None else registry
    df = t.load(kinds=["impact", "touch", "separate"]).sort_values(["t_ms", "seq"])
    body_of = {l["limb_index"]: l["body"] for l in t.limbs}
    # The YAML's limb mass is a snapshot taken while the actor was still
    # standing, and a keyframed Havok body reports mass 0 - so it is zero for
    # every limb in 6 of the 16 takes. The CSV's own `mass` column is read live
    # in the callback and is always right. Take the row's, fall back to the
    # sidecar. See Research/05-Capture-Pipeline-Issues.md.
    mass_of = {l["limb_index"]: l["mass"] for l in t.limbs}
    own = set(t.bodies)
    df["self_body"] = df["limb_index"].map(body_of)
    df["pair"] = [tuple(sorted((a, b))) for a, b in zip(df["self_body"], df["other_body"])]

    out = []
    for pair, sub in df.groupby("pair", sort=False):
        depth = 0
        cur = None
        for r in sub.itertuples(index=False):
            if r.event == "touch":
                if depth == 0:
                    cur = dict(t0=r.t_ms, rows=[])
                depth += 1
            elif r.event == "separate":
                depth = max(depth - 1, 0)
                if depth == 0 and cur is not None:
                    cur["t1"] = r.t_ms
                    out.append(cur)
                    cur = None
            elif cur is not None:
                cur["rows"].append(r)

    recs = []
    for ep in out:
        if not ep["rows"]:
            continue
        rows = ep["rows"]
        first = rows[0]
        peak = max(rows, key=lambda r: r.impact_speed)
        m = first.mass if first.mass > 0 else mass_of.get(first.limb_index, 0.0)
        site = SITE.get(first.limb, "?")
        hit_actor, hit_limb = registry.get(first.other_body, ("", ""))
        recs.append(dict(
            take=t.stem, actor=t.actor, sex=t.sex, interior=t.interior,
            note=t.note, armour=t.armour_kind(),
            t0=ep["t0"], dur=ep["t1"] - ep["t0"], n_rows=len(rows),
            limb=first.limb, site=site,
            side=("L" if " L " in first.limb else "R" if " R " in first.limb else "-"),
            cover=t.covering(LIMB_COVER.get(site, "")),
            mass=m, limb_radius=first.limb_radius,
            v_first=first.impact_speed, v_peak=peak.impact_speed,
            # the solver's number and our own arithmetic for the same quantity;
            # where they disagree the row is a blow-up - see 05 and 07.
            #
            # abs() because the sign is not comparable across schemas: before the
            # 2026-08-22 fix the reconstruction subtracted in *listener* order
            # rather than body[0]-minus-body[1], so 23.3% of rows came out
            # negated. Takes recorded after it are signed consistently; abs()
            # reads both and the detector only ever wanted the magnitude.
            n_first=abs(first.normal_speed),
            # which physics step the contact fell in. Group a manifold by
            # (frame, limb, other_body) - the manifold flags do not bracket one.
            frame=getattr(first, "frame", -1),
            # relative speed along the surface: the scrape-versus-thud signal,
            # measured for the first time in this dataset
            tan_first=first.tangent_speed,
            tan_peak=max(r.tangent_speed for r in rows),
            phase=first.phase,
            body_speed=first.body_speed,
            ang=first.angular_speed,
            layer=first.other_layer, material=first.other_material,
            material_source=first.material_source,
            self_hit=first.other_body in own,
            # a contact against the *other* recorded actor, named through the
            # session-wide pointer registry
            cross_actor=bool(hit_actor) and hit_actor != t.actor,
            hit_limb=hit_limb,
            nz=first.nrm_z, manifold_first=first.manifold_first,
        ))
    e = pd.DataFrame(recs)
    if len(e):
        # metres/second and joules, so the numbers mean something outside Skyrim
        e["v_ms"] = e["v_peak"] / rlib.UNITS_PER_METRE
        e["ke"] = 0.5 * e["mass"] * e["v_ms"] ** 2
        e["p"] = e["mass"] * e["v_ms"]
    return e


def all_episodes() -> pd.DataFrame:
    reg = _body_registry()
    parts = [episodes(t, reg) for t in rlib.takes()]
    return pd.concat([p for p in parts if len(p)], ignore_index=True)
