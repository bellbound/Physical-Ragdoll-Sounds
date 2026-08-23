# 07 — What a runtime sound system has to hold to

Not a design — the design comes later. These are the constraints the data says the design cannot
violate, each traceable to a measurement.

## 1. Gate on ragdoll state, and suppress the get-up blend

The ragdoll rigid bodies exist and collide the whole time — keyframed while the actor is
animated — so without a state gate the mod plays impact sounds while NPCs walk around.

**This dataset gives no evidence for the size of that problem.** Every impact row in it is
`phase == ragdoll`, because the scripted run paralyses the subject and closes the take before any
get-up. The figure the requirement rests on — 41 % of contacts from animated limbs, with a speed
distribution that no threshold separates — comes from the deleted dataset, and nothing here
re-derives it. The requirement stands on the mechanism, which has not changed: the bodies are
there and they collide.

The recorder's shape is worth copying rather than reinventing: only the game thread may ask an
actor whether it is ragdolling, and the contact callback runs on a Havok worker thread inside the
solver, so the answer cannot be fetched where the decision is made. Publish it from the game
thread into an atomic and read it with one relaxed load. The cost is that the gate is one tick
coarse at the boundary, which is the right price — and it is visible in this data: **two of the
eleven takes with a ragdoll window have their first contact 9 ms before their own `ragdoll_start`
row.** The contacts are stamped `ragdoll` correctly; it is the state row that lags.

**Stop listening at `ragdoll_end`, and hold silence for a short blend window after it.** How
short is currently unmeasured. Record one unparalysed take before picking a number.

## 2. Reject physics blow-ups explicitly

The cheap check the old document recommended is the right one. The thresholds it named are wrong,
and **this run did not manage to replace them.**

**Use the arithmetic, not a threshold.** `impact_speed` is the solver's closing speed;
`normal_speed` is the same quantity recomputed from both bodies' motion state. A row where they
disagree is a row the rigid-body arithmetic cannot reproduce, which is what a blow-up is. On this
dataset a 10 % disagreement flags 23 of 1377 rows, and it turns on exactly where it should: 0 %
below 20 u/s, still under 4 % right through the range the kept takes occupy, then 20 % between
1000 and 2000 u/s and 50 % above — which is where the one contaminated take lives.

**Compare magnitudes.** `normal_speed` is exactly negated on 23.3 % of rows
([05 §3](05-Capture-Pipeline-Issues.md#3-normal_speeds-sign-is-unreliable)). Signed, the test
fails on a quarter of good data.

On fixed guards as a backstop, this is all the data supports:

| guard | old value | what the data says |
|---|---|---|
| `impact_speed` | > 700 u/s | **Too low, but the right value is unknown.** Two clean takes exceed it — `light-armour-fall` at 855 and the extreme push at 794 — so 700 would reject real contacts. Both takes that were meant to establish the ceiling were discarded ([01 §Discarded](01-Dataset-Map.md#discarded)), so nothing here says where it belongs. 1000 u/s is a safe floor for the guess, not a measurement |
| `angular_speed` | > 25 rad/s | **Far too low.** 8.3 % of rows in the kept takes exceed 25, and only 7 of those 90 fail the arithmetic check. Their peak closing speed reaches 855 u/s — real contacts. 200 rad/s is where failures become common |

The `fall-10m` take existed to re-derive the first number and it could not: its subject landed on
their feet, which is not a ragdoll impact. **That capture is still outstanding, and it is the
single highest-value one on the list** ([06](06-Gaps-and-Requested-Captures.md)).

## 3. One collision must produce one decision

1377 impact rows describe 552 collisions — a 2.5× over-count, ranging 2.02–3.59 per take
([03](03-Reduction-and-Cleaning.md)). The runtime has to collapse, per frame:

- **Both directions of a limb-on-own-limb contact → one event.** This is now the biggest
  multiplier by far: 46 % of all contacts are self-collisions and **every single one of them fires
  twice** — 624 of 624 ordered pairs have their mirror in the same frame, ×2.00 with no exceptions.
  A runtime with listeners on every limb sees both live and can drop the mirror by taking only the
  pair where its own body sorts first.
- **All contact points of a manifold → take the max.** ×1.07 here, and only 5.1 % of contact
  groups are multi-point at all. **Do not use `manifold_first`/`manifold_last` as a bracket** —
  measured, 244 rows carry `last` with no `first`, and both flags re-fire on most frames of a
  persisting manifold. Group by `(this body, other body)` within the step and accumulate the max:
  one float and one comparison per contact point, no buffering, no look-ahead. `manifold_first`
  does happen to be the fastest point 74 % of the time on this data, and the median max/first
  ratio is 1.00, so the cost of getting this wrong is smaller than the old 27.5 % figure suggested.
- **Multiple `other_body`s at the same contact point → one event**, with a documented rule for
  which material wins. **This did not happen once in this dataset** — 708 world limb-frames, zero
  coincident colliders. The old 19 % came from one custom house mod's outdoor geometry, so it is
  real but local. A rug on a floorboard is still a common case and still needs a tie-break; it is
  just not a tax everywhere.

Without this the loudest moment of a knockdown is also the moment the system double-triggers
hardest, because that is when the most manifolds are open at once.

## 4. Everything time-based must scale with frame time

The target range is 24 to 144 fps (7–42 ms per frame). A fixed millisecond clustering window would
behave as a different system at each end.

Express windows in frames, or as `max(k · frameTime, floor_ms)`. The reduced unit — one collision
episode — appears to be frame-rate invariant: ρ(fps, episodes/s) is near zero against a clearly
negative ρ(fps, impact rows/s), the same pattern the old dataset showed more strongly.

**But this dataset cannot test it.** Every take ran at 48–51 fps. The frame-rate independence
result is inherited from data that has been deleted, and re-confirming it needs two runs at two
capped frame rates ([06](06-Gaps-and-Requested-Captures.md)).

What is not in doubt is timing precision, which is arithmetic: at 24 fps a contact is located to
±42 ms, at 144 fps to ±7 ms.

## 5. Budget for a dozen simultaneous contacts

**65 % of impact frames carry 2 or more distinct limbs and 33 % carry 3 or more**, with a
single-frame maximum of 12. Over a 100 ms window — roughly what a listener fuses into one event —
the per-take p95 is 10–13 distinct limbs, maximum 14.

All of them are worse than the old dataset's, on a harder and more repeatable input. This is the
normal case at the moment of landing, not an outlier.

Whatever the aggregation scheme is, it needs a hard voice cap and a deterministic priority order,
and it needs to behave when several actors are ragdolled at once — a case this dataset still
contains no example of.

## 6. Do not use the Havok limb mass directly

The vanilla ragdoll's masses are asymmetric (R Forearm 6.0 vs L Forearm 2.0) and non-anatomical
(hand heavier than upper arm, neck heavier than head). The arm episodes show the consequence
directly: left arms median 50.7 u/s over 57 episodes, right arms 48.2 over 52 — the same motion —
with 3× the mass behind one of them. **A KE-based loudness would be three times louder on the
right arm for identical movement.**

Use our own per-site nominal mass, scaled by the actor's `scale`. The scaling rule is now measured
and trivial: **Lennald's mass vector is Proventus's × 1.03, his `scale`, limb for limb.** The
weight slider does nothing. So read the Havok mass only to recover `scale`, or read `scale`
directly.

Two more things about mass:

- **The sidecar's mass may be 0.** Three of twelve takes snapshot it while the bodies are
  keyframed
  ([05 §2](05-Capture-Pipeline-Issues.md#2-ragdolllimbsmass-is-0-in-three-of-twelve-takes)). A
  runtime reads it live and does not have this problem, but anything reading these files does.
- **For "how big is this limb", `motionState.objectRadius` is better than mass** — free inside the
  callback, a real bounding radius, and constant per limb: hand 8.3, foot 10.0, forearm 11.7, upper
  arm 14.5, head 15.1, spine 17.3–23.1, thigh 20.5. It does **not** rank the way mass does (a thigh
  is bigger than the 50-mass COM body), which is exactly why it is worth having beside it. On an
  unrecognised skeleton (§7) it is the one honest input left for sizing a sound.

## 7. Never assume the limb set

18 bodies with a fixed order is *this* skeleton. Draugr, creatures and modded skeletons differ, and
`ragdoll_rebuilt` fires on cell change and 3D reload — and, this dataset shows, **six times in
three seconds on a standing actor that gets disturbed**, which is what emptied one take entirely
([05 §4](05-Capture-Pipeline-Issues.md#4-six-ragdoll_rebuilt-events-swallowed-a-whole-take)).

Map bones by **name**, resolve on every attach, degrade to a generic body-sized sound when a name
is not recognised rather than indexing blindly — and **do not throttle the re-attach so long that a
rebuild storm leaves you deaf.** Re-attaching to bodies that still exist is cheaper than rebuilding
the target.

## 8. Materials are unreliable at the edges

- **4.5 % of body-on-body contacts return no material** — a skin shape with no `bhkShape` behind
  it. Unchanged from the old dataset's 4.4 %.
- **Terrain is entirely untested.** There is not one `Ground`-layer contact in this dataset, so the
  `material_source: terrain` branch has never fired in anger. The old finding that terrain never
  resolves through the shape still stands as the reason the branch exists.
- **Adjacent surfaces both reporting on one contact** did not happen once here, but see §3.

So: the collision **layer** is the reliable input (world / body / terrain); the material is an
enrichment. Every material path needs a fallback that sounds acceptable.

Terrain needs a lookup of its own. `RE::TES::GetLandMaterialType(position)` is what the engine's
footstep code uses and is a game-thread call, so sample it per actor per tick and read the
published answer — the same shape as the phase gate in §1. Better still,
[08 §7](08-Audio-Surfaces.md#7-terrain-resolves-through-ltex-not-through-the-shape) shows the
landscape texture's own `MNAM` gives the right answer at the contact point rather than at the
actor. Keep whatever says a material was *sampled* rather than *measured*; at a texture seam they
differ, and a system that cannot tell which it had cannot debug the one time it sounded wrong.

## 9. Fire on first contact

**96.2 % of episodes have their peak on the first impact row**, median peak/first ratio exactly
1.00. The system can trigger immediately with the first contact's magnitude and be right about
nineteen times in twenty: no look-ahead buffer, no waiting a frame to see if it gets louder.

That is up from 88.2 % on the old dataset, which makes it the strongest structural result here.
The remaining 4 % are a candidate for a single "upgrade the voice in flight" rule, not for delaying
everything.

An episode is short: median 2 impact rows, median 79 ms, 41 % of them a single row. The p90 is
377 ms, so a minority do drag on and want a re-trigger guard rather than a new voice.

## 10. Tune against distributions, not against a take

Run-to-run variance on three knockdowns with a **byte-identical scripted input**, 13 seconds apart,
that looked the same on video:

| statistic | spread about the mean |
|---|---|
| episode count | ±12 % |
| median intensity | ±13 % |
| **p90 intensity** | **±3 %** |
| peak intensity | ±20 % |
| total kinetic energy | ±11 % |
| count above 300 u/s | ±50 % |

The old document quoted ±5 % / ±8 % / ±50 % from two hand yanks and warned they were the weakest
numbers in it. They were roughly right, and holding the input fixed did not shrink them — so this
is **Havok's own frame-to-frame nondeterminism**, not the experimenter's hand. Any threshold
calibrated to one recording's maximum will be wrong on the next one.

The **p90 is the stable statistic** at ±3 %, and it is a better calibration target than either the
median or the max.

The other stable quantity is the per-knockdown budget: ~30–60 contacts, ~15–30 above 100 units/s,
and however many above 300 the input decides, over a window whose last contact lands 1.4–2.8 s
after the trigger.

## 11. Where these numbers come from, and what this set cannot measure

Everything above is measured on the twelve files in `NewRecordings/`, hand-picked from one
3½-minute scripted run in Dragonsreach on 2026-08-22. **Four of the run's files were discarded as
bad data**; the fourteen hand-driven takes this research started from have been deleted.

Six things this set **cannot** speak to, and they are not small:

- **The top of the intensity curve.** Both fall takes were thrown out. The highest clean contact
  here is an extreme push at 794 u/s, and §2's guard is still a guess.
- **Scrape versus thud as a classifier.** The take built to isolate a slide was not a slide.
  `tangent_speed` measures something and separates a drop from a shove, but nothing here is a
  scrape.
- **The get-up blend and animated contacts.** Paralysis means no `ragdoll_end`, no
  `knock_get_up`, and 0 % animated rows against the old dataset's 41 %. §1 is a requirement with no
  current measurement behind it.
- **Frame rate.** 48–51 fps throughout. §4's invariance claim is inherited, not confirmed.
- **Surfaces.** One room, one surface family: Carpet and Wood are 94 % of world episodes, all
  eleven `WoodStairs` episodes are in one take, and vertical geometry is 3.8 %. Nothing here says
  anything about snow, dirt or water, and a system tuned only on this data will meet those for the
  first time in the field.
- **Character-on-character contact.** Three takes tried and all three missed. §5's "several
  ragdolls at once" case has no example in either dataset.

Four things it measures better than the old data did, and which should be trusted over anything the
old documents said:

- **Repeatability**, from a genuinely fixed input rather than two hand yanks that happened to look
  similar — and the conclusion that the residual spread is the engine's.
- **Fire-on-first-contact**, at 96.2 % against 88.2 %.
- **The mass model**: skeleton table × `scale`, with the weight slider doing nothing.
- **Armour as a pure sound axis**, now against an identical input.

And two beliefs it corrected outright: both blow-up guards were too tight, and a vanilla cuirass
already covers forearms and calves.
