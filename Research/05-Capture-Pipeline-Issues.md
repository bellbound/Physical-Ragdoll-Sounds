# 05 — Capture pipeline issues

The recorder is `skse/QuickModMenuNG/src/debug/ImpactRecorder.cpp`; the capture run is
**Debug → Record Requested Data** in the same file. This is what the pair gets right, what
could lead us to a wrong conclusion, and what has been done about it.

Every fix the previous version of this document asked for landed and is confirmed working on
this data — that list is at the bottom. What follows first is the **nine things this run broke
or left broken**, in rough order of how much damage each does.

> **Status, 2026-08-22.** Eight of the nine have been addressed in code; §4 has not.
> **None of it is confirmed on data** — the run has not been executed since. Every fix below is
> marked *fixed, unverified* and stays that way until a second run says otherwise. Every number
> in this document comes from the first run and describes the code as it was.

The headline is that **the run's take list is not a description of the data.** Of thirteen
takes, four had to be thrown away, one is mislabelled, three missed their purpose, and one came
back empty. [01](01-Dataset-Map.md) records what each one holds.

## 1. Four of thirteen takes had to be discarded

**Fixed, unverified.** Three changes, all in `CaptureBatch.cpp`:

- **The teleport and the paralysis are no longer the same frame.** A drop places the actor, waits
  `kSettleFrameMs` (120 ms), and paralyses in a second task. They are still animated across that
  gap, so nothing live is being teleported either — which was the reason the two were fused in
  the first place.
- **The limp state is re-asserted every 200 ms for the whole take** (`HoldLimp`). One `SetAV` did
  not survive 1.4 s of falling, which is why `fall-10m`'s subject landed on their feet.
- **The take says how it ended.** A new `outcome:` block in the sidecar carries
  `ended_ragdolled`, `ended_prone` and `actors_apart_m`, with a comment spelling out what a false
  reads like. A landing on the feet is now visible in the file instead of only on video.

Not a recorder fault in any narrow sense — the recorder faithfully wrote what happened — but the
single biggest cost of this run, and it falls squarely on how the takes are staged.

| Take | What went wrong |
|---|---|
| `fall-3m` (5) | Teleport + paralyse in the same frame put the subject inside or on top of something. Contact height rises 674 units (9.6 m) in the first 200 ms at closing speeds to 4591 u/s (65 m/s), limbs spinning at 514 rad/s |
| `fall-10m` (6) | The subject **landed on their feet.** An upright landing is not a ragdoll impact, so the numbers describe something else entirely |
| `stack-both` (8) | The two actors never came within 1.6 m. The subject's file is the wrong half of a take that did not happen; the guard's file is kept only as an artefact sample |
| `stack-onto-standing` (9) | Same miss, and the standing actor's file is empty — see §4 |

**Those two fall takes were the point of the run.** They existed to replace a guessed blow-up
threshold with a measured intensity ceiling
([07 §2](07-Reliability-Requirements.md#2-reject-physics-blow-ups-explicitly)), and both failed
in different ways. The guard is still a guess.

**The fix, in three parts:** teleport, let one frame settle, *then* paralyse; verify the
destination is clear before dropping; and for a drop meant to end limp, confirm the actor is
still ragdolling when they land rather than assuming the paralysis held.

## 2. `ragdoll.limbs[].mass` is 0 in three of twelve takes

**Fixed, unverified.** The masses are re-read at `ragdoll_start`, with the bodies dynamic, and
written into the closing `session:` block as `session.limb_masses`. `ragdoll.limbs[].mass` is
left alone — it is what the engine reported at arm, and overwriting it would hide that the take
was armed on somebody standing.

This bites a **hand-started** take harder than the run: arming on an upright NPC and then
knocking them down is the normal manual workflow, and it produces the zeroes every time.

`BuildYaml` runs at `AddTargetLocked`, before the trigger, while the subject is still standing.
A standing actor's ragdoll bodies are **keyframed**, and a keyframed Havok body reports
`mass = 0`. So in `Proventus` takes 3, 4 and 7 every limb in the sidecar reads

```yaml
      mass: 0.0000
      motion_type: "keyframed"
```

and any kinetic energy computed from the sidecar is identically zero for those takes. It is
silent: the file parses, the limb list is complete, the pointers are right, only the masses are
gone. Whether a take is affected depends on nothing more than whether the subject happened to
still be limp from the previous take when this one armed.

Two things make this survivable. The CSV's own `mass` column is read live in the contact
callback and is correct in every take; and `motion_type` names the problem on the same line.
`lib_events.py` now takes the row's mass and falls back to the sidecar.

**The fix:** re-read the masses at `ragdoll_start` rather than at arm, or write both. The
`limb_sample` rows emitted at `session_start` inherit the same zero for the same reason, so they
cannot stand in.

## 3. `normal_speed`'s sign is unreliable

**Fixed, unverified.** The reconstruction now always subtracts `body[0] - body[1]`, never "us
minus them". The contact normal belongs to the *pair* and its direction is fixed by that
ordering, not by which of the two listeners Havok happened to call — so subtracting in listener
order flipped the sign on exactly those contacts where our limb was `body[1]`. That is the
23.3 %.

`lib_events.py` takes `abs()` regardless, because the two schemas are not comparable and the
detector only ever wanted the magnitude.

`normal_speed` exists so that the solver's `impact_speed` can be checked against arithmetic done
from the two bodies' motion state, and where they disagree the row is a blow-up. The check works
— but only on magnitudes.

**321 of 1377 impact rows (23.3 %) have `normal_speed` exactly negated**, `|impact − normal|`
coming out at exactly `2 × impact`. Which body of the pair the contact normal points away from
is not fixed, and the reconstruction assumes it is. Compared signed, a quarter of the dataset
looks broken and the p95 relative disagreement is exactly 2.0. Compared as `abs(normal_speed)`,
the median disagreement is 0, the p95 is 0, and 23 rows (1.7 %) differ by more than 10 % — which
is a blow-up detector needing no threshold, and a good one:

| band | rows | disagree >10 % |
|---|---|---|
| impact_speed < 20 u/s | 290 | 0.0 % |
| 160–320 | 171 | 3.5 % |
| 320–640 | 116 | 1.7 % |
| 640–1000 | 28 | 0.0 % |
| 1000–2000 | 40 | **20.0 %** |
| > 2000 | 2 | 50.0 % |

Note the shape: it stays near zero right through the range the kept takes occupy and only turns
on above 1000 u/s, which is where the one contaminated take lives. That is the behaviour you
want from a detector, and it is why it is better than a hand-picked speed threshold.

**The fix:** orient the reconstruction against the same body the solver did, or just write the
absolute value and document it as unsigned. Until then, every consumer must call `abs()`.

## 4. Six `ragdoll_rebuilt` events swallowed a whole take

**Not fixed — the only one of the nine left standing.** It is not understood well enough to fix
safely: open question 2 in [06](06-Gaps-and-Requested-Captures.md) is still open, and
re-attaching to surviving bodies instead of rebuilding the target is a change to the recorder's
most delicate path, made blind.

What has changed is only that a deaf take has a second place to show up: the `outcome:` block
sits beside the `ragdoll_rebuilt` rows that were always written. The gap itself remains.

`Lennald_the_Brash_Whiterun_Guard_impacts_log_3` — the standing actor in `stack-onto-standing`,
the take that exists to record limp-on-keyframed contact — contains **zero impact rows, zero
touch, zero separate and zero listener rows.** What it does contain is six `ragdoll_rebuilt`
states, at 1.34 s, 1.86 s, 2.38 s, 2.90 s, 3.42 s and 3.92 s.

Each rebuild detaches the listeners, rebuilds the target, and re-attaches — throttled to
`kReattachTicks = 16`, about 0.5 s. Six rebuilds spaced ~520 ms apart means the reattach never
completes before the next rebuild starts, so the listeners were **off for the entire 2.6 s
window in which the subject was dropped near them**. The take is not empty because nothing
happened; it is empty because nothing was listening.

The old document listed `ragdoll_rebuilt` as a known gap with the note "an equip during a take
holes it". This is the same mechanism doing much worse: a standing actor being disturbed rebuilds
its ragdoll repeatedly on its own, with no equip involved.

**The fix:** re-attach to the bodies that still exist instead of rebuilding the whole target, or
drop the throttle when the rebuild count is climbing. Either way a take that loses its listeners
should say so in the CSV rather than looking like a quiet take.

## 5. `coverage:` never says `bare`

**Fixed, unverified.** A worn `TESObjectARMO` that is nameless **and** weightless, or that
carries a `TNG_` keyword, is treated as skin: still listed under `slots` with a new
`is_skin: true` field, because it genuinely is equipped there, but no longer claiming a site in
`coverage:`. Sites nothing else covers therefore read `bare`, which is what the map's own comment
always promised.

The `coverage:` map reports the heaviest `TESObjectARMO` covering each body site, and TNG's skin
**is** a `TESObjectARMO` occupying slots 0, 2, 3, 7 and 22. So on every stripped take, each site
reads

```yaml
    head: { type: "clothing", name: "", weight: 0.000 }
```

which is the exact opposite of the truth. The map's own comment says "`bare` means skin — no
armour addon draws there", and that value never appears in this dataset.

Nameless **and** weightless is a reliable tell — a real garment has a name, a helmet weighs
2.5 — and `rlib.Take.covering()` applies it. It is correct on the armoured takes without any
help: Iron Helmet → head, Iron Armor → torso/forearms/calves, Iron Gauntlets → hands, Iron Boots
→ feet.

**The fix:** treat an `armour_type: clothing` entry with no name and zero weight as `bare`, or
better, skip forms carrying `TNG_CustomSkin` when building the map.

A second, smaller hole beside it: slots 4, 5, 6 and 8 on the stripped takes emit only
`slot`/`site`/`bones`/`name`/`form`, with no `armour_type` and no `weight`, because the form there
(`NakedTorsoImperial_RBT`) is not a resolvable `TESObjectARMO`. Read `coverage:`, which is built
from the addons and does not have that hole.

## 6. `manifold_first` and `manifold_last` do not bracket a manifold

**Fixed, unverified — by replacing them rather than repairing them.** Every row now carries a
`frame` column: contacts are bucketed by the gap between them, a new frame starting after 2 ms of
silence. The measured gap *inside* a step is 1–5 µs and *between* steps 20.4 ms, so the threshold
sits in the middle of a four-order-of-magnitude gap.

`(frame, limb_index, other_body)` is the manifold key, needs no flags, and is exactly what the
analysis was already reconstructing from timestamps by hand. The flags are still written, and are
now documented for what they are — per-step boundaries, with `manifold_first` picking the
strongest point about three quarters of the time and never to be relied on.

The previous document introduced `manifold_last` so that "one manifold is the run of rows sharing
`(limb_index, other_body)` between the two flags". Measured, they do not pair up:

| | rows |
|---|---|
| both flags set | 1025 |
| `last` only | **244** |
| `first` only | 48 |
| neither | 60 |

and for a manifold that persists across frames — a large minority of contact pairs — both flags
re-fire in most of those frames. So the flags mark per-step boundaries, not a manifold's
lifetime, and 244 rows get a `last` with no `first` at all.

This is not fatal, because grouping by `(frame, limb_index, other_body)` gives the same answer
and needs no flags — and only 5.1 % of groups have more than one row anyway. Two things did get
better: `manifold_first` picks the fastest point of its manifold **74 %** of the time here against
27.5 % on the old data, and the median max/first ratio on a multi-point group is 1.00.

**The fix:** either stop shipping the flags as a bracket, or record the manifold identity Havok
itself uses rather than inferring it from a flag pair.

## 7. The two-actor takes never made contact

**Fixed, unverified.** Two changes:

- **The drop point is the partner's live position**, read at the moment of the trigger, instead
  of the anchor spot both actors were placed against five seconds earlier. Both had drifted from
  it — the standing one on their own feet, the falling one on arrival — and the errors added.
- **The distance is measured and reported.** More than `kContactMetres` (1 m) apart at the
  trigger and the run says so in a corner message and the log, with the figure going into the
  sidecar as `outcome.actors_apart_m`.

Separately, and found while looking at this: **Skyrim's ragdolls do not collide with each other
at all.** `bhkCollisionFilter`'s per-layer bitfields do not set the biped layers against one
another — it is why corpses lie through each other in the vanilla game. Even a perfectly placed
take would have recorded nothing. `RagdollCollision.cpp` forces those pairs on around the takes
that need them and restores the table afterwards; the sidecar note says so, so those takes are
never mistaken for vanilla behaviour.

Three of the thirteen takes (`two-actors`, `stack-both`, `stack-onto-standing`) exist to record
one ragdoll hitting another. In all three the two actors stay **1.37–1.65 m apart** at their
closest measured limb-to-limb distance, and **zero** cross-actor contacts were recorded. Every
`DeadBip`-layer row in the dataset is an actor hitting itself.

That is a run problem, not a recorder problem: body pointers are stable across the whole session,
so `lib_events.py`'s registry resolves a cross-actor limb by name the instant one appears. It
reports `cross_actor: 0` on all 552 episodes.

**The fix:** place the second actor at contact distance and verify it, e.g. by requiring the two
actors' bounding spheres to overlap before triggering, and abort the take with a notice
otherwise — the same shape as the existing "skipped, nobody near" path.

## 8. The run paralyses, so `ragdoll_end` and the get-up are never recorded

**Fixed, unverified.** A fourteenth take, `getup`, runs last: the standard shove with **no
paralysis at all**, held open for 10 s so the handover and the whole get-up animation are on
record. It is the only take in the run that lets go, which is why it is last — everything before
it stays comparable.

The drops are covered separately by `HoldLimp` and `outcome.ended_ragdolled` (§1).

Paralysis is what makes the takes comparable, and it is also why there is no `ragdoll_end`, no
`knock_get_up`, no `getup` phase and no `animated` impact row anywhere in the 12 files. The
blend-out artefacts that dominated the old dataset's top end are gone — so nothing large in this
set is a blend artefact — but the window
[07 §1](07-Reliability-Requirements.md#1-gate-on-ragdoll-state-and-suppress-the-get-up-blend)
says to hold silence through is now **entirely unmeasured**.

It also means the paralysis is doing more than it is credited with. `fall-10m`'s subject landing
*on their feet* is exactly what it looks like when the limp state does not hold all the way to
the ground — so the mechanism that makes the takes comparable is also the mechanism that quietly
invalidated one of them.

**The fix:** one extra take at the end of the run that does the standard shove *without*
paralysis and stays open for 6 s, so the handover and the get-up are on record once. And for the
drops, assert the actor is still ragdolling at the moment of contact.

## 9. The video clips' cut point is not recorded

**Fixed, unverified.** Each take's sidecar gains a `video:` block with `clip`, `clip_start_ms`,
`clip_end_ms`, `lead_in_ms` and `height`, so the subtraction the `obs:` comment asks for is a
number in the file:

```
clip_time_ms = t_ms + obs.offset_ms - video.clip_start_ms
```

It is written **before** ffmpeg runs and before the original is deleted, so a failed cut still
leaves every sidecar saying where its clip would have started — which is what anyone would need
to cut it by hand.

A hand-started take never had this problem: it owns its own recording, so the file in the folder
*is* the one the offsets refer to.

`obs.offset_ms` and the two-row `_sync.csv` align `t_ms` against the **uncut** OBS recording,
which is not in the folder. What is in the folder is nine cut clips, and nothing says where each
cut starts. The sidecar's own comment admits it — "subtract the clip's own start before using
them against a cut file" — without providing the number.

Measured: every clip is the take's duration plus **exactly 4.00 s**, and on take 1 the trigger
(`ragdoll_start` at 818 ms) lands at video-time 4.03 s, so the lead-in is about **3.2 s** and the
tail about 0.8 s. `Scripts/18_video_sync.py` recovers it per clip by frame differencing and lands
within ±100 ms on seven of the nine; it fails on takes 7 and 8, where the second actor moves more
than the subject does.

**The fix:** have whatever cuts the clips write the cut point into the sidecar, or stop cutting
and ship the offset into the long file.

The sync data itself is good news: the two rows agree to **5 ms over a 6.8 s take** with
`rtt_ms: 0` on both, so the drift the header warns about is not measurable at this length and two
points are plenty.

## Smaller things

- ~~**`session_stop` carries `seq` 0.**~~ **Fixed, unverified** — it takes the next sequence
  number like every other row, so ordering by `seq` no longer puts the last row first.
- **`session.terrain_sampled_at` is the last sample, not the first** — metres from where the take
  started, because the per-tick lookup follows the actor through the knockdown.
- **`recording.columns` is a documentation block inside machine-read metadata.** A naive
  key-value YAML reader lands `columns.phase`'s prose in the same namespace as
  `recording.file_index`. Harmless, but it wants a nesting level of its own.
- ~~**The subject faceplants instead of falling backwards.**~~ **Fixed, unverified** — the
  subject is re-placed, and so re-faced, immediately before the push rather than only at the top
  of the step, so whatever their package did during the settle is undone. Cheaper than
  suppressing the package, and it corrects position drift at the same time.
- ~~**`recording.note` should say what happened, not what was intended.**~~ **Fixed,
  unverified** — `recording.note` still says what the take was *for*, which is worth keeping, and
  a new `outcome:` block beside it says what it did: `ended_ragdolled`, `ended_prone`,
  `actors_apart_m` and `ragdoll_collision_forced`. The three failure modes that cost this run
  four takes are all visible in it.

## What the recorder gets right, confirmed on this data

- **Contact listeners, not per-frame sampling.** `impact_speed` is Havok's own
  `separatingVelocity` at the contact point, computed inside the solver. The two 3 m drops land at
  the time free fall predicts, and their contact-point peaks exceed the centre-of-mass figure the
  way a whipping limb should — behaviour no frame-differenced detector would produce.
- **Listeners attached at record-start, not at knockdown.** The first impact lands 11 ms after
  `ragdoll_start`, and in two takes *before* it.
- **`tangent_speed` produces numbers**, ρ=0.42 with normal speed, and it separates a drop from a
  shove (ratio 0.60–0.99 versus 1.47–3.61). The column that replaced `slide_speed` measures
  something real. It has not been checked against an actual scrape, because the take meant to
  provide one was not one.
- **`phase` works**, in the narrow sense that it is present and correct on every row; this dataset
  simply contains only one phase.
- **`material_source` works** — `shape` on all 1348 resolved rows. The `terrain` branch has never
  fired, because there is not one `Ground`-layer contact in the set.
- **`limb_radius` works** and is constant per limb up to actor scale: hand 8.3, foot 10.0,
  forearm 11.7, upper arm 14.5, head 15.1, spine 17.3–23.1, thigh 20.5.
- **`listener` rows work.** The player's position is on every tick, the drift across a take is
  0.00 m, and distance-to-impact is a subtraction — 0.45 m to 21.3 m across the set.
- **Reverb works.** All 12 takes carry `DYLN_ASPC_Int_Wood_Large` and the full
  `BGSReverbParameters` block. This is the first take set where those fields are non-null.
- **`dropped` works and reads 0 on every take**, with `complete: true` in the `session:` block.
  Completeness no longer has to be established from a log.
- **`recording.note` names the take** — which is how the four bad ones could be identified at all,
  even though the name is the intent rather than the outcome.
- **`other_body` is session-stable**, which is what makes both self-collision pairing and
  cross-actor identification possible at all.
- **Lock-free ring, sequence-stamped MPSC queue, drained off the game thread.** No allocation or
  game API inside the physics callback, and `contactPointCallbackDelay` saved and restored.

## Issues that are inherent rather than bugs

- **Row counts are not collision counts.** 1377 impact rows describe 552 episodes, a 2.5×
  over-count, and the multiplier ranges 2.02–3.59 per take. Fully decomposed in
  [03](03-Reduction-and-Cleaning.md). The recorder is faithfully reporting what Havok said;
  collapsing at capture time would remove the choice
  [07 §3](07-Reliability-Requirements.md#3-one-collision-must-produce-one-decision) has to make.
- **The armour snapshot is start-of-take only.** Still true. The run dresses the subject *between*
  takes for exactly this reason, and this dataset has no mid-take equip.
- **`t_ms` is wall-clock, not physics time.** Stamped in the callback, so it resolves the frame
  the step ran on, not the substep the contact occurred in. Precision is one frame — measured
  20.4 ms median here. Fine for everything in this analysis; worth knowing before anyone tries to
  fit an impulse response to it.
- **Per-contact terrain lookup would need the game thread.** The per-tick sample at the actor is
  an approximation that fails across a texture seam. Untestable here, and
  [08 §7](08-Audio-Surfaces.md#7-terrain-resolves-through-ltex-not-through-the-shape) has a better
  answer anyway: read the landscape texture at the contact point and take its `MNAM`.
- **4.5 % of body-on-body contacts resolve no material** — a skin shape with no `bhkShape` behind
  it. Nothing stands in for them.
