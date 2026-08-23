# 04 — Findings

All figures from `Scripts/`, on the 552 collision episodes described in
[03](03-Reduction-and-Cleaning.md). Anything about magnitude excludes `stack-both`, which a
scripted impulse blew up — 439 episodes remain, 261 of them against the world. The two takes
that would have set the top of the curve were discarded before the analysis started
([01 §Discarded](01-Dataset-Map.md#discarded)), and that shows up everywhere below.

## The shape of a knockdown

```
ragdoll_start ─┬─ first contact at +11 ms  (range -9 .. +138 ms)
knock_explode  │
               ├─ loudest contact at +0.75 s   (p10 +0.69 s, p90 +0.93 s)
               │
               └─ last contact at +1.39 .. +2.79 s
                  ... and the take closes with the actor still limp
```

**There is no `ragdoll_end` and no `knock_get_up` anywhere in this dataset.** The run paralyses
the subject at the trigger and closes 6 s later, so the ragdoll window never shuts. That has
two consequences worth holding on to:

- The old dataset's biggest artefact source — the ragdoll→animation blend-out, which produced
  every one of its five impacts above 700 units/s — **is entirely absent here.** Whatever large
  numbers this set contains are not blend artefacts.
- The get-up window itself is **no longer measured at all.** The old figures (window 1.19–2.98 s,
  median 1.48 s; `ragdoll_end` and `knock_get_up` the same instant) came from data that has been
  deleted, and nothing here re-derives them.

The two takes whose first contact is *negative* are not a paradox: state rows are pushed by the
32 ms tick and contacts are stamped in the callback, so `ragdoll_start`'s timestamp can lag its
own first contact by up to a tick. The `phase` column on those rows already says `ragdoll`.

**A knockdown is a fairly fixed budget of contacts**, across three outfits:

| take | episodes | ≥100 u/s | ≥300 u/s |
|---|---|---|---|
| repeat-1 (naked, shove) | 55 | 18 | 2 |
| repeat-2 (naked, shove) | 59 | 19 | 3 |
| repeat-3 (naked, shove) | 46 | 13 | 1 |
| light-armour (shove) | 50 | 15 | 2 |
| heavy-armour (shove) | 39 | 14 | 1 |
| light-armour-fall (3 m) | 34 | 18 | 6 |
| heavy-armour-fall (3 m) | 27 | 18 | 7 |
| two-actors, subject (shove) | 41 | 21 | 2 |
| slide — actually an extreme push | 61 | 29 | 16 |

**~30–60 contacts, of which ~15–30 are worth hearing and 1–16 are the event** — and the number
that are *the event* is what the input varies, not the total. That is the budget any triggering
scheme has to fit a sound design into.

## Impact magnitude — and no measured ceiling

Episode peak closing speeds, world contacts, excluding `stack-both`:

| range (units/s) | m/s | n |
|---|---|---|
| 5–20 | 0.07–0.29 | 46 |
| 20–80 | 0.29–1.14 | 91 |
| 80–160 | 1.14–2.29 | 47 |
| 160–320 | 2.29–4.57 | 49 |
| 320–640 | 4.57–9.14 | 26 |
| 640–1000 | 9.14–14.29 | 2 |

**41.4 % of world episodes peak at ≥100 units/s and 11.9 % at ≥300** — hotter than the old
dataset's 29.6 % / 4.7 %, because the input is a fixed 6.0-magnitude shove rather than a hand
yank. 5.7 % sit within 2× of the 5 unit/s recording floor, so the floor still is not cutting
into anything that matters.

**The top of the range is not established.** The highest clean world episode in the set is
**794 units/s (11.3 m/s)**, and it is in take 4 — the take named `slide` that is actually an
extreme push, so it is not a natural fall either. `fall-3m` and `fall-10m` were both discarded,
so the one thing this run existed to settle, it did not.

What is left is the two 3 m armoured drops, and they are at least *consistent* with physics:

| | light-armour-fall | heavy-armour-fall | free fall from 3 m |
|---|---|---|---|
| first contact after the drop | +754 ms | +539 ms | — |
| loudest contact | +878 ms | +795 ms | 782 ms |
| peak closing speed | 855 u/s = 12.2 m/s | 600 u/s = 8.6 m/s | 7.67 m/s = 537 u/s |

The landing *times* land on the prediction. The peaks exceed the centre-of-mass figure by 1.6×
and 1.1×, which is expected — a whipping limb's *contact point* moves faster than the body's
centre falls, and that is exactly why `impact_speed` is the right column rather than
`body_speed`. But two drops from one height is not a curve.

**So: the useful dynamic range observed here is 20 → 800 units/s, i.e. 0.3 → 11.4 m/s, and the
real ceiling is unknown.** Everything below ~20 units/s is a settle. The old advice to treat
700 units/s as a blow-up guard is certainly too tight — two clean takes exceed it — but nothing
here says what the right number is. That needs a fall take that works
([06](06-Gaps-and-Requested-Captures.md)).

## Which limbs hit, and how hard

Excluding `stack-both`, world and self contacts together:

| site | episodes | median v | p90 v | median KE | p90 KE | self % | median tangential | radius | share of ≥200 u/s hits |
|---|---|---|---|---|---|---|---|---|---|
| head | 14 | **222** | 496 | 24.5 J | 102 J | **0 %** | 133 | 15.1 | 9.3 % |
| torso | 90 | 105 | 261 | 5.9 J | 61 J | 37 % | 172 | 17.3–23.1 | 17.4 % |
| hand | 86 | 91 | 335 | 3.4 J | 46 J | 59 % | 174 | 8.3 | **24.4 %** |
| foot | 42 | 64 | 447 | 1.3 J | 63 J | 17 % | 192 | 10.0 | 10.5 % |
| leg | 98 | 63 | 331 | 2.5 J | 67 J | 48 % | 168 | 19.7–20.5 | **27.9 %** |
| arm | 109 | 48 | 183 | 0.7 J | 10 J | 37 % | 170 | 11.7–14.5 | 10.5 % |

The head is still by far the hardest-hitting site per contact — median 2.1× everything else —
and still the rarest, at the end of the longest whip chain. What has changed is that **not one
head contact in this dataset is the actor hitting themselves** (it was 14 % before), and that
**legs and hands share the loud contacts** at 27.9 % and 24.4 % of everything above 200 u/s,
where the old data gave hands a clear lead. Both are consequences of the fall being a forward
faceplant driven from the chest rather than a sideways leash yank.

**Self-collision is 46 % of all contacts** and cannot be ignored. Hands and legs are the worst
offenders at 59 % and 48 %.

## Left/right is not symmetric, and that is the ragdoll's fault

The vanilla biped ragdoll's masses are not anatomical and not mirrored. Proventus, scale 1.0:

| limb | mass | limb | mass |
|---|---|---|---|
| L Forearm | 2.0 | **R Forearm** | **6.0** |
| L UpperArm | 2.0 | R UpperArm | 2.0 |
| L Hand | 4.0 | R Hand | 4.0 |
| Head | 4.0 | **Neck** | **6.0** |
| Spine | 4.0 | Spine2 | 7.0 |
| COM | 50.0 | Foot | 3.0 |

A hand is heavier than the upper arm it hangs off; the neck is heavier than the head; the right
forearm is three times the left. **Lennald's vector is this one times exactly 1.03 — his
`scale`, limb for limb** — and his 70-point weight slider changes nothing. So ragdoll mass is
the skeleton's table times `scale`, and per-actor mass is a real but very simple input.

The arm episodes show the consequence cleanly: left arms have median closing speed 50.7 over 57
episodes, right arms 48.2 over 52 — statistically the same motion — while the masses behind them
differ 3×. **Any loudness derived from `0.5·m·v²` will be three times louder on the right arm
than the left for the same movement.** Use a per-site nominal mass table of our own.

`limb_radius` is the honest size input beside it: hand 8.3, foot 10.0, forearm 11.7, upper arm
14.5, head 15.1, spine 17.3–23.1, thigh 20.5 game units. Note it does **not** rank the way mass
does — a thigh is bigger than the 50-mass COM body.

## Armour does not touch the physics — now shown against a fixed input

Same scripted shove, three outfits, same room, same actor. All episodes, world and self
together:

| | episodes | median v | p90 v | max v | total KE |
|---|---|---|---|---|---|
| naked (repeat-1) | 55 | 67.9 | 207 | 538 | 857 J |
| light armour | 50 | 49.5 | 263 | 373 | 715 J |
| heavy armour | 39 | 61.9 | 255 | 398 | 658 J |

And the 3 m drops:

| | episodes | median v | p90 v | max v | total KE |
|---|---|---|---|---|---|
| light armour | 34 | 136.2 | 537 | 855 | 1271 J |
| heavy armour | 27 | 139.6 | 521 | 600 | 1021 J |

Every difference is inside the run-to-run spread below. The old dataset showed the same thing
with a hand-driven input, which left room for doubt; with a byte-identical scripted shove there
is none. **Armour is purely a sound-selection axis.** It changes what should be heard — mail
rattle, plate clank, bare flesh — and nothing about when or how hard. The intensity model can be
trained on any take regardless of what the actor was wearing.

Per contact, keyed on what was on **that limb** rather than on the outfit as a whole — from the
sidecar's coverage map — the picture is the same: `bare` 262 episodes at median 68.0 u/s,
`light` 84 at 74.0, `heavy` 93 at 61.9.

The mass vector confirms it from the other side: Proventus's 18 limb masses are bit-identical
across all his takes, naked and in a 30-weight iron cuirass alike.

## Repeatability — and it is Havok's spread, not the input's

`repeat-1/2/3` are the same `PushActorAway` magnitude 6.0, from the same distance and heading, on
the same actor, in the same spot, 13 seconds apart, and the three falls are indistinguishable on
video.

| | episodes | median v | p90 v | max v | total KE | ≥100 | ≥300 |
|---|---|---|---|---|---|---|---|
| repeat-1 | 55 | 67.9 | 207 | 538 | 857 J | 18 | 2 |
| repeat-2 | 59 | 52.5 | 218 | 543 | 935 J | 19 | 3 |
| repeat-3 | 46 | 53.5 | 214 | 355 | 741 J | 13 | 1 |

Spread about the mean: episode count **±12 %**, median intensity **±13 %**, p90 **±3 %**, peak
**±20 %**, total energy **±11 %**, count above 300 u/s **±50 %**.

The old doc quoted ±5 % / ±8 % / ±50 % from two hand yanks and warned they were the weakest
numbers in the set. They were roughly right, and now they mean something stronger: **with the
input held exactly fixed, the spread is unchanged.** It is Havok's own frame-to-frame
nondeterminism, not the experimenter's hand. Tune against distributions across takes; never
against one recording's maximum.

The `p90` being the most stable statistic at ±3 % is worth noticing — it is a better calibration
target than either the median or the max.

## Frame rate — not measured, and that is a regression

Every take in this dataset ran at **48–51 fps**. There is no frame-rate contrast in it at all,
because the run never leaves the room it starts in.

Within-take correlation of frame gap against `impact_speed` still shows no consistent sign, and
ρ(fps, episodes per second) across takes is near zero against a clearly negative ρ(fps, impact
rows per second) — the same pattern as before, episodes being the frame-rate-invariant unit and
rows not being. But over a 3 fps span that is not a test.

The old dataset's frame-rate finding — magnitude not biased, density weakly biased, episodes
invariant — rested on a 30 fps vs 50 fps contrast across six rooms, and that data is gone. Treat
the conclusion as inherited and unconfirmed.

What has not changed is the **timing precision** argument, which is arithmetic rather than
measurement: at 24 fps a contact is located to ±42 ms, at 144 fps to ±7 ms. Any clustering
window must be expressed in frames, or in a duration comfortably above the worst-case frame
time — never a fixed 10 ms.

## Simultaneity — the reason this needs an aggregation scheme

Pooled over the 368 impact frames excluding `stack-both`: **2 or more distinct limbs impact in
238 of them (65 %)**, and 3 or more in **33 %**, with a single-frame maximum of 12. Widening to
a 100 ms window — roughly the time a listener fuses into one event — the per-take p95 is
**10–13 distinct limbs**, maximum 14.

Every one of those numbers is worse than the old dataset's (59 %, 27 %, p95 9–13, max 15). A
naive one-sound-per-contact system would try to start a dozen voices inside a tenth of a second
at the moment of landing. That is the central design problem, and it is what every knockdown
does.

## Surfaces and geometry — one room, and thinner than it looks

World episodes, excluding `stack-both`:

| material | episodes | median v | median tangential | comes from |
|---|---|---|---|---|
| Carpet | 170 | 81.4 | 176 | every take |
| Wood | 75 | 35.9 | 157 | every take |
| WoodStairs | 11 | **181.0** | **443** | **take 4 alone** |
| Stone | 5 | 180.6 | 262 | light-armour (4), heavy-armour (1) |

Stairs hit five times harder than the flat wood beside them and scrape three times as hard,
which is the old `StoneStairs` finding reproducing on a different material. A step edge is a
concentrated collision. **But all eleven of those episodes are in one take**, and that take is
the extreme push, so the effect and the input are confounded.

Contact normals, world episodes, excluding `stack-both`: **239 on a flat floor or ceiling**
(`|nrm_z| > 0.95`), 12 on a slope, and **10 on a wall** (`|nrm_z| < 0.2`) — 3.8 %, hitting at
median 143 units/s against 65 on the floor.

**The wall gap is not closed.** 3.8 % is the same share the old dataset had, and only two takes
contribute any wall contact at all: take 4 (7 episodes) and `light-armour` (3).

What is completely missing is **any natural ground at all** — no `Ground`-layer contact exists in
this dataset, so `material_source: terrain` never fires and the terrain resolver remains written
but untested.

## The tangential axis — measured for the first time, not yet validated

`tangent_speed` is the first working measurement of tangential motion in any of this research.
On world contacts excluding `stack-both`:

- median **177 units/s** (2.5 m/s), p90 440
- ρ with `impact_speed` = **+0.42** — a separate axis, not a restatement of intensity
- **52 % of episodes have tangential > 2× normal**; **12 % have tangential < 0.5× normal**

So half of all ragdoll contacts are grazing rather than perpendicular, and a sound bank of pure
thuds is the wrong shape. That much the column supports.

What it does **not** yet support is a scrape/thud classifier, because **nothing in this set is a
scrape.** The take built to provide one turned out to be an extreme push
([01](01-Dataset-Map.md)), and its tangential-to-normal ratio is unremarkable:

| take | n | median tangential | median tan/normal |
|---|---|---|---|
| slide — actually an extreme push | 35 | **445** | 2.38 |
| light-armour (shove) | 39 | 197 | **3.61** |
| repeat-2 (shove) | 36 | 180 | 3.14 |
| repeat-3 (shove) | 30 | 200 | 3.04 |
| heavy-armour (shove) | 29 | 156 | 2.17 |
| repeat-1 (shove) | 33 | 139 | 1.47 |
| heavy-armour-fall (3 m drop) | 13 | 259 | **0.99** |
| light-armour-fall (3 m drop) | 10 | 168 | **0.60** |

Take 4 has the highest *absolute* tangential speed in the set, but that follows from it being
the hardest push — its normal speeds are the highest too, and its ratio sits mid-table, below an
ordinary armoured shove.

The one contrast that *is* clean is **fall versus shove**: both 3 m drops come out
normal-dominated at ratios of 0.60 and 0.99, while every shove is tangential-dominated at
1.47–3.61. A body arriving straight down looks different from a body being pushed across a
floor, and the column sees it. That is a real result, and it is a weaker one than a slide take
would have given.

The within-take spread is enormous — p10 around 0.2–1.0, p90 around 5–17 — so the ratio is a
per-contact property, not a take property, and any classifier has to work per contact.

## Character-on-character — still zero

Three of the thirteen takes existed to record it, and all three missed: closest limb-to-limb
distance 1.37–1.65 m. Every one of the 638 `DeadBip`-layer contacts is an actor hitting
**itself**. `episodes.csv` reports `cross_actor` false on all 552 rows.

The recorder side is ready — body pointers are session-stable, so `lib_events.py`'s registry
names a cross-actor limb the moment one appears. The run's placement is what needs fixing.

One thing the takes do give: `Lennald` in guard heavy armour taking a scripted impulse
(`stack-both`) reached 2390 units/s / 34 m/s with limbs spinning at 328 rad/s. That is not a
fall, it is what the mod will see when *another* mod throws an actor, and it is a useful artefact
class to have on file.

## Where the listener was

`listener` rows work. The player never moved in any take (0.00 m of drift across 185–249 rows),
which is the run doing what it says. Distance from the player to each impact ranges from 0.45 m
to 21.3 m, with per-take medians of 1.1–15.4 m — so there is genuine distance variation to
calibrate attenuation against, and the geometry is a subtraction from the impact row's own
position.

## Candidate loudness metrics

Spearman correlation with the episode's peak closing speed, excluding `stack-both`:

| metric | ρ |
|---|---|
| kinetic energy `0.5·m·v²` | **0.967** |
| momentum `m·v` | 0.882 |
| body COM speed | 0.455 |
| tangential speed | 0.419 |
| angular speed | 0.322 |
| limb radius | 0.002 |
| limb mass | −0.018 |

Unchanged from the old dataset in every respect that matters. Mass is statistically independent
of impact speed, and within one body the velocity spread dwarfs the mass spread — only six
distinct mass values exist. **KE and closing speed rank impacts almost identically.** The mass
term is not doing separation work, it is doing *timbre* work: it says "this was a torso, not a
hand". Which is the right way to use it.

`limb_radius` is equally uncorrelated with intensity and equally useful as a timbre input, with
the advantage that it is a real bounding radius rather than a fiction of the ragdoll author's.

## Can we build the system from this?

Yes for the core case, with a narrower base than the take list suggested: a humanoid ragdoll
falling onto hard flat interior surfaces, with limb, surface material, contact position, contact
normal, tangential motion, per-limb armour coverage, listener distance and the self-versus-world
distinction all available per contact, at frame precision, reproducibly, in a cell with real
reverb parameters.

Not yet for: **the top of the intensity curve** — both fall takes were discarded and the highest
clean contact here is an extreme push; **scrape versus thud as a classifier** — nothing in the
set is a scrape; natural ground of any kind; character-on-character impacts; the get-up blend;
any frame rate other than ~50; non-human skeletons; female actors; or anything about how the
result should actually *sound* — there is still no audio analysis in this dataset, only video
that has not been auditioned. See [06](06-Gaps-and-Requested-Captures.md).
