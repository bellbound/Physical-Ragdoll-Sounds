# 02 — Data dictionary

Units throughout: positions and speeds in **game units** (1 unit ≈ 1.428 cm; 69.99 units ≈
1 m), angular speed in **rad/s**, masses in Havok mass units, times in **ms since the take
started**.

Grades: **A** = read straight out of the solver, use it. **B** = usable with a stated caveat.
**C** = do not build on it.

One schema. The 34-column header below is what every file in `NewRecordings/` carries. All
counts are over the 12 kept takes — 1377 impact rows.

## CSV columns

| Column | Grade | What it is |
|---|---|---|
| `seq` | **B** | Row counter, shared across all takes in one session. Write order, not physics order. The `session_stop` row is written **out of band and carries `seq` 0**, so sorting a take by `seq` puts its last row first — sort by `t_ms` |
| `t_ms` | **B** | `steady_clock` at the moment Havok called back. Resolves **frames, not physics substeps** — see below |
| `game_hour` | C | Refreshed once per 32 ms tick and stamped onto rows as they are written. Fine for a timeline, not for ordering |
| `event` | A | `impact` / `touch` / `separate` / `state` / `limb_sample` / `listener` |
| `phase` | **B** | `animated` / `ragdoll` / `getup` / `unknown`. Which side of the ragdoll handover the row is on, sampled by the 32 ms tick and stamped in the callback, so it is accurate to one tick either side of a boundary. **Every impact, touch, separate and listener row in this dataset is `ragdoll`** — the run arms, triggers and closes while the subject is paralysed, so no animated or get-up contact was recorded at all. The column works; this data does not exercise it |
| `actor`,`actor_id` | A | Constant per file |
| `limb`,`limb_index` | A | Which of the 18 ragdoll bodies. Index matches the YAML's `limbs` list |
| `impact_speed` | **A** | Closing speed along the contact normal, negated `hkpContactPointEvent::separatingVelocity`, world-scaled. **This is the signal.** Computed inside the solver at the contact point, so it includes `ω × r` and legitimately exceeds `body_speed` in 153 rows (11.1 %) |
| `normal_speed` | **B** | The same closing speed recomputed from both bodies' motion state: `v + ω × (p − com)` on each, differenced, projected onto the normal. **The sign is unreliable** — 321 of 1377 rows (23.3 %) come out exactly negated, because which body of the pair the normal points away from is not fixed. Compare `abs(normal_speed)` against `impact_speed`: then the median disagreement is 0, p95 is 0, and only **1.7 %** of rows differ by more than 10 % — which makes the pair a blow-up detector needing no threshold. Compare them signed and a quarter of the dataset looks broken |
| `tangent_speed` | **A** as a measurement, **B** as an interpretation | The same relative contact-point velocity, projected *along* the surface. This is the scrape-versus-thud signal, it is the column `slide_speed` was supposed to be, and this is the first dataset in which it produces numbers at all. Median 177 u/s on world contacts, ρ=0.42 with `impact_speed` — so it is a genuinely separate axis. What is *not* here is a take that isolates a slide: the take named `slide` turned out to be an extreme push ([01](01-Dataset-Map.md)), so the column has never been checked against a real scrape |
| `body_speed` | A | \|linear velocity\| of the limb's rigid body, at its centre of mass |
| `angular_speed` | A | \|angular velocity\|, rad/s. A genuine armoured landing spins limbs to **60 rad/s** and an ordinary shove to 134 — the old 25 rad/s "blow-up" guard would discard 8.3 % of good rows. See [07 §2](07-Reliability-Requirements.md#2-reject-physics-blow-ups-explicitly) |
| `mass` | **A** | The limb's Havok mass, read **live in the callback**. Constant per actor. **Use this, not the YAML's** — see the sidecar section below. **Asymmetric and non-anatomical**, see [07 §6](07-Reliability-Requirements.md#6-do-not-use-the-havok-limb-mass-directly) |
| `limb_radius` | A | `motionState.objectRadius`: Havok's own bounding radius for the body, in game units. Constant per limb up to actor `scale`. Separates a hand (8.3) from a thigh (20.5); does **not** rank the way mass does |
| `pos_x/y/z` | A on `impact` | The contact point in world units — directly usable to position a sound. **Zero on `touch`/`separate`**; the limb's centre on `limb_sample`; the **player's** position on `listener` |
| `nrm_x/y/z` | A on `impact` | Contact separating normal. `\|nrm_z\| ≈ 1` is floor/ceiling, `≈ 0` is a wall. On `listener` rows, the player's facing as a unit vector. Zero on other non-impact rows |
| `vel_x/y/z` | A | The limb's linear velocity vector |
| `other_layer` | A | Collision layer of what was hit. In this dataset exactly **`Static` 739 (53.7 %) and `DeadBip` 638 (46.3 %)** and nothing else — no `Ground`, no `Biped`. Answers "world, or a body?" without touching a `TESObjectREFR` |
| `other_material` | **B** | Skyrim `MATERIAL_ID` of the hit surface. **Only filled on `impact` rows.** Here: `Skin` 609, `Carpet` 340, `Wood` 207, `WoodStairs` 148, `Stone` 44, unresolved 29. The 29 unresolved are all body-on-body — **4.5 % of `DeadBip` contacts**, a skin shape with no `bhkShape` behind it |
| `material_source` | **A** | `shape` = the contact's own `bhkShape::GetMaterialID`, measured at the contact. `terrain` = the land record under the *actor*, sampled once per tick. `-` = neither. **`terrain` never appears in this dataset** — there is not one `Ground`-layer contact in it, so the terrain path is written and unexercised |
| `other_body` | A | Raw Havok body pointer. Meaningful across the whole session — every take in this set shares one — which is how a contact against the *other recorded actor* can be named at all. `lib_events.py` builds that registry |
| `other_limb` | A | Index into the YAML's `limbs` when the thing hit was one of **this actor's own** limbs, `-` otherwise. It resolves own limbs only; a cross-actor hit reads `-` and has to be joined against the other take's sidecar |
| `manifold_first` | **C** as a picker | `hkpContactPointEvent::firstCallbackForFullManifold`. It marks the first contact point of a manifold *this step*, not the strongest — though on this data it happens to be the fastest of its manifold in **74 %** of multi-point manifolds, against 27.5 % on the old set. Only 5.1 % of contact groups are multi-point at all (66 of 1286), and their median max/first ratio is 1.00, so the practical cost of taking the first row is small |
| `manifold_last` | **C** as a bracket | The intent is that a manifold is the run from `manifold_first` to `manifold_last` for one `(limb_index, other_body)` pair. **The flags do not pair up.** Of 1377 impact rows, 1025 carry both, 60 carry neither, 48 carry only `first` and **244 carry only `last`**; and for a manifold that persists across frames both flags re-fire most frames. Group by `(frame, limb_index, other_body)` and take the max — do not trust the brackets to delimit anything |
| `dropped` | A | Only on the `session_stop` row: how many events the ring threw away. **0 on all 12 takes.** `-` elsewhere |
| `state` | A | On `event=state` rows: `session_start`, `ragdoll_start`, `knock_explode_lead_in`, `knock_explode`, `ragdoll_rebuilt`, `session_stop` are the six seen here. **`ragdoll_end`, `knock_get_up` and `actor_gone` never appear** — the paralysis holds and every take closes with the actor still limp. On `limb_sample` and `listener` rows: the state change that triggered the sample, or `tick` |

### Retired columns

| Column | Why it is gone |
|---|---|
| `slide_speed` | `sqrt(body_speed² − impact_speed²)`, clamped at 0. Mixed a COM velocity with a contact-point velocity and came out exactly 0 in 15.7 % of impact rows on the old dataset — disproportionately the spinning impacts a scrape model cares about. Gone from the header; **nothing in `Scripts/` reads it** and `rlib.NUMERIC` lists it only so a stray old file still parses. Replaced by `tangent_speed`, which is not the same quantity and is not comparable |

### What `t_ms` really resolves

Every contact callback is stamped in the callback, but Havok is stepped from the game thread,
so all of a frame's callbacks arrive within microseconds of each other and the next batch
arrives one frame later. Measured across this dataset: the median gap *inside* a batch is
**1.0 µs**; the median gap *between* batches is **20.4 ms** (p5 8.8 ms, p95 41.4 ms).

That gives a clean way to bucket rows into frames — a gap > 2 ms starts a new frame — and it
sets the ceiling on timing precision: **one frame**. Every take here ran at 48–51 fps, so this
dataset says nothing about how any of it behaves at 24 or at 144.

The state rows are on a different clock. They are pushed by the 32 ms tick, contacts are
stamped in the callback, and **two of the eleven takes with a ragdoll window have their first
impact 9 ms *before* their own `ragdoll_start` row.** The `phase` column on those rows is
already `ragdoll`, so the gate itself is right; it is the state row's timestamp that lags.

## YAML sidecar

`recording:` — `file_index`, `csv`, a **`note` naming which of the thirteen scripted takes this
is**, wall-clock and in-game start, the 5.0 unit/s floor, the 32 ms tick, and a `columns:`
block describing the newer columns. **The note is the take's intent, not a description of what
it contains** — read [01](01-Dataset-Map.md) before trusting it. `actor:` — form/base/race IDs,
sex, level, weight slider, scale, race height/weight. `ragdoll:` — the 18 limbs with `mass`,
`motion_type` and the Havok `body` pointer the CSV's `other_body` is matched against.
`armour:` — every occupied biped slot, plus a `coverage:` map (below). `environment:` — cell,
worldspace, location, position, acoustic space, the full reverb block, weather. `session:` —
appended when the take closes: `duration_ms`, `impacts`, `dropped`, `complete`, and the terrain
material sampled under the actor. `obs:` — appended after the video stops.

### `ragdoll.limbs[].mass` is zero in a quarter of the takes — **use the CSV's**

`BuildYaml` runs at `AddTargetLocked`, while the subject is still standing, and a **keyframed**
Havok body reports mass 0. So in **3 of the 12 kept takes** (`Proventus` 3, 4 and 7) every limb
reads `mass: 0.0000, motion_type: "keyframed"`, and any kinetic energy derived from the sidecar
is identically zero. The tell is `motion_type`.

The CSV's own `mass` column is read live in the contact callback and is right in every take.
`lib_events.py` takes the row's and falls back to the sidecar. `limb_sample` rows written at
`session_start` inherit the same zero, for the same reason.

### `armour:` and how to read it

Per equipped item: `slot` (where it was equipped), `site`/`bones` (what that slot means),
`covers_slots` and `covers_sites` (where its armour **addons** actually draw), plus form,
armour type, rating, weight and keywords.

`coverage:` is the same information the other way round — one line per body site giving the
heaviest thing on it and which piece decided that. **Use this, not slot occupancy.** It is
correct on the armoured takes: Iron Helmet → head, Iron Armor → torso/forearms/calves, Iron
Gauntlets → hands, Iron Boots → feet, and the same shape in leather.

Two things to know before trusting it:

- **It never says `bare`.** TNG's skin is a `TESObjectARMO` occupying slots 0, 2, 3, 7 and 22,
  so on a stripped subject every site reads `type: "clothing"` with an empty `name` and
  `weight: 0.000`. Nameless **and** weightless is the tell, and `rlib.Take.covering()`
  translates it to `bare`; anything else reading these files must do the same. Five of the nine
  Proventus takes here are affected.
- **Some slots emit a stub.** Slots 4, 5, 6 and 8 on the stripped takes carry only
  `slot`/`site`/`bones`/`name`/`form` — no `armour_type`, no `weight` — because the form there
  (`NakedTorsoImperial_RBT`) is not a resolvable `TESObjectARMO`. Read `coverage:`, which is
  built from the addons and does not have the hole.

**Correction to the old docs:** they said "an iron cuirass occupies slot 2 and covers slot 2
alone, leaving forearms and calves as skin". That is wrong. Vanilla `ArmorIronCuirass`
(`00012E49`) declares `covers_slots: [2, 4, 5, 8]` and `ArmorLeatherCuirass` (`0003619E`) the
same, so a cuirass on its own already covers forearms and calves. Gauntlets (3, 4) and boots
(7, 8) still matter — they put metal on the hands and feet, which the cuirass does not — but
"the hardest-hitting limbs were uncovered" was never true of the cuirass.

### `environment:` — reverb is real now

`acoustic_space: DYLN_ASPC_Int_Wood_Large` and the whole `BGSReverbParameters` block are
populated in all 12 takes. This is the first take set where they are, so the fields are
confirmed working rather than merely present. One space only — there is no reverb *comparison*
in this data.

### `session.terrain_sampled_at` is the last sample, not the first

It is the position the per-tick terrain lookup last ran at, which on a knockdown is metres from
where the take started. `session.terrain_material` is `-` in every take here.

### `obs:` and the sync CSV

`obs.offset_ms` plus the two-row `_sync.csv` align the take against the **uncut** OBS
recording. The `.mp4` files in the folder are cuts of it, and their cut point is not recorded
anywhere — see [01](01-Dataset-Map.md#video-and-sync) and
[05 §9](05-Capture-Pipeline-Issues.md#9-the-video-clips-cut-point-is-not-recorded).
