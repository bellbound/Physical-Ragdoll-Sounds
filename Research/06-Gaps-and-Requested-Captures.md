# 06 — Gaps, unknowns, and what to record next

The scripted run has been executed once, all thirteen takes. **Nine files survived review; four
were bad enough to discard.** This is what the run settled, what it did not, and what it cost —
because a run that never moves the player buys repeatability by giving up variety, and more than
half of its takes did not do what they were named after.

## What the run settled

| Question | Where it stands |
|---|---|
| **The input impulse.** The old data's leash yank had uncontrolled speed and direction | **Settled.** A fixed `PushActorAway` magnitude 6.0 from a fixed distance and heading. `repeat-1/2/3` measure the spread left over, and it turns out to be Havok's, not the experimenter's — [04](04-Findings.md#repeatability--and-it-is-havoks-spread-not-the-inputs) |
| **Does armour change the physics?** | **Settled: no.** Three outfits, identical input, all differences inside the run-to-run spread; and the limb masses are bit-identical across all of one actor's takes |
| **Per-actor mass model** | **Settled, and simpler than feared.** Lennald's mass vector is Proventus's × 1.03, his `scale`, limb for limb. The weight slider does nothing |
| **Can a runtime fire on first contact?** | **Settled: yes.** 96.2 % of episodes peak on their first impact row, median ratio 1.00 — up from 88.2 % |
| **Acoustic space / reverb** was `null` in every old take | **Settled.** `DYLN_ASPC_Int_Wood_Large` and the full `BGSReverbParameters` block on all 12 takes. One space, so no comparison — but the fields are confirmed working |
| **Ground-truth video** | **Settled for the picture.** A 1920×1080 60 fps clip per Proventus take, with a sync pair that agrees to 5 ms. The cut point is not recorded — [05 §9](05-Capture-Pipeline-Issues.md#9-the-video-clips-cut-point-is-not-recorded). Nobody has auditioned the audio yet |
| **Light armour** — the axis was cloth / naked / heavy only | **Settled.** Leather cuirass, bracers, boots and helmet, `coverage: light` on all six sites |
| **Metal on the hands and feet** | **Settled.** Iron gauntlets and boots, `coverage: heavy` on all six sites. Also corrected a wrong belief: a vanilla cuirass already covers forearms and calves, so the old "heavy" takes were not as bare as the docs claimed |
| **Was any take truncated by ring overflow?** | **No.** `dropped: 0` and `complete: true` in all twelve sidecars |
| **Player position for loudness** | **Settled.** `listener` rows every tick, 0.00 m of player drift, impacts 0.45–21.3 m away |
| **Does `tangent_speed` measure anything?** | **Partly.** It produces numbers, ρ=0.42 with normal speed, and it cleanly separates a drop (ratio 0.6–1.0) from a shove (1.5–3.6). It has **not** been checked against a real scrape — see below |

## What the run did not settle

### The two it was built for, and lost

- **The top of the intensity curve.** `fall-3m` blew the solver up and `fall-10m` ended with the
  subject on their feet; both were discarded. The highest clean contact left in the set is
  **794 u/s (11.3 m/s)** and it is an extreme push, not a fall. The blow-up guard in
  [07 §2](07-Reliability-Requirements.md#2-reject-physics-blow-ups-explicitly) is still a guess —
  all this run established is that the old 700 u/s figure is too low, because two clean takes beat
  it. **This is now the highest-value single capture on the list.**
- **Scrape versus thud.** The take named `slide` is an extreme push
  ([01](01-Dataset-Map.md#where-the-runs-intent-and-the-data-disagree)). `tangent_speed` works, but
  nothing in the set isolates a body sliding along a floor, so a scrape classifier has no training
  case. The fall-versus-shove contrast is the closest thing here and it is a much coarser
  distinction.

### Still completely missing

- **Character-on-character impact.** Three takes tried; all three missed by 1.4 m or more. Zero
  cross-actor contacts in 552 episodes. See
  [05 §7](05-Capture-Pipeline-Issues.md#7-the-two-actor-takes-never-made-contact) — the run's
  placement needs fixing before this is worth attempting again.
- **Any natural ground.** Not one `Ground`-layer contact exists. Materials present: Carpet, Wood,
  WoodStairs, Stone, and Skin — and Carpet plus Wood are 94 % of the world episodes. Missing: dirt,
  grass, snow, ice, gravel, sand, mud, water, metal, glass, organic. Snow is half of Skyrim.
  **The `terrain` branch of `material_source` has never fired.** This is the one thing no amount of
  code fixes — it needs somebody to stand on snow.
- **Vertical surfaces.** 10 of 261 world episodes (3.8 %), from two takes. The same share the old
  dataset had.
- **Non-human ragdolls.** Both actors are the standard 18-body biped. Draugr, wolves, trolls,
  chaurus all have different bodies and different `limb_index` meanings.
- **Three or more simultaneous ragdolls** — the mass-brawl case the voice budget has to survive.

### Newly missing — things the old dataset had and this one does not

- **The get-up blend.** No `ragdoll_end`, no `knock_get_up`, no `getup` phase row anywhere, because
  the run paralyses. The old dataset's window figures (1.19–2.98 s, median 1.48 s) are from data
  that has been deleted.
  [07 §1](07-Reliability-Requirements.md#1-gate-on-ragdoll-state-and-suppress-the-get-up-blend)
  still requires a silence window, and nothing now measures it.
- **Animated-limb contacts.** 41 % of the old dataset; 0 % of this one. The problem has not gone
  away, it just is not in the data.
- **A frame-rate contrast.** Every take ran at 48–51 fps. The old 30-vs-50 comparison across six
  rooms is gone, so the frame-rate-independence result is inherited rather than confirmed.
- **A second actor body type.** No female, no second scale, no second race pairing beyond
  Imperial/Nord. The old dataset's sex comparison was confounded, but it existed.
- **More than one room.** One cell, one acoustic space, one surface family.

### Open questions

1. **Is the `abs()` needed on `normal_speed` a bug or the contract?** 23.3 % of rows come out
   negated. Fixing the sign in the recorder is a one-line change if the convention is meant to be
   fixed; if it genuinely depends on manifold ordering, the column should be documented as
   unsigned. Either way, every consumer currently has to know.
2. **Why does a standing actor rebuild its ragdoll six times in three seconds?** That is what
   emptied `Lennald…_3`. Whether it is the drop, the paralysis on the other actor, the hurt idle,
   or something in this load order is not answerable from the CSV.
3. **Why did `fall-10m`'s subject land on their feet?** If the paralysis lapses during a long fall
   then every drop take is at risk, and the fix belongs in the run before it is attempted again.
4. **What did the `fall-3m` teleport put the subject inside?** Contact height rising 9.6 m in
   200 ms at 65 m/s says intersecting geometry, but the take carried no material or position that
   names it.
5. **Does `impact_speed` from a keyframed body mean anything?** Still unknown, and this dataset
   contains no keyframed contact at all — `Lennald…_3` should have provided them and provided
   nothing. `phase` marks such rows when they exist, so it can be deferred rather than answered.
6. **Do the video clips share one lead-in?** All nine are the take plus exactly 4.00 s, so probably
   yes; `18_video_sync.py` measures ~3.2 s on seven of them and gets confused by the second actor
   on the other two.

## What to record next — shortest useful list

**Tier 1**

1. **A drop take that works** — the highest-value capture on this list, because it is the only way
   to put a number on the top of the intensity curve. Fix the staging first
   ([05 §1](05-Capture-Pipeline-Issues.md#1-four-of-thirteen-takes-had-to-be-discarded)): settle a
   frame before paralysing, verify the landing zone is clear, and assert the actor is still limp
   when they hit. Then do 3 m, 6 m and 10 m so there is a curve rather than a point.
2. **A real slide take.** Something that puts a limp body in sustained sliding contact with a
   floor — a shove with the actor already prone, or a push onto a slope — so `tangent_speed` can be
   validated against an actual scrape.
3. **The run, five times: snow, dirt/grass, gravel, shallow water, and a metal or wooden-hollow
   floor.** Same takes, different ground. The only item on this list that cannot be fixed by
   editing code.
4. **One run with the two-actor placement fixed**, so limb-on-another-body finally has data.
5. **One take without paralysis**, so the handover and the get-up are on record once.

**Tier 2**

6. **Two runs of the same steps at 30 fps and at 120+ fps**, to confirm the frame-rate independence
   result under a controlled input rather than inheriting it.
7. **The run in a second acoustic space** — a barrow, a cave, outdoors — so reverb has a comparison
   rather than a single reading.
8. **A wall take** — a shove into a vertical surface at close range. Vertical geometry is still
   3.8 % of contacts and confounded with the one extreme-push take.
9. **The run on a female actor and on a second scale**, to see whether anything beyond
   `mass × scale` moves.

**Tier 3**

10. **The run on a draugr and on a wolf**, to see how far the limb→site mapping generalises.
11. **Three or more actors ragdolled at once**, by hand, for the voice budget.
12. **A deliberate high-impulse take**, since `stack-both` produced one by accident and it is the
    only sample of what another mod's `ApplyHavokImpulse` looks like from inside the callback.

## Recorder and run changes worth making

Ordered by how much they cost against how much they unlock. All are described in
[05](05-Capture-Pipeline-Issues.md).

| | Change | Why |
|---|---|---|
| 1 | **Teleport, settle one frame, then paralyse** — and check the destination is clear | `fall-3m` was thrown away |
| 2 | **Assert the actor is still ragdolling at the moment of landing**, and mark the take if not | `fall-10m` was thrown away for landing upright |
| 3 | **Verify the two actors overlap before triggering a stack take**, abort with a notice otherwise | Three takes' worth of the biggest coverage gap, wasted |
| 4 | **Stamp what actually happened into `recording.note`**, not only what was intended | Every mismatch in [01](01-Dataset-Map.md) had to be recovered by watching video |
| 5 | **Snapshot limb masses at `ragdoll_start`, not at arm** | Three of twelve sidecars carry mass 0 |
| 6 | **Treat a nameless, weightless clothing entry as `bare` in `coverage:`** | The map's most important value never appears |
| 7 | **Take the absolute value of `normal_speed`, or fix its orientation** | Nearly a quarter of rows read negated |
| 8 | **Suppress the subject's package during the placement window** | The takes are a faceplant, not the described backward fall |
| 9 | **Write the video cut point into the sidecar** | Otherwise every clip has to be aligned by eye |
| 10 | **Re-attach to surviving bodies instead of rebuilding, or log a listener gap** | An empty take currently looks like a quiet take |
| 11 | **Stop shipping `manifold_first`/`manifold_last` as a bracket** | They do not pair up; the frame key does the job |
| 12 | **Give `session_stop` a real `seq`** | Sorting by `seq` puts the last row first |
| 13 | **Re-snapshot armour when it changes mid-take** | Not needed by the run, but a hand-run equip swap is currently invisible |
