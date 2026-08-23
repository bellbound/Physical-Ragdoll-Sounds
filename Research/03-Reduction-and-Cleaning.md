# 03 — Reduction and cleaning

The raw CSV over-counts real collisions by **2.5×** — 1377 impact rows describe 552 collision
episodes. Nothing derived from raw row counts means anything until that is collapsed. The
mechanisms have changed since the old dataset: one got much bigger, one got much smaller, and
one turned out to be a property of that one location rather than of Skyrim.

## The multipliers, measured

### 1. Mirrored self-collisions — ×2.00, and now the dominant one

When limb A of the recorded actor hits limb B of the same actor, both listeners fire and both
rows are written. **638 of 1377 impact rows (46 %) are `DeadBip`-layer**, and within a frame
**every single ordered `(limb, other_limb)` pair has its mirror present** — 624 of 624. Those
624 ordered pairs describe **312 unordered ones**.

This was ×~1.5 on 32 % of rows in the old data; here it is ×2.00 on nearly half of them, with
no exceptions at all. The scripted shove tangles the subject with itself far more than a
hand-driven leash yank did.

Detect with `other_body`: for a row from limb A, if `other_body` is in this take's own
`limbs[].body` set, it is a self-collision, and the partner is the row in the same frame whose
own body is A's `other_body`. `other_limb` does the join for you. Collapse on the **unordered
pair** — which is exactly what keying an episode on `tuple(sorted((self_body, other_body)))`
does, so the reduction below already handles it.

### 2. Several contact points per manifold — ×1.07

Grouping impact rows by `(frame, limb_index, other_body)` gives 1286 groups from 1377 rows.
**Only 66 groups (5.1 %) have more than one row**, at a mean 2.38 rows each.

This was ×1.65 on the old data. The flags meant to bracket a manifold do not pair up (see
[02](02-Data-Dictionary.md)), so group by the frame key rather than by the flags.

Within a multi-point group, `manifold_first` **is** the fastest row 74 % of the time — the old
figure of 27.5 % does not reproduce — and the median max/first ratio is 1.00. Taking the max is
still right and costs one float; the point is that the penalty for not doing it is now small
rather than severe.

### 3. Coincident world colliders — **zero here**

Grouping world contacts by `(frame, limb_index, contact position)`: 708 limb-frames, and **not
one** reports against two different `other_body` pointers.

The old dataset's 19 % came from a custom house mod's outdoor area, covered by two overlapping
static compound bodies both reporting `Stone`/`StoneStairs`. Dragonsreach has nothing like it.
So the phenomenon is real but **local to particular geometry, not a general property** — which
also means this dataset cannot answer whether that area was a duplicated collision mesh.

A runtime still needs a tie-break rule for it, because a rug laid on a floorboard is a common
real case and the two adjacent materials both answer. It just is not a 19 % tax everywhere.

Note that `08_anomalies.py`'s `multi-other frames` column counts the same limb hitting **more
than one different thing** in a frame, mostly the floor and its own thigh at once. That is not
this phenomenon and must not be collapsed.

## The reduction

Implemented in `Scripts/lib_events.py`, which writes `Scripts/episodes.csv`.

```
frame     = rows separated by a gap > 2 ms   (a stepped frame; see 02)
pair      = unordered (this limb's body, other_body)   <- collapses the mirror
episode   = touch ... separate on one pair, with a depth counter so the
            mirrored touch/separate of a self-collision counts once
```

Per episode keep: the **first** impact row (limb, position, normal, layer, material, coverage,
and the kinematics at the moment of contact), the **peak** `impact_speed`, and the **peak**
`tangent_speed` over the episode.

1377 impact rows → **552 episodes** (303 against the world, 249 body-on-body, **0 against
another actor**). 826 touch/separate episodes exist in total; **274 of them (33 %) never exceed
the 5 unit/s floor** — soft settles, and they should stay silent. The old dataset's figure was
29 %.

**96.2 % of episodes have their peak on the very first impact row**, median peak/first ratio
exactly 1.00. The old figure was 88.2 %. So a runtime can fire on the first contact of an
episode and be right about the intensity about nineteen times in twenty, without waiting or
look-ahead. That remains the single most useful structural fact in this dataset, and it got
stronger.

An episode is a short thing: median 2 impact rows, median 74 ms long, a third of them a single
row. The p90 is around 440 ms, so a minority do drag on.

## Cleaning rules, in order

1. **Drop `stack-both` for anything about magnitude.** `Lennald…_2` is dominated by a scripted
   impulse that blew the solver up: peak 2390 units/s (34 m/s), angular speeds to 328 rad/s,
   52 rows above 700 u/s of which 9 fail the arithmetic check below.
   `16_summary_numbers.py` names it `CONTAMINATED`. The two takes that were *worse* than this
   are not in the folder at all — see [01 §Discarded](01-Dataset-Map.md#discarded).
2. **Reject blow-ups on the arithmetic, not on a threshold.** Compare `impact_speed` against
   `abs(normal_speed)`; a disagreement above 10 % means the solver's number came from somewhere
   the rigid-body arithmetic cannot reproduce. That flags 23 rows (1.7 %), and it scales the way
   you would want: 0 % of rows below 20 u/s, 20 % of rows between 1000 and 2000, 50 % above.
   **Use `abs()`** — the sign of `normal_speed` is negated on 23.3 % of rows for reasons
   unrelated to physics.
3. **Fixed guards are not derivable from this data.** The old `impact_speed > 700` is exceeded
   by two clean takes — `light-armour-fall` peaks at 855 and take 4 at 794 — so it is certainly
   too tight, but the takes that would have set the real ceiling were both discarded, so
   nothing here says where it is. `angular_speed > 25` is worse: 8.3 % of rows in the kept takes
   exceed it and only 7 of those 90 fail the arithmetic check.
4. **Gate on `phase`, even though it costs nothing here.** Every impact row in this dataset is
   `phase == ragdoll`, so the filter is a no-op — but only because the run paralyses the subject
   and closes the take before any get-up. It is not evidence that animated contacts have stopped
   happening.
5. **Collapse the mirror and the manifold** as above.
6. **Treat an unresolved material as the layer.** 29 rows (4.5 % of body-on-body) resolve no
   material at all. Terrain would need the same fallback; there is no terrain contact here to
   test it on.

## Derived columns in `episodes.csv`

| Column | Meaning |
|---|---|
| `v_first`, `v_peak` | closing speed, units/s |
| `v_ms` | `v_peak / 69.99`, m/s |
| `ke` | `0.5 · mass · v_ms²`, joules, using the **CSV's** live mass |
| `p` | `mass · v_ms`, N·s |
| `n_first` | `normal_speed` on the first row — compare `abs()` of it against `v_first` |
| `tan_first`, `tan_peak` | tangential speed at the contact, units/s |
| `site` | head / torso / arm / hand / leg / foot |
| `cover` | `bare` / `clothing` / `light` / `heavy` — what was on **this limb**, from the sidecar's coverage map, with the TNG-skin correction applied |
| `limb_radius` | the limb's own bounding radius |
| `self_hit` | `other_body` is one of this actor's own limbs |
| `cross_actor` | `other_body` belongs to the *other* recorded actor. **False on every row in this dataset** |
| `hit_limb` | the limb name behind `other_body`, resolved through the session-wide pointer registry |
| `nz` | contact normal z — `≈1` floor, `≈0` wall |
| `note`, `armour` | which scripted take this is, and what the actor was wearing overall. `note` is the take's *intent* — see [01](01-Dataset-Map.md) |
