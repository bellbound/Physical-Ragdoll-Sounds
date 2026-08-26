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
| **3 Strategies** | The pluggable layer. Five of them. They **propose only** | `IStrategy`, `RunStrategies` |
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

### The two volume sliders

The mod ships `RagdollSounds.esp` — light-flagged, so it costs no plugin slot — and the only thing
in it is two sound category records:

| Slider | EDID | Local id | Parent | Governs |
|---|---|---|---|---|
| **Ragdoll Sounds** | `RDS_AudioCategoryRagdoll` | `0x800` | `_AudioCategoryMaster` | every voice the mod opens |
| **Ragdoll Gore** | `RDS_AudioCategoryGore` | `0x801` | `RDS_AudioCategoryRagdoll` | the crunch and gore layers |

A slider on Skyrim's Audio settings page *is* a sound category with the ShouldAppearOnMenu flag —
the engine builds that page by walking the load order's `SNCT` forms — so two records is the whole
mechanism, and `tools/make_categories_esp.py` writes them. A category's level multiplies its
parent's, so the gore slider trims within whatever the ragdoll slider has set and pulling either to
zero silences what it governs. Nothing at runtime writes a category volume: `GetCategoryVolume` does
not read back what `SetCategoryVolume` wrote, which SkyrimNet established the hard way
(`skse/SkyrimNet/docs/Engine Audio RE Notes/RE-FINDINGS.md` §4b), so the level is authored into the
record and the player's slider drives it.

#### Why the ragdoll slider ships at 0.6 and the mix does not compensate

The ragdoll record's UNAM is **0.6**, so the mod ships 4.4 dB under its tuned calibration and the
slider has somewhere to go in both directions. Nothing in the mix buys that back, and this is the
part worth remembering: **it cannot be bought back.**

Every multiplier in the engine's category chain caps at unity — VNAM, UNAM and the player's slider
are all 0–1, and the parent chain only attenuates. The buffer handed to the engine is therefore the
loudest this mod can ever be, and every slider position is a cut from it. Room to turn it *up* can
only be paid for with default level.

Paying for it in the mix instead was measured and rejected. `rds-verify --headroom` mixes every
composite in the corpus twice, once as it ships and once with a make-up gain added to every cue —
which is exactly what raising `fMasterGainDb` does, master being a term in `cue.gainDb` that nothing
upstream reads. Over **1820 composites** at the shipping defaults:

| post-clip peak | median | p90 | p99 | max |
|---|---|---|---|---|
| | 0.012 | 0.161 | 0.809 | 0.901 |

The top percent is already hard against `MixParams::clipCeiling` — about 3.8 dB inside the soft clip
before anything is added. So a make-up gain does not raise the mod, it *compresses* it:

| make-up | slider it pays for | worst delivered | p90 loss | composites losing > 0.5 dB |
|---|---|---|---|---|
| +2.00 dB | 0.79 | +0.41 dB | 0.05 dB | 54 (3.0%) |
| +4.44 dB | 0.60 | +0.65 dB | 0.14 dB | 112 (6.2%) |
| +6.02 dB | 0.50 | +0.70 dB | 0.23 dB | 132 (7.3%) |

The quiet 90% take the gain cleanly and the hero hits take almost none of it, which flattens exactly
the dynamic contrast the hero-cliff check exists to protect. A lower default with the mix untouched
keeps every internal relationship intact and is one slider move from the old level.

The gore record stays at UNAM 1.0. Its job is to take gore *out*; a default under unity would ship
the crunch and gore layers below the balance they were tuned at, which is not a level change anyone
asked for. It inherits the ragdoll slider's upward room through the parent link.

VNAM is 1.0 on both, for the reason UNAM is not: a static multiplier is a second trim in a file
nobody would think to look in, and the default belongs in the part the player can see and move.

One further level change is unavoidable and is `$Master`'s own VNAM of 0.90, about **0.9 dB**, which
our voices did not pay while they had no category at all.

Two consequences for stage 5. The category is a property of a **voice**, so the crunch and gore
layers need a voice of their own — `GameRenderer::StartBus` splits a group by bus and mixes each
half **against the group's own earliest time**, so the +20 ms a crunch sits behind its transient
survives as leading silence rather than being lost to whichever frame each voice opened on. And a
missing or unticked esp is survivable: `CategoryFor` returns null, the sound plays uncategorised at
the level we mixed it at, and all that is lost is the sliders.

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
    Launch ⇄ Airborne ⇄ Tumble ⇄ Slide

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
| `Airborne` | a contact arrives, or the flight ends untouched (both → `Tumble`) |
| `Tumble` | airborne **and** pose is available (→ `Airborne`), sustained tangential motion (→ `Slide`) |
| `Slide` | two exits and only two — see below |

**No motion state suppresses a contact**, and none may be given the job. Everything that keeps a
fall unobtrusive already judges the contact in front of it: intensity puts a settling forearm at 0.03
and the tap branch catches it, temporal masking drops anything 12 dB under the ceiling, the chain
merge collapses a limb chain, and burst shaping caps the grains. A phase cannot judge a contact, so
it does not get to silence one — a state carrying a quiet budget silences by *category*, and is
wrong for every contact the category got wrong.

There is no closing cue for the same reason: it needed a state that meant "this fall is over", which
is an inference. `SlotId::kSettleRest` stays declared with no user, like `grunt_impact` and
`scream_big`, so bringing one back is a strategy rather than an asset change.

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

`fSlideMinTangentSpeed` is an **entry** gate and only that. Nothing re-applies it, because a gate
with no hysteresis in it closes on its own decay curve: the tangent peak-hold falls on the grace
window, so a slide that opened at 200 u/s would shut seventy milliseconds later at 119 with the
body still grinding. The **exit** speed is a separate number asked of a different quantity —
`fSlideHoldSpeed`, against `bodySpeed` off the pose sidecar. Over it the slide outlives its own
grazes; under it `fSlideGraceMs` has the last word as before. Set it below `fSlideMinTangentSpeed`
and the pair is proper hysteresis: real rubbing to start a grind, rather less to keep one.
`fSlideHoldMaxMs` bounds it for a pose stream that says a body is drifting with nothing touching
it. Zero is off, and off is the default.

Measured on `Proventus_Avenicci_devbench_3_cut_2`, whose last slide opens at 5328 ms and whose graze
stream dries up at 5777: off it ends at **5917 ms**, at 80 u/s it ends at **6179**, at 40 u/s at
**6280** — the body was still travelling 40–80 u/s for a third of a second after the collisions
stopped being reported, and the grind stopped with it still moving.

It leaves two ways:

| Exit | Test | What it costs |
|---|---|---|
| `kLaunched` | `airborne` **and** pose | the loop fades over `Slide:fLaunchFadeMs` — a slide that ends in flight ends faster than one that ends in friction |
| `kEnded` | the grazing stopped — and, with `fSlideHoldSpeed` set, the body has slowed under it | the loop fades over `fStopFadeMs` |

**Neither of those fades is what a slide's ending sounds like. The speed ramp is.** The body grind's
level tracks measured body speed from `fSpeedForMaxGain` down to `fSpeedForMinGain` across
`fSpeedRangeDb`, and the bottom of that ramp is also where the grind stops — so the two numbers
have to be set together, such that `fGainDb + fSpeedRangeDb` lands on `Mix:fVoiceFloorDb`. At the
shipped -16 and -32 it lands on -48, the floor: a body slowing to a halt fades itself to silence on
its own measured speed and arrives at nothing exactly as it arrives at the bottom, so the stop has
nothing left to cut.

That correspondence is easy to break and worth stating plainly, because it used to be broken. With
`fGainDb` at -20 and `fSpeedRangeDb` at -12 the ramp bottomed out at -32 dB, 16 dB clear of the
floor, and the grind was switched off partway down it with `fStopFadeMs` left to hide the step —
which is a large part of why a slide read as a noise being turned off rather than a body running
down. `fSpeedForMinGain` was 120 u/s for the same reason: that is a body still moving at a walking
pace, so the ramp was never given room to finish. It is 40 now.

An envelope was tried here and removed. An attack and release on the loop level are a second,
time-based ending competing with the speed ramp for the same job: fixed in duration however the
body actually behaved, wrong on a launch — where it drags a grind behind a body already in the air,
the exact thing `fLaunchFadeMs` exists to prevent — and with no scope that works for both ends,
since an attack belongs to each voice's own arrival while a release belongs to the slide. The
physical quantity was already there; it just was not reaching the floor.

**The slide end does nothing to the collision that ends one, and that is the point of the speed
hold.** There used to be a lift here — `LiftSlideEndContact` found the strongest contact of the
tick and made it bigger, with a hero clause of its own beside it — and it existed because the exit
was inferred from the contact stream drying up. A slide ended while the body was still travelling,
so the contact that "stopped" it had to be found and inflated by hand to sound like the stop it
was.

`fSlideHoldSpeed` measures the body instead. A grind now outlives its own grazes and ends when the
body does, so the collision that ends one arrives into an ordinary quiet stretch and is an ordinary
contact: judged on its own closing speed, by the dominance clause like anything else. The lift, its
four tuning keys, `Hero:fSlideEndFrac` and the `slideImpacts` counter are all gone.

The corpus said so before the removal did. `lifted a contact` read **0 on all eight takes**: no
slide in the recordings ever ended on a frame carrying a real collision, so the rule that was
supposed to describe that moment had never once described anything. A rule standing in for a
measurement is not a feature to keep beside it once the measurement arrives (§7.4).

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

There used to be a third way in that did not run through `AdvanceMoment` at all — a real collision
ending a slide, anchoring directly out of `LiftSlideEndContact` — and it went with the lift it rode.
Its premise was that a slide is a long stretch of grazes, so `energyRecent` is low when one ends and
the dominance clause would call a gentle stop dominant. That is still true of a slide that runs out
of grazes, and a slide no longer ends that way: `fSlideHoldSpeed` holds it open on the body's own
speed, so the stop lands in an ordinary quiet stretch, which is exactly what the dominance clause is
for. See §3.1.

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

Five, in `Engine.cpp`, run in a fixed order. They propose; they never dispose, and no strategy
modifies another's cues.

| Strategy | Job |
|---|---|
| `ScrapeLoop` | The voicing of `Motion::kSlide`: level and pitch off the measured body speed. **The only strategy that claims** |
| `HeadImpact` | Head contacts get their own accent layer and their own gate. The accent only - damage left here |
| `ImpactComposite` | The core. The timed layer stack, or a `limb_tap` when intensity is under the threshold. `BodySlot` picks which of the two body layers carries the mass |
| `Damage` | Crunch and gore for head, spine and limb. One rule, three tunings, six budgets. Ride-along only |
| `MotionFoley` | The continuous bed, and the airborne rise |

Three ways a proposal relates to the frame:

- **an onset** — counts against the rate cap, the chain merge and the burst shape. The default.
- **a ride-along** (`Proposal::rideAlong`) — an accessory to an onset, judged in a second pass and
  emitted only if its parent's `sourceSeq` was accepted. A crunch with no impact under it is not a
  sound anybody can place.
- **a bypass** (`Proposal::bypass`) — not an onset at all. The two loops and the closing cue.

**Claiming** is the one cross-strategy mechanism there is: returning `true` from `Propose` stops
later strategies seeing the contact. Only `ScrapeLoop` does it, and only for grazes.

### The two body layers

`imp_body` is the torso's — and the head's, which has the mass to sound like one and gets
`head_impact` on top to say it was a skull. `imp_body_limb` is the same layer out on an arm or a leg:
drier, tighter, higher, less weight under it. `BodySlot` chooses between them off `DamageSiteFor`,
and nothing else about the composite changes — same ramp, same offset, same rank — because which wav
carries the mass is a question about timbre and not about how big the contact was.

The impact family was the last place the mod did not make a distinction it makes everywhere else: the
loops have `scrape_loop` against `scrape_limb`, the crunches have three tunings, and a faceplant and
a forearm were built from the same file with only the mass term between them.

`imp_body_limb` **falls back to `imp_body` and shares its mute**, so a bank with nothing recorded for
it is byte-identical to one without the slot — measured over the whole corpus, not assumed. It keeps
a trim of its own for the reason the three crunches do: two recordings arrive at two levels. Until
one is recorded, `SlotGain:fImpBodyLimb` trims the `imp_body` file the limb composite falls back to,
which is the useful half of it early — it sits every limb impact back without touching the torso's.

**New slots go at the end of `SlotId`, not where they belong.** A slot's *number* is an input to
`StableHash`, which is what decides which variant of a slot a contact plays, so inserting a value in
the middle renumbers every slot below it and silently re-rolls the variant of every cue in the mod.
That was measured on the first pass of this change: filed tidily beside `imp_body`, it moved
`head_impact` from variant 0 to variant 1 across the corpus and changed nothing else. The whole point
of deriving the draw from the contact is that adding a layer leaves the rest of a take bit-identical,
and a prettily grouped enum gives that up. The manifest table is indexed by the enum and has to match
it, so the row is at the end too, and the character line says where it belongs.

### Correcting one recording

Two fields in a file's own `.meta.ini`, beside `Name` and `Disabled` in the
section that file already labels *"yours"*:

```ini
[Sfx]
Pitch  = 1.0900   ; playback rate. 1 is the file as recorded
TrimDb = -2.50    ; level, at Stage 5, with the other trims
```

They belong to **the sound**, not to the assignment, and `RagdollSounds_SFX.ini`'s
own header is where that distinction is already drawn: a mute "is about the sound,
not about one entry in the list", while a condition "is about one entry in `Sfx`,
not about the sound". A recording that is flat is flat wherever it is used. A
level wanted on one slot and not another is a decision about the slots, and
`SlotGain:f<Slot>` is the control for that.

**The trim cannot change what was chosen, and that is the ordering rather than a
promise.** Nothing knows which *file* a layer resolved to until `Emit` calls
`SoundBank::Resolve`, which is after Stage 4 has sorted, rate-capped, masked and
burst-shaped. There is no path by which a per-file number could reach
`Proposal::levelDb`. `config.md` calls this the strongest form the Trim rule
takes.

**The pitch multiplies rather than replaces.** The per-cue scatter, the intensity
bias and the armour bias are what *this contact* should sound like; the library's
number is what the *recording* got wrong, and the two are different questions.

Both travel on `ResolvedSound`, copied off the `SfxEntry` in `LoadAssigned`, so
the engine never holds the library and `Emit` never looks anything up. Both are
identities at their defaults — the corpus is byte-identical with the feature in.

Three things worth knowing:

- **Pitch is resampling, so it changes how long a one-shot plays.** `lengthMs` on
  the resolved sound already has it in it, because a renderer sizes its mix buffer
  from that and would otherwise cut the tail off every corrected file.
  `SfxEntry::EffectiveDurationMs()` is the number a length spec has to be checked
  against; `durationMs` stays what the container says.
- **The by-name fallback scan carries no corrections.** They live in a library
  file's metadata, and a wav found by the `<slot>_<NN>.wav` convention in the pack
  has none to read. That includes `rds-verify`'s own corpus bank, which is why the
  feature has a check of its own (`sfx adjust`) rather than being covered by a
  replay.
- **A loop resolves on its voice id, not its `sourceSeq`.** A loop traces back to
  no feed event, so its token was 0, which turns the stable picker off and drew a
  fresh variant on every update cue. That was invisible while a variant only named
  a file — the renderer re-follows gain, pitch and position on an update and never
  re-attaches the sound — and stops being invisible the moment a variant carries a
  correction, because then the *correction* flaps between two files' answers on a
  voice playing neither.

**What is deliberately not offered here.** `Cue.h` lists what the game can
actually do: volume, continuous pitch, position, bone attachment, whole-file
looping, fade, raw wav playback — and names per-voice filtering, EQ and reverb
send as unavailable. A testbench EQ on a slot assignment would be tuning something
that cannot reproduce in the game, which is the trap §1's two seams exist to name.
A start offset is the same answer from the other side: `Cue` has no such field, so
a file with dead air in front is *late*, and lateness is load-bearing when the sub
is specified at +55–75 ms. That one is baked by `sfx.py`, not turned at runtime.

### One rule for every crunch

`Damage` puts a crunch and a gore layer on any contact hard enough to earn one, and it is tuned
three times over: `head`, `spine`, `limb` — `DamageSiteFor` maps a `LimbSite` onto one of them, with
the neck counted as spine and anything unrecognised as limb. Each part carries two tiers, each tier
carries a threshold, a cap, a chance at each end, a level at each end, a delay, a budget and a
spacing, and **every tier has its own budget**. The head keeps `crunch_gran`; spine and limb get
`spine_crunch` and `limb_crunch`, which fall back to it until somebody records them. All three share
`gore_wet`.

**It used to be two rules, and the seam is what this replaced.** `HeadImpact` owned a deterministic
crunch ramped on level; `CrunchGore` owned a probability gate for everything else. Both shapes
survive here as *tunings* — a tier whose two chances are 1 is the deterministic rule exactly, one
whose chance at threshold is 0.15 is the old body ramp exactly — so nothing was given up by having
one rule. What went was the second gate, the second level ramp, and the single
`iMaxCrunchesPerEvent` the two of them spent from.

That shared counter is worth naming as a failure, because it was invisible. Whichever rule reached a
contact first spent from it, so switching the body's crunch on could silence the head's — and the
head's is the loud, checkable one. Worse, the budget was charged at *proposal*: a body crunch that
the arbitrator then dropped as a ride-along with no surviving parent still took the slot, and no
counter anywhere moved to record it. The fix is not ordering, it is arithmetic: six ledgers, one per
part per tier.

**The tiers are independent.** Gore is not nested inside crunch. It has its own threshold, its own
budget and its own spacing, so a contact bad enough to be wet still sounds wet on a frame where the
crunch budget is gone. Nesting it made the rarest and most expensive layer in the mod the easiest
one to suppress by accident.

**Every threshold is measured, and it has to be.** A tier pitched above its part's own maximum is not
a rare tier, it is a dead one — the trap the head's gore fell into when it was pitched at the
obliterate frac. The corpus, binned exactly as `DamageSiteFor` bins it:

| part | contacts | p95 | p99 | max | crunch | gore |
|---|---|---|---|---|---|---|
| head | 409 | 0.59 | 0.76 | 1.14 | 0.45 — top 8.3% | 0.65 — top 4.4% |
| spine | 2019 | 0.26 | 0.45 | **0.87** | 0.45 — top 1.0% | 0.72 — top 0.30% |
| limb | 6853 | 0.35 | 0.64 | 1.14 | 0.68 — top 0.83% | 0.95 — top 0.12% |

All as fractions of the loud anchor. The spine's ceiling is the number to keep in view: it tops out
at 0.87, so anything pitched at 0.90 can never fire, and a first pass of these defaults did exactly
that. The ladder across the three is deliberate — a skull landing hard is *supposed* to be the
common one, and limb contacts outnumber heads better than sixteen to one, so a gate that suits a
skull turns an ordinary tumble into a bag of breaking sticks out on the arms.

**Past the obliterate point the limits loosen rather than tighten.** `Intensity:fObliterateFrac` used
to be a second gate ANDed under the body's gore, which made the most extreme contacts the engine can
see the *hardest* ones to hear. It now grants `Damage:iObliterateBudgetBonus` extra slots and scales
the spacing by `Damage:fObliterateSpacingScale` for that contact. A relaxation, not a waiver: a
raised budget is still a budget, so an absurd impulse from another mod cannot machine-gun the layer.

**A tier at 1/1 draws no random number.** That is load-bearing rather than an optimisation. Under the
old rule, enabling the body's crunch consumed RNG and re-rolled every variant and scatter after it,
so two exports differed everywhere instead of where the edit bit. Switching a part's damage on or off
now leaves every other cue in the take bit-identical — `Damage:bEnabled=0` against the default over
the whole corpus moves `proposed` and `emitted` and not one drop counter.

---

## 5. The modifier pipeline

Rules that mutate a contact between ingest and arbitration are a declared list, in four stages with
a contract each:

| Stage | May touch | Occupants |
|---|---|---|
| **Admit** | Veto, speed floor | nothing today |
| **Shape** | `intensity`, `onsetGainDb` — bounded, never `rawIntensity` | glancing, head air time, body air time |
| **Budget** | Burst gap, rate-cap bypass, burst reset — **never level** | the hero moment |
| **Trim** | Loudness only, after arbitration | post-intensity, role and file trims, the library's per-file trim, motion/hero trim, compressor |

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

`Grant` enforces the second by having no level to give. Its combining rules: both fractions take the
smaller scale — the more generous of two asks, said the other way round — and the flag is sticky.
Both fractions mean *how much is waived*, so 0 asks for nothing and 1 waives the rule outright; a
field that pointed the other way would make that comparison silently pick the wrong ask.

`BudgetWaiver` is three fields: `burstGapFrac`, `rateCapFrac` and `resetsBurst`. `grainBonus` and
`maskWidenDb` are not among them, and the arbitrator's grain cap and masking drop are fixed rules
with no per-proposal scale. A rule that needs one adds the field, the `Grant` line and the
arbitration term together — which is less work than reading a dead field and guessing whether
anything writes it.

**Prefer a scale to a switch.** `rateCapFrac` replaced a boolean that turned the rate cap off inside
a hero window, and the difference is the whole point: what turns up in one frame at a landing is not
all alike. Contacts on successive frames land ~18 ms apart and read as a cluster; contacts in the
*same* frame land ~0.1 ms apart and do not read at all — they sum, and the peak doubles for no extra
density. Nothing in the corpus lands between 1 and 17 ms, so a scaled cap in that gap keeps every
cluster and rejects every stack. A boolean cannot express that, because it never looks at the gap.

---

## 6. Stage 4: arbitration

Fixed rules, in order, over proposals sorted by `priorityDb` with ties broken by `sourceSeq`. See
00-Design §4 for what each is *for*; this is where they live.

1. **Global rate cap** — no two onsets closer than `fRateCapMs`, scaled by the proposal's
   `rateCapScale` (`Hero:fRateCapFrac` is the only writer), unless the newcomer is
   `fRateCapOverrideDb` above the onset holding it. An onset admitted inside the *nominal*
   cap because a moment scaled it counts as a `rateCapOverrides`, so the verifier — which measures
   gaps against the nominal cap and cannot see a scale that lived on a discarded proposal — still
   balances.
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

### 6.1 Priority — rank without loudness

`config.md`'s level/rank split, live. `Proposal::priorityDb` is `levelDb` plus the contact site's
`[Arbitration]` weight, assembled in `Arbitrate` and **nowhere else** — a strategy that could write a
priority would be a strategy able to make its own cue outrank the frame, which is the trap that file
was written about.

Four readers, and only four: **the sort, the rate cap's override comparison, the chain merge, and
what is stored into `lastOnsetDb` / `chainLastDb`** for the next proposal to be compared against.
Masking and `maskCeilingDb` deliberately keep reading `levelDb` — "can this be heard under what is
already sounding" is a question about air, and a weight nobody can hear must not raise the ceiling
the next contact has to clear. Both halves of every comparison are weighted; a ledger that mixed the
two scales would let a torso outrank a hand while the hand outranked it back.

| key | what it weights |
|---|---|
| `Arbitration:fTorsoWeightDb` | spine, COM and the neck — the column |
| `Arbitration:fHeadWeightDb` | the skull |
| `Arbitration:fLimbWeightDb` | arms, legs, and anything off a skeleton we could not read |

Binned through `DamageSiteFor`, which is the engine's one site→part mapping — the damage tiers read
it, `BodySlot` reads it, `SiteWeightDb` reads it. Only the *differences* between the three matter, so
raising two and leaving the third at zero says everything raising all three could.

**Every default is 0, and that is not timidity.** At zero the priority is the level exactly, the
arbitrator sorts the way it always did, and the whole corpus is byte-identical — which is what makes
the first non-zero value an A/B with one variable in it. The measurement that says where to start:
over the thirteen devbench takes, **398 torso proposals were dropped with a limb holding the window,
and in 63 of them the limb was quieter on closing speed** — the torso lost on the mass term, on the
glance cut, or on arriving a tick later. Another 67 lost by under 3 dB, and the median margin is
6.2 dB. So 3 dB of torso weight keeps the ~130 where the arm was barely ahead and leaves the ~270
where the arm really was the event.

**A weight is bounded on purpose.** It is not a veto: at 3 dB a limb genuinely 6 dB louder still
wins. "Sometimes the arm *is* the sound" is arithmetic here rather than a second switch.

It carries across ticks as well as within one, and that is the half that matters most. 99.5 % of the
corpus's ten-millisecond contact clusters fall inside a single tick, where the sort already decides
everything; the rest are a contact arriving into a rate cap somebody else is holding. Weighting both
sides of `fRateCapOverrideDb` is what lets a torso landing 20 ms after a hand open its own onset at
3 dB of real level instead of 6 — and makes a hand arriving after a torso need 9.

A bypass keeps its priority at its level. It is not an onset, nothing sorts it against anything, and
a weight on a loop would be a number with no reader.

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
- **Every parameter is reachable from a command line the moment it is declared**, in both halves of
  the loop: `rds-verify --set Section:Key=value` for an offline A/B, and `tools/tune.py set
  Section:Key=value` for the testbench that is running right now — which patches the session, saves
  the result as a new config and selects it. Neither needed a line of UI, for the same reason the
  slider did not.

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
otherwise a voicing trim silently becomes a priority. See `config.md`. `Proposal::priorityDb` (§6.1)
is now the place to put a number that should change rank *deliberately*; a voicing number still
belongs in a `Trim`, and *when in doubt, `Trim`* still holds.

**Inserting a `SlotId` where it reads nicely.** The enum value is an input to the variant hash, so a
slot filed in the middle renumbers everything below it and re-rolls which file every later cue plays,
across the whole mod, for a change that was supposed to add one layer. Append at the end. See §4.

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

**Letting a deadband on one field gate another.** A running loop only re-cues when its level moves
more than `fLevelDeadbandDb`, which is what keeps the cue list readable. The callers then patched the
loop's *anchor* onto the proposal after the fact - so on a tick that produced no proposal, the hop
was thrown away too. The renderer re-attaches a voice when a cue arrives and at no other time, so a
grind that moved onto the other hip at a steady level went on sounding from the hip it left until
something unrelated moved its gain, and `fLimbHoldMs` makes those hops deliberate and rare, which is
exactly the update you cannot afford to drop. The anchor is now decided *before* the deadband and
passed in, and a hop re-cues on its own. Worth 13-25 recovered updates per take in the corpus.

**Leaving a placement to a default index.** A whole-body loop that never set `limbIndex` got 0, and
`NodeFor`'s last resort resolves index 0 to a bone - so the loop hung off whichever rigid body Havok
listed first. On a humanoid ragdoll that is `NPC COM [COM ]` and the accident came out right, which
is why it survived; on a creature or a modded skeleton it is a wing or a tail. `bodyLimb` resolves
the torso off the profile's bone names, the same way every other site is resolved, and falls back to
0 only where no bone reads as a torso.

**A running maximum used as a live signal.** It never comes down. One fast skim held the slide's
entry test open for the rest of a knockdown and pinned the loop's level and pitch at the loudest
tangent the fall ever saw. Anything read every tick is a peak-hold with a decay, not a max.

**Assuming ingest can see this tick's state.** Ingest runs before Stage 2, so anything it reads about
motion is *last* tick's. Nothing in ingest reads it today — the slide floor was the one rule that
did — so do not build a rule that needs it to be current.

**Asking a live clock a question about something that is over.** A landing rule asks "this contact
ended a fall of how long"; `airborne` and `freeFlightSinceMs` answer "the body is unsupported right
now and has been for this long". Those are different questions, and the second is wrong at both ends
of the rule it was standing in for: it pays out on mid-air clips that are not landings, and it reads
exactly **zero** on the arrival, because by the time the arrival is judged the flag has cleared. The
air-time rules — head and body — and the hero test's arrival clause all read it, so all three were
paying for the wrong contact. The flight is latched at the `!airborne && wasAirborne` edge now
(`lastFlightMs`, `lastFlightDropUnits`) and read back for `Motion:fLandingWindowMs`. If a rule is
about an event that has finished, latch the event; do not sample the state that was true during it.

**A detector whose evidence the thing it is detecting also produces.** `driven` asks whether the
acceleration has more in it than gravity, to discount a leash haul from a fall the rules pay for.
A collision produces exactly that signature and produces far more of it — the yank the gate was
measured on reads 900–1600 u/s², and `Proventus_Avenicci_devbench_5`'s arrival at 2919 ms reads
**5110**. So every landing declared itself driven, rewound `freeFlightSinceMs` onto the frame of
impact and zeroed `fallDropUnits` with it, and all six flights in that take reported 20–23 ms and
0 units against a real 61–331 ms and 4–43. Self-hits do it too: the centre is mass-weighted off
`NominalMass` rather than the solver's, so a limb swinging into its own torso does not cancel out of
it the way a true internal impulse would, and that leak alone read 554 u/s² at 6226 ms with nothing
outside the body touching it. The test now skips any tick carrying a contact at all
(`sawContactThisTick`, stamped in ingest above every rule about whether a collision is worth
*hearing*, because this one is not asking that). Before trusting a gate, ask what else makes the
number it reads — and if the answer is "the event I am trying to measure", the gate needs a second
term, not a higher threshold.

**Adding a voice cap.** There is deliberately none — not globally and not per actor. Both were
measured away: the engine started 288 sounds with 224 alive and never found a ceiling (00-Design
§14), and the per-actor one went the same way when it was tested against the same engine. What
decides how much is heard is the rate cap, the chain merge, masking and burst shaping — all of which
judge the mix. A count judges nothing, and it takes the sound away from whoever asked last rather
than from whoever mattered least.

`Engine::LiveVoices()` survives the caps and is not one. It is a leak detector: a non-zero count
with nothing tracked is a loop that was booked and never given back.

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
- **`--headroom` answers "can the mix be made louder", and the answer is usually no.** It mixes
  every composite twice, once as it ships and once with a make-up gain on every cue, and prints how
  much of that gain actually survived `clipCeiling`. Reach for it before any change that raises a
  global level — the loudest composites sit inside the soft clip already, so a global gain reaches
  the quiet contacts and not the hero hits, and the mod comes out flatter rather than louder. It is
  what settled the sound category's default slider position; see §1.
- A **pure refactor should produce byte-identical output.** If it does not, it was not a refactor.
- The `hero moments` check is an absolute rather than calibration: a knockdown carries 0–3 hero
  moments.
- **`slides` says the entry test is finding them at all**, which is what the rework was for — it
  used to find none on the one take that is mostly sliding. `lifted a contact` stood beside it and
  is gone with the slide-end lift; it read 0 on all eight takes, which is the measurement that
  retired the rule.

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
| Per-episode stats (`heroes`, `heroReanchors`, `slides`, `drivenFlights`) | **done** |
| The slide: entry on contacts, two exits, level off body speed, lift on real contacts only | **done** |
| `Proposal::priorityDb` — the level/rank split `config.md` waited on | **done** — plumbed, every weight defaults to 0 |
| `imp_body_limb`, the limb body layer | **declared** — falls back to `imp_body`; nothing recorded yet |
| Per-file pitch and trim in the library's metadata | **done** — identities by default; `sfx adjust` checks them |
| Heard in game | **no** |

### The corpus carries pose now, and that is what the slide rework is built on

`Research/NewRecordings` holds six takes with `_pose.bin` sidecars beside them — the devbench
captures. Everything in §2 that says "measured" is measured on these, including the two clauses that
were untested code when the sidecar did not exist yet: the hero test's arrival clause and the
`Tumble → Airborne` edge.

The slide is the part that most depends on it. Every number the loop and the two exits read —
body speed for the level and the pitch, centre-of-mass travel for the distance clause, the free-fall
flag for the launch exit — comes out of the sidecar, and on a take without one the whole feature
falls back rather than guessing: `SlideSpeed` returns the held tangent instead of the body's speed,
and the launch exit is gated on `haveBodySamples` like every other rule that reads flight.

**A 10 m fall is now the highest-value capture outstanding**, along with a natural-ground surface
and one unparalysed take for the get-up window (00-Design §15).
