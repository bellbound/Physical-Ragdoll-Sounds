# Physical Ragdoll Sounds — the architecture

How the code is organised, what each part owns, and where a change belongs.
[00-Design.md](00-Design.md) says what the mod should sound like and why; this says how the code is
arranged to do it, and is the place to look when something reads as broken rather than as merely not
to taste.

| Doc | What it is |
|---|---|
| [00-Design.md](00-Design.md) | The design. The asset list, the strategy list, the decisions and why |
| **01-Architecture.md** (this) | The pipeline, the two axes, the modifier contract, and how to extend it |
| [03-Implementation.md](03-Implementation.md) | Build layout, the plugin/testbench split, how to run things |
| [04-Reference-Analysis.md](04-Reference-Analysis.md) | Measurements of the Skate 3 reference clips — where the layer timings and level budgets come from |
| [config.md](config.md) | What a decibel means, and which side of arbitration it lands on |
| [Research/](Research/) | The capture study. What the engine can tell us and how far it can be trusted |

---

## 1. The pipeline

Contacts stream in from Havok; cues stream out to a renderer. Six stages, one object
(`core/src/Engine.cpp`), driven by a clock the caller owns — the game drives it from the frame hook,
the testbench from a virtual clock as fast as it likes. Same object, same order, same output, which
is the only reason tuning offline against a recording means anything about the game.

| Stage | Job | Where |
|---|---|---|
| **0 Ingest** | Gate on ragdoll state, collapse duplicate reports, reject solver blow-ups on the arithmetic, tag with limb site and surface, route self-contacts to the foley bed. Also folds in pose | `Impl::Ingest` |
| **1 Crash state** | A running per-actor summary, a few dozen floats. The only state the strategies see | `Impl::UpdateState`, `CrashState` |
| **2 Motion + Moment** | Two axes: what the body is doing, and what the mix is doing. Together they set what may be audible and how much budget it gets | `AdvanceMotion`, `AdvanceMoment`, `BudgetFor` |
| **3 Strategies** | The pluggable layer. Six of them. They **propose only** | `IStrategy`, `RunStrategies` |
| **4 Arbitration** | Fixed rules, in order. It **disposes** | `Impl::Arbitrate` |
| **5 Render** | Cue list → voices, through `ICueSink` | `Impl::Emit` |

Everything except stage 3 is fixed infrastructure. That is deliberate, and it is what keeps the
strategy layer from becoming a plugin framework.

### The two seams

**Between 4 and 5** is the one that matters most: stage 4's output is an abstract cue list, and the
game renders it through Skyrim's audio while the testbench renders it through miniaudio, both
implementing the same limited feature set. Anything the testbench can do that the game cannot is a
trap — it means tuning something that will not reproduce.

**Between the game and stage 0** is `Feed.h`. Everything the game knows arrives as plain data
through `IFeed`, which is what lets a recorded take and a live session be indistinguishable to the
engine.

### The portability rule

`core/` may not include `RE/` or `SKSE/`. The engine is compiled twice — once into
`RagdollSounds.dll` inside the game process, once into the testbench with no game running — so
anything the game knows and the testbench does not has to arrive through `Feed.h` as plain data.
A build-time grep enforces it (`core/cmake/CheckPortable.cmake`), and it runs on **every** build.

---

## 2. What the engine measures

Two inputs: collisions, and pose.

Collisions alone are not enough, because **they are dense exactly when a fall is busy and absent
exactly when it is not** — so anything inferred from the gaps between them is backwards.

The plugin publishes every ragdoll limb's position and velocity once per tick
(`GameFeed::PushLimbSamples`), as `kLimbSample` events through the same ring the contacts use. The
engine folds them into a **mass-weighted body centre** — our nominal mass table (`NominalMass`),
never the solver's, which is asymmetric enough that the right arm would drag the centre sideways
(07 §6).

| Quantity | How it is measured | Field |
|---|---|---|
| Body speed | The measured centre's speed | `CrashState::bodySpeed` |
| Airborne | Downward acceleration past a fraction of gravity, with hysteresis at both ends | `CrashState::airborne` |
| Air time | Duration of the measured flight | `AirTimeMs()`, from `airborneSinceMs` |
| Fall size | How far the centre came down since it left support | `CrashState::fallDropUnits` |

`ConsumePose` does the folding, once per actor per tick, before anything reads the result.

### Why acceleration, and not height

Height needs a ground to be above, and the only ground available is the last floor contact — which
is stale the moment a body starts travelling, and on a staircase is a step the body left three
bounces ago. Acceleration needs no reference at all: **a body nothing is supporting falls at
gravity**, on the flat, on stairs and off a cliff alike.

Measured in the capture set, differencing vertical velocity across the quiet gaps:

```
Vayne_impacts_log_2_cut_4, the opening scuff   (442 → 696 ms)    +229 u/s²   pushed UP
Vayne_impacts_log_2_cut_4, the real fall      (1036 → 1633 ms)   −675 u/s²   free fall
```

Free fall in game units is 9.8 m/s² × 69.99 u/m = **−686 u/s²**. The real fall measures −675, within
1.6 %. The opening scuff is the opposite sign entirely — a body being shoved *upward* — which is why
"nothing has touched recently" is not a usable definition of flight.

Fall *size* is measured relative, as the drop from where the body left support, so it survives a
staircase in the same way.

`Motion:fFreeFallMinMs` and `fFreeFallHoldMs` give the flag hysteresis at both ends. The hold is
load-bearing beyond noise rejection: it is what keeps the flight readable on the frame the body
*lands*, which is the frame the hero rule's arrival clause needs it.

### The fallback, and what it costs

A take with no pose sidecar still replays. `haveBodySamples` is false, `bodySpeed` decays
exponentially from the last contact instead of being measured, and `airborne` falls back to "nothing
has touched for `fAirborneMinTimeMs`".

**That fallback is wrong in a specific direction, and rules must be written knowing it.** A body
lying still on the floor has touched nothing recently, so it reads as *maximally airborne*, and so
does the very first contact of a take. Any rule that would draw a conclusion from flight therefore
gates on `haveBodySamples` and simply switches off without pose, rather than acting on a guess. Two
do today: the hero test's arrival clause, and the `Tumble → Airborne` edge.

### The two rows that are not pose

The older captures carry a `limb_sample` per limb at `ragdoll_start` and `ragdoll_end` — two
snapshots seconds apart, a launch pose rather than a signal. `rds::pose::IsTickSample` tells them
from real per-tick pose on the state text, and both the writer and Ingest use it. Treating them as
pose is worse than having none: two frames set the "we have measurements" flag, which switches off
every fallback the take actually needs, and then differences a velocity across a gap that is not a
frame.

### Where pose lives on disk

In `<stem>_pose.bin` beside the take, found by stem exactly like `<stem>.yaml`, `<stem>_sync.csv`
and `<stem>.mp4`. Not in the impacts CSV: pose is eighteen limbs at sixty hertz, and putting it
there would cost that file the one property it has, which is being readable in a spreadsheet and
diffable in git.

It decodes back into the same `kLimbSample` events the live path pushes, so by the time the engine
sees anything the two are indistinguishable. That also means a cut take carries its pose for free —
`SliceTake` works off the decoded event stream and delegates to `WriteTake`.

---

## 3. Stage 2: two axes

```
MOTION   what the body is doing        physics owns it, transitions freely
    Launch ⇄ Airborne ⇄ Tumble ⇄ Slide ⇄ Resting

MOMENT   what the mix is doing         design owns it, latched and windowed
    Ordinary ──hero evidence──▶ Hero (≈220 ms) ──window expires──▶ Ordinary
```

This is the design's own rule made structural: physics owns *when, how loud, where, how long*;
design owns *how many, which one, what it sounds like* (00-Design §3). They are two questions, so
they are two values. One enum cannot answer both — a single state that means both "the body is
tumbling" and "the mix should be loud" has to choose, and whichever it chooses is wrong for the
other reader.

### 3.1 Motion

`AdvanceMotion` in `Engine.cpp`. Every edge is available from every state that can reach it.

| State | Leaves when |
|---|---|
| `Launch` | a contact arrives (→ `Tumble`), or the body is measurably airborne (→ `Airborne`) |
| `Airborne` | a contact arrives (→ `Tumble`), the fall is spent (→ `Resting`), or flight ends untouched (→ `Tumble`) |
| `Tumble` | spent (→ `Resting`), airborne **and** pose is available (→ `Airborne`), sustained tangential motion (→ `Slide`) |
| `Slide` | three exits and only three — see below |
| `Resting` | a contact clears the wake bar, after a minimum dwell |

"Spent" is three conditions, not two: `energyRecent` below `fSettleEnergyFloor`, **the body itself
below it**, and quiet for `fSettleQuietMs`. Dropping the middle one closes the event during the
airborne gap between two bounces, and the closing cue then lands while the body is still in the air.

`Resting` means "no recent contacts". It carries the quiet budget, because the last twenty contacts
of a knockdown are limbs flopping and keeping them nearly silent is the main lever the design has
for staying unobtrusive (00-Design §4).

**What wakes it is a contact over a bar, not any contact.** The bar is
`max(fSettleEnergyFloor * 2, peakSpeed * fRestingExitPeakFrac)`, so it scales with how hard the fall
was and means the same thing in a shove and in a ten-metre drop. Free re-entry hands the quiet tail
back to masking alone; no exit at all makes `Resting` a trap that swallows the loudest contacts of a
fall. The bar is the middle.

`Resting` also holds a **minimum dwell** of `fGetUpBlendMs`, measured from entering it. Not from the
`ragdoll_end` row: that row releases the actor outright — runtime, crash state and all — so by then
there is nothing left to measure against.

#### The slide

**Entry is measured from the contacts and exit is measured from the body**, and that split is the
whole shape of the rule. A graze — sideways motion *instead of* a hit — is a good signal that a
slide has **started** and a bad signal that one is still going, for the reason §2 opens with:
collisions are dense exactly when a fall is busy and absent exactly when it is not, so a slide whose
end is inferred from them ends every time the solver takes a breath.

A slide opens on a run of world grazes that is fast enough (`fSlideMinTangentSpeed`, against a
peak-hold that decays on the grace window) and either long enough (`fSlideMinDurationMs`) or far
enough (`fSlideMinDistance`, measured off the centre of mass where the take carries pose). A run
with a hole in it longer than `fSlideGraceMs` is two runs.

It leaves three ways, and the order is the order of certainty:

| Exit | Test | What it costs |
|---|---|---|
| `kRested` | `spent`, the ordinary Resting edge | the loop fades over `fStopFadeMs`; the closing cue does the rest |
| `kLaunched` | `airborne` **and** pose | the loop fades over `ScrapeLoop:fLaunchFadeMs` instead — a slide that ends in flight ends faster than one that ends in friction |
| `kStruck` | neither of those, and the grazing stopped | a **slide-end impact**, and a hero moment with it if the body was still fast |

`kStruck` is inferred rather than measured, and that is the point: a slide that stops for no reason
the body can account for was stopped by something. The collision that does it is regularly missing
from the contact stream — the limb catches on a doorframe, reports one glancing row, and the body is
simply stopped — so `PlaceSlideImpact` synthesises a `Contact` at the speed the body was travelling
when the grazing stopped, on the limb that was demonstrably grinding, against the surface it was
grinding on. It goes into this tick's contact list in its sorted place, and the composite, the
crunch gate, arbitration and the trace all pick it up without knowing it is unusual. Its sequence
number comes from `nextSlideSeq`, a range well above anything a feed hands out, so it can never
collide with a real row in the variant shuffle or the accepted set.

**Continuation is a different question from entry, and asking it with the entry test is a gate with
no hysteresis in it.** The tangent hold decays on the grace window, so re-applying the entry speed
every tick ended slides on the decay curve rather than on anything the body did: one would open at
200 u/s of tangent and close seventy milliseconds later because the hold had fallen to 119, with the
body still moving and still grinding. All that is asked to *stay* in `Slide` is whether the body is
grazing at all.

The exit is left on `CrashState::slideExit` after the fact rather than being a transient, because
two readers want it a tick later: `ScrapeLoopStrategy` chooses its fade from it, and the testbench's
slide lane colours the end of a span it has already drawn.

### 3.2 Moment — the hero test

`AdvanceMoment` in `Engine.cpp`. Measured on raw `impactSpeed` throughout, never on intensity —
intensity clamps at 1.0, so above `speedRefHigh` every contact reads the same and a test that has to
find the *biggest* moment of a fall would be blind at the top of its own range. Closing speed is not
clamped.

A contact is hero evidence when it clears an absolute floor (`Hero:fFloorFrac` × `speedRefHigh`)
**and** either:

- **dominance** — it is `Hero:fDominanceRatio` (1.3) times the decaying recent peak. The envelope is
  `energyRecent`, a peak-hold over closing speed in units/s with a 300 ms constant.
- **arrival** — it lands out of a real measured flight of at least `Hero:fArrivalMinAirMs`, having
  dropped `fArrivalMinDropUnits`. Gated on `haveBodySamples`; on a take with no pose it is switched
  off rather than guessed.

There is a third way in, and it does not run through `AdvanceMoment` at all: a **slide stopped by
something** while the body was still travelling faster than `Hero:fSlideEndFrac` × `speedRefHigh`
anchors directly, in `PlaceSlideImpact`. That is not a shortcut around the test — it is there
because the slide-end impact is the one contact in the mod the dominance clause cannot judge fairly.
A slide is a long stretch of grazes, so `energyRecent` is *low* when one ends, which would make a
gentle stop dominant; and the contact is measured on the body rather than on a limb, so it is not on
the same scale as the peak it would be compared against. How fast the body was actually going when
it was stopped is the honest test, and it is one a listener can check against what they saw.

Two things about the dominance clause are not obvious and both are load-bearing:

**The envelope is read from before this tick's contacts.** `energyRecent` takes the maximum with
every contact of the frame during Stage 1, which runs before Stage 2 — so a contact read against the
live value would be compared against itself and could never be 1.3× it. `energyRecentBeforeTick` is
the snapshot, taken beside `worldContactBeforeTickMs`, which exists for exactly the same reason.

**And it is floored at the hero floor.** The envelope decays with a 300 ms constant, so a few hundred
milliseconds of quiet take it to nothing — and against nothing, *everything* is dominant. The floor
says the sensible thing: when nothing has happened recently there is no peak to stand out from, so
the contact has to be hard in its own right.

**The floor is limb-blind, except for one relief.** `HeadImpact:bHeroFloorRelief` lets a head
contact over `fHeroFloorReliefAtFrac` be judged against a floor `fHeroFloorReliefFrac` lower — both
fractions of the loud anchor, so they subtract and a 0.30 floor with 0.10 of relief reads as a head
floor of 0.20. It exists because a faceplant is the one contact the blind floor gets wrong: the
strike a fall is *about* can sit under it, or clear it and then fail the dominance ratio.

Two things about it are worth knowing before reading a sweep. It moves the dominance clause as well
as the absolute gate, because `recent` is clamped at the floor — which is what actually lets a head
*anchor* rather than merely pass the first test. And with the trigger defaulted to `0.31`, just
above `Hero:fFloorFrac`, only that second half can ever fire: a head hard enough to trigger it has
already cleared the ordinary floor. Setting the trigger **below** `fFloorFrac` is what opens the
first half. `EngineStats::heroHeadRelief` counts the moments that would not have happened without
it, which is the only way to tell a trigger set out of reach from one doing the work.

A fall may reach `Hero` late, more than once (`Hero:iMaxPerEvent` is 0, unlimited), or **never** — a
gentle slump crosses nothing, and that is a legitimate outcome rather than a missing feature.

**Re-anchoring.** A contact at `Hero:fReanchorRatio` of the open window's own anchor speed takes the
moment over: the window restarts, the burst budget resets again, and the spatial collapse point
moves onto the new contact. This is what makes a landing read as one event with peers rather than
three separate events, and it is why the moment tracks its own peak rather than its first grain.
`AnchorHero` does both jobs, because opening and re-anchoring do the same three things.

### 3.3 Budget

```cpp
const PhaseBudget& BudgetFor(const CrashState& state) const;
```

Motion owns the trim and the grain count; the hero latch overrides both while it is open. **This one
function is the entire coupling between the two axes** — "design's answer wins over physics' answer"
is a statement about precedence, and this is where the precedence lives. Keep it that way: a second
place that mixes the two axes is a second place they can disagree.

---

## 4. Stage 3: strategies

Six, in `Engine.cpp`, run in a fixed order. They propose; they never dispose, and no strategy
modifies another's cues.

| Strategy | Job |
|---|---|
| `ScrapeLoop` | The voicing of `Motion::kSlide`: level and pitch off the measured body speed. **The only strategy that claims** |
| `HeadImpact` | Head contacts get their own layer and their own gate, and above two thresholds their own crunch and gore |
| `ImpactComposite` | The core. The timed layer stack, or a `limb_tap` when intensity is under the threshold |
| `CrunchGore` | The gnarly gate for everything that is not a head, probability-ramped. Ride-along only |
| `MotionFoley` | The continuous bed, and the airborne rise |
| `SettleClose` | The closing cue |

Three ways a proposal relates to the frame:

- **an onset** — counts against the rate cap, the chain merge and the burst shape. The default.
- **a ride-along** (`Proposal::rideAlong`) — an accessory to an onset, judged in a second pass and
  emitted only if its parent's `sourceSeq` was accepted. A crunch with no impact under it is not a
  sound anybody can place.
- **a bypass** (`Proposal::bypass`) — not an onset at all. The two loops and the closing cue.

**Claiming** is the one cross-strategy mechanism there is: returning `true` from `Propose` stops
later strategies seeing the contact. Only `ScrapeLoop` does it, and only for grazes.

### Two rules for the same crunch

`HeadImpact` and `CrunchGore` can both put a `crunch_gran` on a contact, and they are shaped
differently on purpose. The body's is a **probability** gate, softened so an ordinary knockdown
cracks sometimes and a real fall always does — deliberately vague, because nobody can check whether a
given tumble should have broken something. The head's is **deterministic and ramped on level**: the
harder a skull lands the worse it is, every time, and that *is* checkable.

Where they meet the head's wins. `CrunchGoreStrategy` re-derives `ClassifyHeadDamage` and yields on
any contact it fires for, rather than being told — the same pattern `ClassifyHead` already uses in two
strategies, and what keeps neither dependent on the other's running order. They share
`CrunchGore:iMaxCrunchesPerEvent`, so one knockdown cannot become a bag of breaking sticks by coming
in through two doors.

**Both of the head's thresholds are reachable on real data, and that took measuring.** The corpus
holds 323 head contacts with a closing speed; they top out at **732 u/s**, 0.76 of the loud anchor.
A gore tier pitched anywhere near `CrunchGore:fGoreGateFrac` (1.46, the obliterate tier) can never
fire on a head, so the head's own defaults are anchored on that distribution instead — 0.45 for the
crunch, 0.65 for the gore, 0.80 for the top of the gore ramp.

---

## 5. The modifier pipeline

Rules that mutate a contact between ingest and arbitration are a declared list, in four stages with
a contract each:

| Stage | May touch | Occupants |
|---|---|---|
| **Admit** | Veto, speed floor | nothing today |
| **Shape** | `intensity`, `onsetGainDb` — bounded, never `rawIntensity` | glancing, head air time, body air time |
| **Budget** | Burst gap, rate-cap bypass, burst reset — **never level** | the hero moment |
| **Trim** | Loudness only, after arbitration | post-intensity, role and file trims, motion/hero trim, compressor |

Two functions carry it, and every rule goes through one of them:

```cpp
void Shape(Contact&, const ShapeLift&, float weight, float dynamicRangeDb);
void Grant(Proposal&, const BudgetWaiver&);
```

`ShapeLift` and `BudgetWaiver` are declared in `Config.h`, and a config struct that wants to be a
modifier is written as those pieces — `AirTimeConfig`'s two halves are each a `ShapeLift`, and the
hero's budget is a `BudgetWaiver` assembled on the spot. The Admit stage has no occupant at all right
now: the one rule that scaled the ingest floor was the slide sensitivity, and it is gone.

### The two invariants

- **Anything that changes rank is bounded, and never touches `rawIntensity`.** A level added before
  arbitration is also a *rank*, so an unbounded one does not merely make a contact loud — it makes it
  outrank everything in the frame. And Stage 2 must read the untouched figure, or a lifting rule can
  walk the actor into a different motion state and quieten everything after it.
- **Anything at Trim cannot change what was chosen.** By the time it runs, the slot, the layer
  balance and the pitch are all decided; loudness is the only thing left, which is what makes it safe
  to turn while listening.

`Shape` enforces the first by construction: it carries the onset gain with the intensity delta rather
than recomputing it (so a later rule cannot throw away what an earlier one charged), caps the lift
against `maxLevelDb`, and leaves cuts alone. Every Shape-stage rule's post-arbitration half
accumulates into one `Contact::modTrimDb`, which becomes `Proposal::postTrimDb` — **`Emit` must not
grow a term each time a rule learns to trim.**

`Grant` enforces the second by having no level to give. Its combining rules: the burst gap takes the
smaller scale — the more generous of two asks, said the other way round — and the two flags are
sticky.

`BudgetWaiver` is three fields, not six. `grainBonus`, `rateCapFrac` and `maskWidenDb` went with the
slide rule that was their only writer; the arbitrator's grain cap, rate cap and masking drop are
fixed rules again, with no per-proposal scale on any of them. A rule that needs one back adds the
field, the `Grant` line and the arbitration term together — which is less work than reading three
dead fields and guessing whether anything writes them.

---

## 6. Stage 4: arbitration

Fixed rules, in order, over proposals sorted loudest-first with ties broken by `sourceSeq`. See
00-Design §4 for what each is *for*; this is where they live.

1. **Global rate cap** — no two onsets closer than `fRateCapMs`, unless the newcomer is
   `fRateCapOverrideDb` louder than the onset holding it, or carries a waiver.
2. **Chain merge** — contacts on one limb chain inside `fChainMergeWindowMs` collapse to the
   strongest.
3. **Temporal masking** — anything more than `fMaskDropBelowDb` under the decaying ceiling is dropped
   entirely, not played quietly.
4. **Burst shaping** — grains per burst from `BudgetFor`, then `fBurstMinGapMs` of near-silence.
5. **Spatial collapse** — while a hero window is open, every layer is placed at one point.

**The provisional commit.** Everything a proposal changes about actor state — burst, onset, chain,
mask ceiling, admitted count, the hero burst reset — is snapshotted into a local `before` struct,
applied, and **rolled back if `Emit` returns 0**. A stack whose every layer fell under the voice
floor is not an audible moment, and it must not spend the burst budget, raise the masking ceiling or
count towards the reduction ratio; all three would be counting silence as an event.

---

## 7. Making a change

### 7.1 Where a feature goes

Ask what kind of question the feature answers, and the stage follows:

| The feature says… | It belongs in | Example |
|---|---|---|
| "this contact is not real / not worth reading" | **Stage 0**, ingest gates | blow-up rejection, the speed floor |
| "the body is doing *X*" | **Stage 2, motion** | a new motion state, a new edge |
| "the mix should treat this as a moment" | **Stage 2, moment** | a third hero clause |
| "this contact should be built bigger or smaller" | **Shape** | air time, glancing landings |
| "this contact should be *heard* when the budget says no" | **Budget** | the hero's own budget |
| "this kind of sound needs its own layers" | **Stage 3**, a strategy | the head accent, the crunch |
| "no two of these should play together" | **Stage 4**, a fixed rule | the rate cap, chain merge |
| "this is too loud" | **Trim** | role trims, slot trims, the compressor |

Two of those lines are the ones people get wrong, and they are worth stating as a rule:

> **How big a contact is *built* and whether it is *heard* are different questions.**
> Shape answers the first, Budget answers the second. A rule that reaches for both is two rules.

> **Physics decides, design disposes.** If the answer is checkable against what the player sees — a
> hard landing must be loud, a slide must last as long as the slide — it is physics and belongs on
> the motion axis or in Shape. If nobody can check it and it is only a matter of taste, it is design
> and belongs on the moment axis, in a strategy, or in Trim.

### 7.2 Who owns what

| Owner | Owns | Does not own |
|---|---|---|
| `core/` | The whole algorithm, stages 0–5. Portable, no `RE/` or `SKSE/` | Anything about Skyrim |
| `plugin/` | Getting data in and voices out: hooks, the contact ring, the renderer | What any of it should sound like |
| `testbench/` | Listening, tuning, capture. Throwaway | Any behaviour the game cannot reproduce |
| `Config.h` | Every default, with the reason beside it | Key names |
| `ConfigSchema.cpp` | Every key name, range and tooltip | Defaults — those are read out of `Config.h` |
| Motion axis | When, how long, where the body is | How loud the mix is |
| Moment axis | How much budget the mix gets | Where the body is |
| Strategies | What a sound is *made of* | Whether it plays |
| Arbitration | Whether it plays | What it is made of |

### 7.3 Adding a config parameter

Two files, in lockstep, and nothing else:

1. `core/include/rds/Config.h` — the field, with its default and a comment saying what it changes
   *perceptually*.
2. `core/src/ConfigSchema.cpp` — one `RDS_PARAM` / `RDS_PAIRS` row, placed in file order, because
   that order **is** the ini's key order and the testbench's slider order. `RDS_HRULE` and
   `RDS_HPAIR` are the same two rows with a rule drawn above them, for the row that opens a feature
   of its own inside a group that holds several — the second `bEnabled` in a drawer, or a block that
   answers a different question from the one above it. All four names are nine characters wide on
   purpose: marking a row never moves its continuation lines.

Everything else follows: the ini reader, the writer, the clamping, the "what did the user change"
log line and the whole slider panel are all walks over that one table. The default is read out of a
value-initialised config (`ROOT{}.MEMBER`), so it is never written twice and the two cannot drift.

- **Moving or renaming a key** costs one wrapper: `Renamed(RDS_PARAM(...), "OldSection", "sOldKey")`.
  The reader accepts the old name and the next save writes it out under the new one, so no user's
  tuning breaks and the file migrates itself.
- **Removing a key** is safe on its own — an unrecognised line is logged at debug and left alone.
- **The shipping ini is generated**, by `rds-verify --write-config <dir>`, into
  `deployment_files/main/`. Regenerate it after any schema change or the pack ships a stale one.
- Config structs must stay **standard-layout and trivially copyable**: the schema addresses fields by
  `offsetof`, and the testbench memcpy-swaps whole configs between audio callbacks.

### 7.4 Pitfalls

Every one of these has been hit for real.

**Reading a running total that already includes this frame.** `energyRecent`, `energyAccum` and the
world-contact stamps are all folded in during Stage 1, before Stage 2 and Stage 3 read them. If a
rule compares *this* contact against a recent-history value, it needs the `…BeforeTick` snapshot or
it is comparing the contact with itself. Two such snapshots exist already; add the third beside them
rather than inventing a new mechanism.

**Comparing against a decayed envelope with no floor.** Anything that decays reaches zero, and
against zero every test passes. Floor the reference against something absolute.

**Granting a per-contact waiver for a per-event decision.** A landing is five limbs arriving in one
frame carrying identical evidence. A waiver granted per proposal is granted five times, and one burst
becomes five. Decide once — on the actor, or in the arbitrator where proposals are ordered and the
count is authoritative — and let every proposal read the same decision.

**Acting on `airborne` without checking `haveBodySamples`.** Without pose it means "nothing has
touched recently", which is what a corpse lying still looks like. An ungated edge will fly it for the
rest of the take.

**Touching `rawIntensity`.** It is written exactly once, in ingest, and read by Stage 2 and the
leading-limb tally. A rule that lifts it changes how big the *fall* looks, not how big the contact is,
and walks the actor into a different state.

**Adding an uncapped level before arbitration.** It is a rank, not just a loudness. Use `Shape`,
which caps by construction.

**Letting a flag move a parameter across the arbitration line.** If a proposal can be either an onset
or an accessory depending on config, its level must be assembled the same way in both cases —
otherwise a voicing trim silently becomes a priority. See `config.md`; the real fix is the planned
`Proposal::priorityDb`, and until it lands the rule is *when in doubt, `Trim`*.

**Adding actor state to arbitration without adding it to the rollback.** Anything a proposal mutates
before `Emit` must be in the `before` snapshot, or a silent stack leaves it changed.

**Measuring a window in frames.** Every window is `Window(floorMs, frameMs, k)` = seconds with a
floor, so the engine behaves the same at 24 fps and 144. A frame count does not.

**Asking a strategy to decide something the motion axis owns.** `ScrapeLoop` kept its own duration,
distance and speed gates alongside `[Motion]`'s slide keys, named differently and defaulted
differently, and the two could and did disagree — a knockdown could sit in `Slide` with no loop under
it, or grind away in `Tumble`. When a state and a sound are the same event they are one decision,
and the state owns it.

**Standing in for a measurement the engine now has.** The slide's level rode a distance ramp, its
`fPitchPerThousandUnits` was named for a speed nothing had measured — the tangent of whichever graze
had been fastest since the slide opened — and its end was inferred from the impacts stopping. All
three were written before pose existed. When a measurement arrives, the things that were
compensating for its absence are not features to keep beside it.

**A running maximum used as a live signal.** It never comes down. One fast skim held the slide's
entry test open for the rest of a knockdown and pinned the loop's level and pitch at the loudest
tangent the fall ever saw. Anything read every tick is a peak-hold with a decay, not a max.

**Assuming ingest can see this tick's state.** Ingest runs before Stage 2, so anything it reads about
motion is *last* tick's. Nothing in ingest reads it today — the slide floor was the one rule that
did — so do not build a rule that needs it to be current.

**Adding a voice cap.** There is deliberately no global one. It was measured: the engine started 288
sounds with 224 alive and never found a ceiling (00-Design §14). What decides how much is heard is
the rate cap, the chain merge, masking and burst shaping — all of which judge the mix. A count judges
nothing.

**Branching on `CueReason`.** It is provenance, for the timeline and the log. There are exactly two
places in the engine that decide anything on it, both deliberate and both on `layers[0]` — the class
compressor's threshold lookup, and the closing cue's in-flight tripwire. A third wants justifying:
the reason a cue exists is not usually the reason it should behave differently, and a rule that
needs to know is usually a rule that should have been given its own field.

### 7.5 Verifying a change

- Engine changes: `ninja core/rds-verify.exe`, from `testbench/build/testbench`. Not a bare `ninja`,
  and never the copied `-bench.exe` — that is a hand-made copy with no CMake target behind it.
- The plugin builds only through `build-skse-mods.ps1`.
- The testbench exe cannot be linked while the GUI is running. Compile the objects and defer the
  link; do not kill the app to get one.
- **Record a baseline before editing.** `rds-verify <recordings> <bank> > baseline.txt`, then diff
  named checks and the per-take funnel counts against it afterwards. A take whose `emittedCues` or
  `bursts` move is expected; a take whose `contactsIn` or `rejectedBelowFloor` move means something
  upstream of the change was disturbed, and that is a bug.
- **The `determinism` check is itself flaky, and the failure total inherits that.** Measured on
  `Vayne_impacts_log_2_cut_2`: four failures in eight consecutive runs of one binary, always
  reporting *30 against 30 cues, first difference at index 28*. Same count, same index — that is not
  the engine disagreeing with itself about what to play, it is `std::memcmp` over a `Cue` that has
  padding holes. A vector reallocation copies element-by-element and leaves padding indeterminate,
  so the bytes between the fields differ while every field agrees. **The fix is to compare field by
  field** (`core/src/Offline.cpp`); until then, a `determinism` failure with matching cue counts is
  noise, and the total moves with it. Read the named rows and the funnel counts, and re-run before
  believing a one- or two-check swing.
- Do not run two `rds-verify` processes concurrently. They race on a shared temp file and the config
  round-trip check reports `hand edit read back NO`.
- A **pure refactor should produce byte-identical output.** If it does not, it was not a refactor.
- Two rows are absolutes rather than calibration, and the `hero moments` check enforces both:
  `settle in flight` must read **0** on every take, and a knockdown carries 0–3 hero moments.

The remaining `rds-verify` failures are calibration disagreements, not crashes — `TODO.md` lists them
under "Calibration decisions — need ears, not code". Two are worth knowing before reading a score:
`hero cliff` asserts a ≥ 9 dB gap among played events, which temporal masking makes impossible by
construction, and the `reduction ratio` / `audible moments` pair measure the same rhythm two ways and
disagree about which is the assertion.

---

## 8. Status

| Part | State |
|---|---|
| Pose published per tick, written to `_pose.bin`, decoded on load | **done** |
| Old takes replay with a `[no pose]` warning | **done** |
| Measured `bodySpeed`, `airborne`, air time, `fallDropUnits` | **done** |
| Two-axis motion/moment machine | **done** |
| The hero test | **done** — dominance live, arrival needs pose |
| The declared modifier pipeline | **done** |
| Per-episode stats (`heroes`, `heroReanchors`, `settleInFlight`, `slides`) | **done** |
| The slide: entry on contacts, three measured exits, level off body speed | **done** |
| `Proposal::priorityDb` — the level/rank split `config.md` waits on | **not started** |
| Heard in game | **no** |

### The corpus carries pose now, and that is what the slide rework is built on

`Research/NewRecordings` holds six takes with `_pose.bin` sidecars beside them — the devbench
captures. Everything in §2 that says "measured" is measured on these, including the two clauses that
were untested code when the sidecar did not exist yet: the hero test's arrival clause and the
`Tumble → Airborne` edge.

The slide is the part that most depends on it. Every number the loop and the three exits read —
body speed for the level and the pitch, centre-of-mass travel for the distance clause, the free-fall
flag for the launch exit — comes out of the sidecar, and on a take without one the whole feature
falls back rather than guessing: `SlideSpeed` returns the held tangent instead of the body's speed,
and the launch exit is gated on `haveBodySamples` like every other rule that reads flight.

**A 10 m fall is now the highest-value capture outstanding**, along with a natural-ground surface
and one unparalysed take for the get-up window (00-Design §15).
