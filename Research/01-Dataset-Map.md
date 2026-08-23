# 01 — Dataset map

12 files, 42 artefacts: per take a `.csv` of events, a `.yaml` sidecar, an `_sync.csv` OBS
timing pair, and — for the nine Proventus takes — a `.mp4`. 1377 impact rows, 1632 touch,
1203 separate, 2149 listener, 414 limb samples, 65 state changes. Every take says
`dropped: 0`, `complete: true`.

All of it is **one press of Debug → Record Requested Data**, started 2026-08-22 15:38:21 and
finished 15:41:19 — 3 minutes 58 seconds, thirteen takes on fixed timings, in **Dragonsreach**
(`WhiterunDragonsreach`, interior). No `Bones_*.log` skeleton dump was taken for either actor
in this session; the ones in `QuickModMenuNG`'s output folder are from the deleted 12:27
session and describe different actors.

**This folder is curated.** The run wrote sixteen files; four were reviewed against the video
and thrown out. The recorder's output directory still has them and they must stay out — see
[Discarded](#discarded).

## The mapping

`recording.note` in each sidecar names the take. It says what the take was *for*; the last
column is what it holds.

| # | File | Take | What it was for | What it actually is |
|---|---|---|---|---|
| 1 | `Proventus…_1` | `repeat-1` | run-to-run variance, 1 of 3 | ✔ usable. A **forward faceplant**, not the backward fall the run intends. 55 episodes, peak 538 u/s |
| 2 | `Proventus…_2` | `repeat-2` | 2 of 3 | ✔ 59 episodes, peak 543 u/s. Same fall as 1, indistinguishable on video |
| 3 | `Proventus…_3` | `repeat-3` | 3 of 3 | ✔ 46 episodes, peak 355 u/s |
| 4 | `Proventus…_4` | `slide` | scrape rather than thud | ⚠ **not a slide.** An extreme push. Usable as a hard shove — it holds the set's highest clean contact at 794 u/s, all 11 `WoodStairs` episodes and 7 of the 10 wall contacts — but **nothing in it isolates a scrape** |
| 7 | `Proventus…_7` + `Lennald…_1` | `two-actors` | limb on another body | ⚠ **missed.** Closest limb-to-limb distance 1.38 m, zero cross-actor contacts. Both actors' own falls are usable; the take's purpose is not met. Both had also been knocked down repeatedly already and were in a hurt idle when it armed |
| 8 | `Lennald…_2` only | `stack-both` | limp on limp | ⚠ **missed**, and the guard took a scripted impulse instead: 299 impacts, peak 2390 u/s (34 m/s), limbs to 328 rad/s. **Excluded from every magnitude figure**; kept because it is the only sample of what a violent external impulse looks like from inside the callback |
| 9 | `Lennald…_3` only | `stack-onto-standing` | limp on keyframed | ✘ **empty.** 0 impacts, 0 touch, 0 listener rows, six `ragdoll_rebuilt` states. Kept as the evidence for [05 §4](05-Capture-Pipeline-Issues.md#4-six-ragdoll_rebuilt-events-swallowed-a-whole-take) |
| 10 | `Proventus…_10` | `light-armour` | all four pieces | ✔ Leather Armor + Bracers + Boots + Helmet, coverage `light` on all six sites |
| 11 | `Proventus…_11` | `light-armour-fall` | light armour, 3 m drop | ✔ **the only clean drop in the set.** Teleported up 3 m and paralysed in the same frame, no push. Lands at +0.75–0.88 s, peak 855 u/s |
| 12 | `Proventus…_12` | `heavy-armour` | cuirass **and** gauntlets and boots | ✔ Iron Armor + Gauntlets + Boots + Helmet, coverage `heavy` on all six sites |
| 13 | `Proventus…_13` | `heavy-armour-fall` | heavy armour, 3 m drop | ✔ same 3 m drop, lands at +0.54–0.80 s, peak 600 u/s |

Nine takes, of which **six do what their name says**: the three repeats, the two armour shoves
and the two armoured drops. One (`slide`) is mislabelled but usable as something else. Two
(`two-actors`, `stack-both`) missed their purpose while still recording valid falls. One
(`stack-onto-standing`, Lennald) is empty.

`Scripts/16_summary_numbers.py` marks `stack-both` as `CONTAMINATED` and excludes it from
magnitude figures; the empty take drops out on its own.

## Discarded

Four files the run wrote and this folder deliberately does not hold. They are bad data, not
missing data:

| Take | Why it is out |
|---|---|
| `fall-3m` (5) | The teleport blew the solver up. Contact height rises 674 units (9.6 m) in the first 200 ms at closing speeds to 4591 u/s (65 m/s), with limbs spinning at 514 rad/s. It does not record a 3 m fall |
| `fall-10m` (6) | The subject **landed on their feet.** A 10 m drop that ends in an upright landing is not a ragdoll impact, so its numbers describe something else. This is the take that would have set the top of the intensity curve, and it cannot |
| `stack-both` (8), Proventus's file | The subject's half of a take that missed |
| `stack-onto-standing` (9), Proventus's file | The same |

The two fall takes are the expensive loss: **there is now no measured ceiling for the intensity
curve**, and the blow-up guard in
[07 §2](07-Reliability-Requirements.md#2-reject-physics-blow-ups-explicitly) is still a guess.
The only clean drops left are the two 3 m armoured ones.

## Where the run's intent and the data disagree

**The subject was meant to fall backwards, away from the player. They faceplant.** The run
teleports the subject 2.5 m in front of the player facing back at them, waits 800 ms, then
applies `PushActorAway` magnitude 6.0 from the player's position plus paralysis. Between the
teleport and the trigger, the subject's **sandbox package turns them around**, so the shove
lands on their back and they fly forward 1–2 m onto their face. Confirmed on the video at
video-time 4.25 s in take 1.

That is not a data problem — the input is still identical between takes, which is what
`repeat-1/2/3` measure — but *what* the takes contain is a forward faceplant.

**Take 4 is not a slide.** The run's note calls it "flat shove along the floor — scrape rather
than thud"; on the video it is an extreme push. Its tangential speeds are the highest in the
set (median 359 u/s against 139–200 for the repeats), but so are its *normal* speeds, and its
tangential-to-normal **ratio** (median 2.38) sits in the middle of the repeats' 1.47–3.17 and
below `light-armour`'s 3.61. So the take demonstrates a harder shove, not a scrape. Every
scrape statement in these documents is qualified accordingly.

**The two-actor takes never touched.** Takes 7, 8 and 9 all place a second actor to be hit,
and in all three the two ragdolls stay 1.37–1.65 m apart at their closest. Every one of the
638 `DeadBip`-layer impact rows in the set is an actor hitting **itself**. Character-on-
character contact is still entirely unrecorded, and the fix is the run's placement, not the
recorder: cross-actor bodies would have been named, because the pointer registry in
`lib_events.py` resolves them and reports `cross_actor: 0` on all 552 episodes.

## Actors

| | Proventus Avenicci | Lennald the Brash |
|---|---|---|
| form | `0001A67D` (Skyrim.esm) | `000D0FF6` (BecomeKingofSkyrimTNG.esp) |
| base | `ProventusAvenicci` | levelled, `FF000FFE` |
| race | ImperialRace | NordRace |
| sex | male | male |
| level | 4 | 20 |
| weight slider | 0 | 70 |
| scale | 1.0000 | 1.0300 |
| ragdoll bodies | 18 | 18 |
| total ragdoll mass | 125.00 | 128.75 |

Both are the standard 18-body vanilla biped ragdoll, same limb order, same `limb_index`
meaning. **Lennald's mass vector is Proventus's multiplied by exactly 1.03 — his `scale`** —
limb for limb, including the asymmetries. The 70-point weight slider changes nothing. That
settles one thing the old dataset could not: ragdoll mass is the skeleton's mass table times
`scale`, and nothing else.

**Only the subject is dressed by the run.** Proventus is stripped for takes 1–9 and dressed in
leather and then iron for 10–13. Lennald is never touched: he wears his own Whiterun Guard kit
throughout — heavy helmet, cuirass, gauntlets and boots, plus a **shield in slot 9 and a cloak
in slots 10 and 16**, which no take of Proventus has. So his three files are the only ones in
the set with a shield-arm or a cloak on the actor, and his `coverage` map reads `heavy` on all
six sites without the run having arranged it.

**No female actor was recorded.** The old dataset's female/male comparison has no counterpart
here, and it was confounded anyway.

## The environment

Identical in all 12 takes, and it closes a gap the old data could not:

```
cell            WhiterunDragonsreach (000165A3), interior
acoustic_space  DYLN_ASPC_Int_Wood_Large (000C5D0A)
reverb          DYLN_REVB_Int_Wood_Large (000C5D09)
                decay 1290 ms, hf ref 1976 Hz, room -5, room hf -15,
                reflections 0 (+4 ms), reverb 3 (+0 ms), diffusion 100 %, density 100 %
surfaces hit    Carpet, Wood, WoodStairs, Stone  (+ Skin, body on body)
                Carpet and Wood are 94 % of the world episodes; all 11 WoodStairs
                episodes are take 4 alone, and Stone is 5 episodes across two takes
terrain         none - `material_source` is `shape` on every resolved row
```

This is the first take set with a real `BGSReverbParameters` block in it, so the reverb fields
are confirmed working rather than merely present. It is also **one room, one surface family and
one frame rate** — 48–51 fps across all twelve takes — which is the price of a scripted run
that never moves the player.

## Video and sync

Each of the nine Proventus takes has a 1920×1080 60 fps `.mp4` and a two-row `_sync.csv`.
Lennald's three files have no video of their own; takes 7 and 8 are visible in Proventus's
clips for the same takes.

The sync rows are the arm sample and the close sample, and the two agree to **5 ms over a
6.8 s take** (`t_ms` 19 ↔ OBS 6999; `t_ms` 6864 ↔ OBS 13849), with `rtt_ms: 0` on both. The
clock drift the sync file's own header warns about is not measurable at this length — two
points are plenty.

The catch is the **clips are cuts, and the cut point is not recorded.** `obs.offset_ms` is an
offset into the uncut OBS recording, which is not in the folder. Every clip runs the take's
duration plus exactly 4.00 s of padding, and on take 1 the trigger (`ragdoll_start` at
818 ms) lands at video-time 4.03 s, so the lead-in is about **3.2 s** and the tail about
0.8 s. `Scripts/18_video_sync.py` does that by frame differencing and gets within ±100 ms on
seven of the nine; it fails on takes 7 and 8, where the second actor moves more than the
subject does. See
[05 §9](05-Capture-Pipeline-Issues.md#9-the-video-clips-cut-point-is-not-recorded).
