# Cloth rustle — a proposal

*Not built. This is the argument for what to measure, what it costs, and the order
to build it in. Every number below is marked as the guess it is — and unlike the
armour axis, all of them can be replaced from the corpus we already have, without
recording anything.*

| Doc | What it is |
|---|---|
| [00-Design.md](00-Design.md) | The design. §12 is the asset list this grows |
| [01-Architecture.md](01-Architecture.md) | The pipeline. §7.1 is the table that decides where each half of this lands |
| [Proposal-Armor.md](Proposal-Armor.md) | The armour axis this rides on. Four classes, already plumbed end to end |
| [Proposal-Slide-Rework.md](Proposal-Slide-Rework.md) | The loop this is modelled on, and the mistakes it made first |
| [config.md](config.md) | What a decibel means, and which side of arbitration it lands on |
| **Proposal-Rustle.md** (this) | Fabric and armour under a falling body: what makes the sound, how we detect it |

Spelling, as with armour: identifiers and ini keys are **`rustle`** and
**`armor`**; prose is **armour**.

---

## 1. The one-paragraph version

A continuous fabric layer riding a knockdown, whose level tracks how hard the
**limbs are thrashing relative to the body** and how fast the body is **rotating**
— the two things a tumble down a staircase is made of and the two things nothing
in the mod currently voices. It is the scrape's shape pointed at a different
signal: one bypass loop voice per actor, level and pitch tracked continuously, a
deadband on the cue and an asymmetric envelope on the level. It ships as four
slots on the existing coverage axis, three of which fall back to the fourth, so
**one recorded wav is a working feature** and a mail rattle is a later file drop.

Everything it needs is already measured and already flowing. The pose sidecar
carries per-limb velocity and per-limb angular speed on every tick of every take
in `Research/NewRecordings`, and the angular half is currently thrown away.

---

## 2. The precedent that has to be answered first

**There was already a continuous cloth layer under every ragdoll and nobody ever
turned it on.** `MotionFoleyConfig` owned it; `bFoleyCloth = 0` in **all
thirty-eight** saved configs, so it went with its slot rather than staying as a
layer nobody switches on.

That is the strongest argument against this feature and it deserves a real answer.
Three things were wrong with it, and all three are structural rather than a matter
of taste:

**Its level was a function of body speed.** A ragdoll's COM speed is highest when
the body is airborne — where cloth is doing comparatively little, because
everything is falling together — and during a slide, where it is drowned by the
grind. So it was loudest at the two moments it had least to say, and quietest
during the tumble, which is the whole of §3.

**It had no armour class.** Every actor got the same file at the same level. That
is wrong for three quarters of the population by construction, and it is the one
thing that has genuinely changed since: `Coverage` is now plumbed end to end and
dropped only at the last step.

**It had nothing to fill.** Or rather, it did and nobody had noticed what it was.
The mod deliberately throws away about nine contacts in ten — the design's own
10:1 reduction ratio — and the body goes on visibly thrashing through every one of
the gaps. Between bursts there is a body cartwheeling down a staircase and
**silence**. A bed pinned to body speed does not fill that, because body speed
does not know the difference between a limp drift and a cartwheel. A layer driven
by the thrashing itself does, and that is the whole proposal.

If those three answers are not convincing, do not build this. It is much cheaper
to decide now than after the assets exist.

---

## 3. When does cloth rustle in a fall, and what do we measure

### 3.1 The physical question

Fabric makes noise when it **moves relative to the body it is on** — a sleeve
sliding over an arm, a hem swinging past a leg, a mail skirt shifting on a belt.
Cloth at rest against a body that is moving at constant velocity makes nothing.

So the quantity is *relative* motion of the garment. Two independent things
produce it in a knockdown, and they are separate enough that folding them into one
number would lose the case the other one covers.

### 3.2 The thrash — relative limb acceleration

The obvious implementation is `|acceleration of the COM|`, and it is wrong. The
centre of mass of a body bouncing down stairs traces a fairly smooth arc between
treads; what is violent is the **limbs**, flung about the body as it turns over. A
COM-driven layer would be near-flat through the most visually chaotic part of the
fall.

So the measurement is per limb, and weighted by how much fabric that limb carries:

```
thrash = Σ_limb  fabricWeight(site, coverage) · |a_limb − a_body|
         ────────────────────────────────────────────────────────
                     Σ_limb  fabricWeight(site, coverage)
```

Three things about it are load-bearing:

**Fabric weight, not `NominalMass`.** `NominalMass` answers "how much body", and
we want "how much cloth". A small `FabricWeight(LimbSite, Coverage)` table beside
it — torso and thighs heavy, calves and upper arms middling, hands, feet and head
near zero unless the class says gauntlets, boots and a helm. Maybe thirty numbers,
and it is the difference between a garment and a noise generator. It is also the
second consumer of the coverage axis, which currently has exactly one.

**`a_limb − a_body`, not `a_limb`.** The subtraction is what makes a flight
correctly quiet: everything on a falling body accelerates at g together, so the
relative term goes to nothing. Make it a switch (`bRelativeToBody`) so the A/B is
one keystroke — I expect it to be obviously right, but it is exactly the kind of
thing that turns out to be doing something else.

**The engine already sums a fabric-shaped quantity over limbs once per tick**, for
the slide's contact fraction, in `ConsumePose`. This is the same shape in the same
place, and the per-limb store it needs (`ActorRuntime::LimbTrack`) already exists
and already holds a position — it needs a velocity and a timestamp beside it.

### 3.3 The tumble — limb rotation

This is the term I would have missed and the one your staircase names. **A limb
rotating at a constant rate drags its sleeve across itself continuously**, with no
acceleration involved at all. A body that is turning over is a body whose every
garment is in sustained relative motion, and rotation is the single most
characteristic thing about a fall down stairs.

```
tumble = Σ_limb  fabricWeight(site, coverage) · angularSpeed_limb · max(1, radius)
```

`angularSpeed` is **already published on every pose sample** —
`PushLimbSamples` fills it from `body->motion.angularVelocity.Length3()` — and
Ingest currently drops it on the floor for `kLimbSample` rows. It is read only for
contacts, by `Coupling`, which multiplies it by the limb radius for exactly this
reason: what matters is surface speed at the limb, not radians. The same product
is the right one here.

That it is a **speed and not an acceleration** is the point, and it is why it
cannot be folded into §3.2. A cartwheel at a steady rate has a large `tumble` and a
small `thrash`; a body slamming from one pose to another has the reverse. Both are
rustle and neither implies the other.

### 3.4 The two weak terms

| Term | Default | Why it is weak, or off |
|---|---|---|
| **speed** | `fSpeedWeight = 0.30` | A multiplier on the two above, never an addend. The same thrash at 600 u/s displaces more cloth further. A body drifting fast and limply must not rustle, which is what makes it a multiplier |
| **air** | `fAirWeight = 0.0` — **off** | A cloak genuinely flaps in a long drop, but `air_whoosh` already fires on exactly this state and giving fabric its own airborne term is two voices saying "this body is falling". Off by default, present so the A/B can be made; if the fabric version wins, `air_whoosh` should be *retired* rather than layered under |

At `fSpeedWeight = 0` and `fAirWeight = 0` the layer is pure thrash and tumble,
and that is the baseline every A/B should start from.

### 3.5 The impact tail, which comes free and is the best part

A limb hitting a stair tread has its velocity reversed in one tick. That is an
enormous relative acceleration, so `thrash` spikes — and with a slow release
(§3.6) the spike decays into a fabric tail behind the impact.

**That is the sound of clothing settling after a hit, and we get it without
inferring anything.** No "the fall is over" state, no synthesised contact, no rule
standing in for a measurement. It is the envelope of a measured quantity decaying,
which is the one shape this codebase has never had to delete.

It also means the layer is automatically loudest immediately after the loudest
impacts, which is where a mix wants it, and it is *continuous through the gaps*
the arbitrator makes — which is §2's third answer, made concrete.

Worth naming explicitly because it borders on something the design rejected:
`SlotId::kSettleRest` stays declared and unfilled, and this proposal does **not**
revive it. The closing cue needed a state meaning "this fall is over", which is an
inference. A decaying envelope needs no such state — it is quiet when the
measurement is small, whether that is between two bounces or after the last one.

### 3.6 The envelope

Raw drive is spiky by nature — it is a second derivative of a pose stream sampled
at frame rate, on a body being hit. Feeding it to a voice unfiltered is a level
that flickers at solver rate. Two mechanisms, because "cloth reacts fast" and
"cloth keeps moving afterwards" are two facts:

- **`fAttackMs`** (~30 ms, guess) — how fast the level rises. Cloth responds
  immediately; a slow attack puts the rustle behind the impact that caused it.
- **`fReleaseMs`** (~280 ms, guess) — how slowly it falls. **This is the delay you
  described**, and §3.5 is what it buys. It is also what keeps a tumble from
  sounding like a rattle: the release fills the gaps between bounces.

One more, to stop a two-second loop reading as a two-second loop:
**`fWanderDepthDb` / `fWanderHz`**, a slow deterministic sine on the level, a
decibel or so at a fraction of a hertz. Deliberately *not* a random walk — the
engine must stay deterministic given a seed, and anything drawing a random number
per tick re-rolls every variant and scatter downstream of it (01 §4, on why a 1/1
damage tier draws no number).

### 3.7 Two hazards the measurement has, both real

**A solver blow-up would make it scream.** The ingest path rejects contact
blow-ups on the arithmetic, needing no threshold; pose has no such check, and a
limb that teleports produces an acceleration of arbitrary size. `fDriveCeiling`
clamps the per-limb term before the sum. This is not optional — the corpus already
contains rows the blow-up detector flags.

**The dynamic range is enormous and the ramp has to be measured, not guessed.**
The `driven` investigation measured a real landing at **5110 u/s²** and a leash
haul at 900–1600. If `fDriveFull` is set anywhere near an impact's acceleration,
every tumble reads as zero; set it near a tumble's and every impact saturates.
Bin the corpus by percentile the way the damage table is binned (p95 / p99 / max,
per motion state) and set the ramp off that. §10 is that job and it is an
afternoon.

---

## 4. What it does in each motion state

The motion axis already answers "what is the body doing", so rustle does not need
a state of its own and must not get one. It reads the existing four:

| State | What rustle does | Why |
|---|---|---|
| `Launch` | **Loud.** Often the first thing heard | The body has been flung and no contact has happened yet. There is currently silence between the shove and the first impact, and the garment is the honest thing to put in it |
| `Airborne` | Quiet by default | Relative acceleration goes to nothing by construction (§3.2) and `air_whoosh` already owns this state (§3.4) |
| `Tumble` | **The case.** Peak value | Limbs flailing, body rotating, and nine contacts in ten discarded. This is the gap the layer exists to fill |
| `Slide` | Ducked hard, `fSlideDuckDb` | A grind has a body loop, up to three limb loops and a grain layer describing the same motion. A fabric layer under all that is mud, and it is precisely the mistake §2 diagnoses |

The duck is scaled by the body loop's own weight, exactly as `bBodyDucksLimbs`
scales `fLimbDuckDb`, so it arrives with the grind rather than switching on. Deep
enough that it is suppression rather than damping, so one slider covers both of
what two behaviours would have been.

`Launch` being the loud one is worth dwelling on, because it is the case with no
competition at all. Nothing else in the mod can voice a knockdown before its first
collision — there is nothing to voice it *with*, since every other layer is built
on a contact. A garment is not.

---

## 5. Slots: one, or one per class?

**Four**, mirroring `armor_*` exactly, with fallbacks:

| Slot | Falls back to | Character |
|---|---|---|
| `rustle_cloth` | — | The base, and the only one that must exist. Soft continuous fabric shifting |
| `rustle_light` | `rustle_cloth` | Leather creak with a small buckle jingle over the fabric |
| `rustle_heavy` | `rustle_cloth` | Mail shift and plate shuffle. Metallic, dense, no ring |
| `rustle_bare` | **nothing** | Declared, empty, and *stays* empty. A naked body is silent |
| `rustle_grain` | — | One catch. Optional, see §6.3 |

The argument for four over one is the armour axis' own and it is not a matter of
taste: **you cannot get from a swish to a mail chink with pitch and trim.** They
are different spectra. Colour is additive, so four classes cost four files rather
than multiplying anything.

The argument for four over four-with-no-fallbacks is `SlotDesc::fallback`, which
the slide rework added: `rustle_light` with no recording behind it plays
`rustle_cloth`, so the pack ships with **one** file and works, and a plate rattle
recorded later is a file drop with no code change and no ini edit.

`rustle_bare` having no fallback is the important asymmetry. If it fell back to
cloth, a naked draugr would swish — worse than silence, and wrong in exactly the
cases a player notices. Empty, with `expectedVariants = 0`, it resolves to nothing
and the loop never starts: the same door `grunt_impact` sits behind.

**One voice per actor, not one per limb.** The scrape splits body and limb loops
because a dragging foot behind a stopped body is separately audible. A garment is
one object, and splitting it would put four voices on every ragdoll to describe
one shirt. The class is resolved once, from the actor's fabric-weighted dominant
coverage — `ActorClassCoverage` already exists for exactly this, and source `0`
(`bodyCoverage`) is the right one here.

The mixed-loadout case — plate cuirass over cloth sleeves — is real and
**explicitly out of scope**. The extension, if it is ever wanted, is two voices
split by fabric weight, and it is additive on top of everything below.

---

## 6. Where each piece lands

Walking 01 §7.1's table:

### 6.1 The measurement — Stage 1, not a strategy

`rustleDrive` is a `CrashState` field beside `bodySpeed` and `contactFraction`,
computed in `ConsumePose` where the per-limb loop already runs. Raw and enveloped
are two fields (`rustleDriveRaw`, `rustleDrive`), because the timeline needs both
to show the envelope working (§8).

Three things it needs that are not there yet, all small:

- `LimbTrack` grows `vel`, `haveVel` and `lastVelMs` beside its existing `pos` /
  `havePos`. That is the whole of the acceleration measurement.
- Ingest keeps `event.angularSpeed` on `kLimbSample` rows, which it currently
  drops.
- `FabricWeight(LimbSite, Coverage)` beside `NominalMass`.

**It is not a `Motion` state**, and it must not become one. Motion is
Launch/Airborne/Tumble/Slide and "the garment is moving" is not a fifth member of
that set — adding one would force every existing rule to have an opinion about it.
Rustle is a continuous scalar, and scalars live in `CrashState`.

**It gates on `haveBodySamples`.** With no pose sidecar there is no measurement,
and the fallback is *off* rather than a guess — the same rule the hero test's
arrival clause and the `Tumble → Airborne` edge already follow. An old take
without pose sounds exactly as it does today.

### 6.2 The voice — a strategy, `ProposeTick`, bypass

`ClothRustleStrategy`, sixth in the list, after `MotionFoley` because it is the
same kind of thing: an actor-level loop with no contact behind it. It proposes on
`ProposeTick` only, never on `Propose`. Its proposal is a **bypass** — not an
onset, not a ride-along. It never claims, it cannot suppress a contact, and it
cannot be suppressed by one.

The plumbing is `EmitLoopProposal` / `StopLoopProposal` with a
`rustleRunning` / `rustleVoice` / `rustleLastDb` / `rustleAnchor` set on
`ActorRuntime`, exactly as `riseRunning` and `scrapeRunning` are. The slot is
pinned at start like `scrapeSlot` is: a coverage that resolves differently
mid-fall must not swap files under a running voice, which is a click.

The anchor is `BodyAnchor(actor)` — the resolved torso limb, never index 0
(01 §7.4, on the whole-body loop that hung off whichever body Havok listed
first). For the player, `BoneFor` puts it on the bone, which matters most at
arm's length.

Distance: `kFull` only, like the airborne rise. A rustle is a close, quiet sound
and there is nothing to gain from spending a voice on one at thirty metres.

### 6.3 The grain layer — the one genuinely debatable piece

The slide's biggest lesson was that a loop alone reads as a noise file, and that
the grain layer is what makes it a slide. That is more true for plate, whose real
sound is a sequence of chinks rather than a sustained anything.

The counter-argument is a rule this codebase has paid for twice: **no inferred
state may invent a sound.** The settle system and the synthesised slide-end impact
were both deleted for firing where the solver reported nothing.

I think it survives, narrowly, and the distinction is worth being explicit about:
the deleted rules invented **impacts**, and an impact is a claim about a collision
the solver did not report. A rustle grain is a claim about *garment motion*, and
garment motion is measured — pose is a measurement, not an inference. There is no
collision it can be unfaithful to.

But it is close enough that I would ship it **off by default**, gated to
`kLight` / `kHeavy` only, and let the ear settle it. If it is on in one saved
config out of thirty-eight, delete it — exactly as the cloth bed was deleted.

### 6.4 Level

The loop bypasses Stage 4, so its dB never acts as a rank and the gain/trim
distinction has no teeth. Name it `fGainDb` anyway, matching `ScrapeLoop:fGainDb`
and `MotionFoley:fAirborneRiseGainDb`, which are the same case.

---

## 7. Config — the `[Rustle]` section

Three groups, as you asked: **how loud**, **what counts as movement**, **how it
moves**. Every default is a **guess** flagged as one, and §10 replaces them from
the corpus. `Config.h` carries the reason beside each; `ConfigSchema.cpp` carries
the key and the perceptual tooltip.

### Intensity — how loud

| Key | Default | What it changes |
|---|---|---|
| `bEnabled` | `1` | The whole feature. Off is byte-identical to today |
| `fGainDb` | `-26` | The layer's level at full drive. Guess: the bed measured 30–36 dB under a hero hit and rustle belongs at the quiet end of that |
| `fDriveRangeDb` | `-20` | How far under `fGainDb` at `fDriveFloor`. Deep, so crossing the floor is not an event in itself — the shape of `ScrapeLoop:fSpeedRangeDb` |
| `fBareTrimDb` `fClothTrimDb` `fLightTrimDb` `fHeavyTrimDb` | `0` | Per-class voicing. Zero at ship, like the armour trims, so the axis costs nothing until somebody turns it |
| `fPlayerTrimDb` | `-3` | Your own ragdoll is at arm's length and follows a bone |
| `fSlideDuckDb` | `-14` | How far a running grind pulls it down, at full body weight (§4) |

### Sensitivity — what counts as movement

| Key | Default | What it changes |
|---|---|---|
| `fThrashWeight` | `1.0` | The relative-acceleration term (§3.2) |
| `fTumbleWeight` | `0.8` | The rotation term (§3.3). Its own weight because the two are independent signals, and this is the pair to A/B first |
| `fDriveFloor` | `150` | Below this there is no rustle at all. **The most important number in the section** — it is what keeps a body lying still silent |
| `fDriveFull` | `1400` | Where the drive reads 1. Currently anchored on nothing; §10 sets both from percentiles |
| `fDriveCeiling` | `6000` | Per-limb clamp before the sum, against a solver blow-up (§3.7). Not optional |
| `bRelativeToBody` | `1` | Subtract the body's own acceleration from each limb's. Off is the naive version, kept as a one-key A/B |
| `fSpeedWeight` | `0.30` | How much a fast body multiplies the two terms above. 0 is pure thrash and tumble |
| `fSpeedForFull` | `600` | Body speed, u/s, at which the speed multiplier saturates |
| `fAirWeight` | `0.0` | **Off.** The airborne addend, against `air_whoosh` (§3.4) |
| `fAirSpeedForFull` | `500` | Fall speed at which it would saturate |
| `fSilenceHoldMs` | `500` | How long under the floor before the loop stops rather than holding at silence. Restarting costs a fade and a cue, and a body that pauses between two bounces should not pay for one |

### Modulation — how it moves

| Key | Default | What it changes |
|---|---|---|
| `fAttackMs` | `30` | How fast the level rises. Short — a slow attack puts the rustle behind the impact that caused it |
| `fReleaseMs` | `280` | How slowly it falls. **The delay**, and what buys the settle tail in §3.5 |
| `fPitchAtFloor` / `fPitchAtFull` | `0.94` / `1.07` | Pitch across the drive range. Small — a wide range is the fastest way to make this sound synthetic |
| `fWanderDepthDb` | `1.5` | A slow deterministic wobble so a two-second loop does not read as one |
| `fWanderHz` | `0.23` | How slow |
| `fLevelDeadbandDb` | `0.25` | How far the level moves before a running loop is re-cued. The scrape's value, and for the scrape's reason |
| `fStartFadeMs` / `fStopFadeMs` | `90` / `240` | Longer than the scrape's at both ends. Fabric has no transient, and a rustle that snaps in is the most obvious thing in the mix |

### The grain layer — off by default (§6.3)

| Key | Default | What it changes |
|---|---|---|
| `bGrainEnabled` | `0` | Off. Turn it on for one config and see whether it survives |
| `bGrainClothToo` | `0` | Off — cloth has no chink. Light and heavy only |
| `fGrainGainDb` | `-18` | |
| `fGrainDriveRatio` | `1.8` | How much harder than the recent envelope a spike must be to count as a catch. At 1.0 every bounce is one, which is a rattle |
| `fGrainMinGapMs` | `140` | |
| `fGrainProbability` | `0.5` | Both, because a gate that is only a rate limiter turns a tumble into a metronome |
| `fGrainPitchScatter` | `0.15` | |

And one row in `[Layers]`: `bRustle`, the class mute, beside the others.

---

## 8. Timeline

The slide lane is the model but not the shape. 01's own note is that **a loop is
not an event, and drawing one as a row of bars was the mistake the cue lane
already made** — so rustle does not get a span lane with end caps. It gets an
**envelope curve**, which is what a continuous level actually is.

Three new `BodySample` fields — `rustleDriveRaw`, `rustleDrive`,
`rustleCoverage` — and one drawing pass:

- **A filled curve** at `rustleDrive`, in a fabric colour. Teal is the slide's,
  lavender is flight's, gold is hero — a warm neutral is free.
- **A thin line over it** at `rustleDriveRaw`. This is the point: the gap between
  the two lines *is* the attack and the release, drawn. Watching the raw drive
  spike on a stair tread and the smoothed level decay into the next one is worth
  more than any number in the panel, and it is the only way to tune §3.6 by eye.
- **Coverage on the hover**, so "why is this one clanking rather than swishing" is
  answerable by pointing.

`rustleCoverage` on the sample rather than only on the cue, because the loop
re-cues rarely by design (the deadband) and the timeline needs the answer on every
tick, not on the four that happened to carry an update.

One free diagnostic falls out: laid against the cue lane, the curve shows exactly
how much of a fall the arbitrator is discarding. A tall rustle envelope with no
cues under it *is* the 10:1 reduction ratio, drawn.

---

## 9. What does not have to change

Worth listing, because it is most of why this is a small feature:

- **No new tracking.** Actors are already tracked across the whole knockdown.
- **No new capture.** `Research/NewRecordings` has six takes with pose sidecars,
  and every quantity in §3 is derivable from them today.
- **No `Offline.cpp` change.** The takes replay now, at the recording's own frame
  boundaries.
- **No plugin change at all**, beyond keeping a field Ingest already receives.
- **No arbitration change.** It is a bypass loop; the rate cap, chain merge,
  masking and burst shaping never see it.
- **No `Verify()` change**, except one new check (§10).

The whole feature is a table, a few fields on `CrashState` and `LimbTrack`, one
strategy, one config section, four slot declarations and a timeline curve.

---

## 10. Verification, and the measurement that comes first

**Phase 0 is a measurement and not code**, and unlike the armour axis it needs
nothing recorded. Dump `thrash` and `tumble` per tick over the whole corpus,
binned by motion state, and read the percentiles the way the damage table's are
read:

| What it settles | How |
|---|---|
| `fDriveFloor` | The p50 of `Tumble` must be well over it and a body at rest well under. If those two overlap, the signal does not separate and the feature does not work — **this is where it stops** |
| `fDriveFull` | The p95 across `Tumble`, not the max. An impact tick is an outlier by construction (§3.7) and a range set to contain it flattens everything else |
| `fThrashWeight` / `fTumbleWeight` | Whether the two correlate. If they track each other across the corpus, one of them is redundant and should be deleted rather than shipped as a slider |
| `fDriveCeiling` | Against the rows the blow-up detector already flags |

The baseline discipline applies: `rds-verify <recordings> <bank> > baseline.txt`
before touching anything, and afterwards a take whose `contactsIn` or
`rejectedBelowFloor` has moved means something upstream was disturbed. With
`bEnabled = 0` the whole corpus must come back **byte-identical**, which is the
acceptance test — the same one the armour axis passed.

One new check, and it is an absolute rather than a calibration: **a take's rustle
envelope must reach the floor before the take ends.** A layer still running when a
body has come to rest is a stuck loop, and that is the failure mode a continuous
voice has.

---

## 11. Phasing

**Phase 0 — measure (§10).** No code, an afternoon, and it can kill the feature
cheaply. Every number in §7's sensitivity table comes out of it, and without it
they are decoration.

**Phase 1 — the measurement in the engine.** `LimbTrack`'s velocity, the kept
`angularSpeed`, `FabricWeight`, the `CrashState` fields, and the timeline curve
(§8). **No sound.** At the end of this you can watch a staircase tumble's drive
curve against its cue lane, which is the right thing to be able to do before
choosing a wav.

**Phase 2 — the voice.** The strategy, the `[Rustle]` section, `rustle_cloth`
with a procedural stand-in so it is audible before any asset exists. Ends with a
knockdown that rustles.

**Phase 3 — the assets.** `rustle_cloth` first and alone. The other three are file
drops and can land whenever. Note that unlike the armour skins this base slot
**should** get a procedural stand-in: those had a "sounds like today" state to
preserve and this has none.

**Phase 4 — the rest.** The grain layer, the mixed-loadout split, and the
`air_whoosh` A/B.

---

## 12. The SFX generation prompts

House style: generate long, cut a stable window from the middle, and the *Post*
note is where the trap lives.

The trap for this family is the one `air_whoosh` names. A text-to-SFX model asked
for "cloth rustle" hands back a **designed gesture** — a swish with an arc, a
beginning and an end. That is unusable, because the engine owns the envelope. The
file must be **flat, steady and seamless**, with no built-in climax. Say so three
different ways or it will not stick.

### `rustle_cloth` ×2 — 1.5–3 s, the base and the only one that must exist

```
continuous handling of heavy linen and wool clothing, steady fabric shifting and
sliding against itself, soft dry rustle, even and unbroken throughout, no sudden
swish, no gesture, no beginning or end, close mic, dry studio foley, no reverb
```

Generate 10–12 s. *Post:* cut a seamless 2 s **from the middle** — the middle
specifically, because the model will put a shape at both ends whatever the prompt
says. **High-pass at 120 Hz**: there is no weight in a garment, and any low end
here fights the composite's body layer on every landing. Check the loop for a seam
at unity *and* at 0.94 and 1.07, since the engine plays it at both. It should be
almost boring alone; anything interesting in isolation is intolerable under every
knockdown.

### `rustle_light` ×2 — 1.5–3 s, leather and buckles

```
continuous handling of a leather jerkin with small brass buckles and straps,
steady creaking of leather with faint irregular metal buckle taps, even and
unbroken throughout, no gesture, no swish, close mic, dry studio foley, no reverb
```

*Post:* the buckle taps must be **sparse and irregular** — three or four a second
at most. A dense jingle is the heavy slot's job, and having it in both makes light
armour sound like mail. Same high-pass, same seam check.

### `rustle_heavy` ×2 — 1.5–3 s, mail and plate

```
continuous shifting of a mail hauberk and plate armour, dense metallic rings
sliding and chinking against each other, steady overlapping small metal contacts,
even and unbroken throughout, no gesture, no clash, no ringing tone, close mic,
dry studio foley, no reverb
```

*Post:* **no pitched ring** — the same note `armor_heavy` carries. A sustained
metallic tone is a bell, and a bell riding every knockdown is the most fatiguing
sound this mod could make. High-pass at 200 Hz. Density is the whole character;
this is the one file allowed to be busy where the other three must not be.

### `rustle_bare` — not generated

Stays declared and empty, with no fallback. A naked body is silent, and that is
the feature rather than a gap (§5).

### `rustle_grain` ×3 — 150–400 ms, one catch (only if §6.3 survives)

```
a single sharp shift of leather and metal buckles, one short creak and chink, one
event only, dry and close, no impact, no thud, close mic, dry studio foley, no
reverb
```

*Post:* no fade-in — the catch is the first sample. Generate **neutral**; the
engine pitch-scatters them.

---

## 13. What I think of the approach

It is the right feature and the reasoning behind it is right. Three notes.

**The correction I would make is §3.2:** the drive is *limb* acceleration relative
to the body, not the body's own. Driven off the COM this would be nearly flat
through the most visually violent part of a fall, which is the part it exists to
cover.

**The addition I would make is §3.3:** rotation. A limb spinning at a steady rate
drags its sleeve continuously with no acceleration at all, and a staircase tumble
is mostly rotation. It is a *speed* where the other term is an acceleration, which
is why it cannot be folded in — and it is already measured, already published on
every pose sample, and currently thrown away.

**The thing to be honest about is §2.** A continuous cloth layer under a ragdoll
has been built here before and was never once switched on. The three answers I
give are, I think, real ones — the old one was pinned to the wrong quantity, had
no armour class, and nobody had framed what it was for — but they are answers to a
failure rather than evidence of a success, and §10 exists so the feature can be
killed for the price of an afternoon rather than a pack.

**What I would cut if anything:** `fSpeedWeight`. I expect the thrash and tumble
terms to already carry everything a fast fall has, and a third knob that mostly
correlates with the first two is exactly what 01 §7.4 warns about. Ship it at
0.30, and if the corpus of saved configs never moves it, delete it.
