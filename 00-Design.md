# Physical Ragdoll Sounds — the design

The settled design. Supersedes the asset and strategy lists in every earlier draft.

| Doc | What it is |
|---|---|
| **00-Design.md** (this) | The design. One authoritative asset list, one strategy list, the decisions and why |
| [04-Reference-Analysis.md](04-Reference-Analysis.md) | Measurements of the Skate 3 reference clips — where the layer timings and level budgets come from |
| [Research/](Research/) | The capture study. What the engine can tell us and how far it can be trusted |

---

## 1. Decisions

| Question | Settled |
|---|---|
| Vanilla's own ragdoll impact sounds | **Suppress.** We own the whole mix — see [§8](#8-suppressing-vanilla) |
| Vocal layer (grunts, screams) | **Not in v1.** The slot stays declared but unfilled, so it is a config-only addition later |
| Player ragdoll | **In scope**, with its own mix profile — see [§9](#9-the-player) |
| Loudness relative to combat | **Ini-configurable.** Master gain, a trim per layer role, and a trim per file in the bank |
| Airborne anticipation rise | **Ini-configurable**, on by default at a low level |
| How gnarly by default | **Gnarly by default, gated on impact strength.** Crunch and gore layers ship enabled, behind intensity gates set so an ordinary knockdown rarely triggers them and a real fall always does |
| Death versus knockdown | **No distinction.** Death by impact gets the same gnarly treatment. Removes a subsystem — a death ragdoll simply never leaves `Rest` |
| Gear (sheathed weapons, shields) | **Out of scope** |
| Distance | **First-class.** Three tiers, with actors beyond audible range culled entirely — see [§10](#10-distance) |

---

## 2. The one-paragraph design

A ragdolled actor produces thirty to sixty collisions over one to three seconds. We turn that into
**four to six audible moments**, each built as a **timed stack of layers** rather than a single
sample, with a continuous foley bed underneath and real silence between the bursts. Physics decides
when, how loud, where and how long. Design decides how many, which one, and what it sounds like.
Almost all of the work is deciding what *not* to play.

---

## 3. Physics or design — the rule

**Physics owns *when, how loud, where, how long*. Design owns *how many, which one, what it sounds
like*.**

The reason is falsifiability. A listener checks the physics-owned quantities against what they see —
a hard landing must be loud, a slide must last as long as the slide, a sound must come from where the
limb hit. Get those wrong and it reads as broken immediately. They cannot check the design-owned
ones; nobody knows what a ragdoll head hitting a floorboard *should* sound like, only whether they
liked it.

That also matches how far the capture data can be trusted: timing, intensity and contact position are
measured cleanly; surface identity is workable but only tested indoors on four materials; what a
body-on-body contact means acoustically is not in the data at all.

Over-physicalise and you get a machine gun. Over-design and it stops tracking the picture.

---

## 4. Architecture

Six stages. Everything except Stage 3 is fixed infrastructure — that is deliberate, and it is what
keeps the strategy layer from becoming a plugin framework.

**Stage 0 — Ingest.** Gate on ragdoll state. Collapse duplicate reports of one collision. Reject
solver blow-ups on the arithmetic rather than a threshold. Tag each survivor with limb site, surface
class, coverage, and thud-versus-graze. **Route self-collisions to the foley bed, not the impact
path** — half of all contacts are one limb touching another limb of the same body, and an arm
brushing your own thigh makes no impact sound. A high threshold lets a genuine self-hit through.

**Stage 1 — Crash state.** A running per-actor summary, a few dozen floats: time since ragdoll start,
energy accumulated, peak so far, contact count, body speed and height, airborne flag, leading limb,
head-down, sliding ratio, surface underneath, distance to listener.

**Stage 2 — Phase machine.** `Launch → Airborne → PrimaryImpact → Tumble → Slide → Settle → Rest`.
Each phase sets what may be audible and how much budget it gets. This is the main lever for
"unobtrusive" — the last twenty contacts of every knockdown are settles and should be nearly silent.

**Stage 3 — Strategies.** The pluggable layer. Six of them ([§11](#11-strategies)). They **propose
only**. The one cross-strategy mechanism is *claiming* a contact so it does not also reach the
default path.

**Stage 4 — Arbitration.** Fixed rules, in order:

1. **Global rate cap** — no two impact onsets closer than ~46 ms, regardless of limb. Measured floor
   in three independent reference clips, and about where hearing stops resolving two impacts as
   separate.
2. **Chain merge** — contacts on one limb chain inside a short window collapse to one cue at the
   strongest. This is the "a strong hand impact silences the elbow and plays one arm flop" rule.
3. **Temporal masking** — a decaying loudness ceiling per actor; anything proposed more than ~12 dB
   under it is *dropped entirely*, not played quietly. Realistic, and it is what turns a dozen
   simultaneous contacts into one event with texture.
4. **Burst shaping** — audible events cluster into bursts of three to five grains inside 200–400 ms,
   then at least 300 ms of near-silence. The arbitrator picks *bursts*, not individual sounds.
5. **Spatial collapse** — during a hero moment, place every layer at one point. Several points read
   as several events; one point reads as one event with detail. Spread back out during the tumble.

**Stage 5 — Render.** Cue list → voices. Stage 4's output is an abstract cue list, which is the clean
seam: the game renders it through Skyrim's audio, the testbench through miniaudio, both implementing
the same limited feature set.

---

## 5. How an impact is built

The most important mechanism in the design, and the one the reference clips taught us. **An impact is
not a sample. It is a timed stack**, and the biggest part of it arrives late.

| Layer | Offset | Level | Character |
|---|---|---|---|
| `imp_transient` | **0 ms** | quietest, −5 to −18 dB | Bright, fast attack. The contact itself |
| `surf_*` | +0–15 ms | −6 to −12 dB | What it hit |
| `imp_body` | +10–30 ms | −2 to −8 dB | Low-mid flesh and mass |
| **`imp_sub`** | **+55–75 ms** | **loudest layer, 0 dB** | **Pitched boom sweeping ~150 Hz → 30 Hz** |

The sound starts bright and quiet and finishes dark, loud and short. That late sub is what makes a hit
read as *mass* rather than as *contact*, and it is the single biggest contributor to the gnarl. Bake
the downward sweep into the file; scale the whole thing with the engine's live pitch control so a
heavier impact starts lower and reads bigger from one asset.

Offsets are structured with a few milliseconds of scatter — not random jitter, which would smear the
shape rather than build it.

**Loudness comes from layer balance, not from tiers.** A light contact is mostly transient with almost
no sub; a heavy one is sub-dominant with the transient riding on top. That gives a smooth continuum
across the whole intensity range out of eight files, with no boundaries to hide.

**Calibrate the whole range onto about 35 dB** — the references span 13–17 dB across their onsets with
the bed sitting 30–36 dB under the hero hit. A naive log curve would give 60 dB and sound wrong at
both ends.

**Shape the level twice, and only once before the layers are chosen.** Intensity decides both what a
contact is made of and how loud it comes out, so reaching for the dynamic range because ordinary
knockdowns are too quiet also re-balances every composite in the mod — two changes arriving together
with no way to hear which did what. The same three numbers therefore exist again at Stage 5, applied
against the intensity the cue was already built with: range, curve and knee, moving loudness and
nothing else. Neutral by default, so the second half costs nothing until it is turned.

**One gain trim per file, under the per-role trims.** A role trim balances kinds of layer against
each other; it is the wrong tool for a bank, where three separately recorded surface skins arrive at
three different levels and pulling the surface trim to fix stone takes wood down with it. Both land
after arbitration, alongside the mutes, so neither can change which cues were chosen.

---

## 6. Smooth or threshold

**Continuous** for anything on a spectrum: loudness, pitch, layer balance, loop level.

**Discrete** for anything that is a fact about what happened: a crunch, gore, an obliterate. You cannot
have thirty percent of a bone break, and one played quietly sounds like a bug.

**Soften a gate with probability, not volume.** Near the threshold fire it sometimes; well over, always.
Plus hysteresis so it does not flicker during a tumble.

Concretely, from the capture data: an ordinary shove peaks at 355–543 units/s, a three-metre fall at
600–855, a ten-metre fall at ~960. So a crunch gate opening around 500 with the probability ramp
reaching certainty near 700 means an ordinary knockdown cracks occasionally — which feels alive — and
a real fall always does. Gore sits at the obliterate tier, above anything a fall can produce.

**Every gate is a fraction of the loud anchor, not a speed.** What each one is really saying is where
it sits in the range the mod hears — "a head accent belongs on the top third of that" — and written as
a raw number that statement quietly stops being true the moment the anchor moves. Re-anchoring then
meant re-deriving four thresholds by hand, and forgetting one left a gate that had silently become
free or unreachable.

**The ramp starts at a chance, not at zero.** A gate whose probability is nil at the speed it opens
at has not opened there: with 500 and 700 the bottom of the span was dead and the first crunches
really arrived around 550, which is neither what the slider says nor what softening a gate was for.
The threshold has to be a *maybe*. A certain speed at or below the gate is not a mistake either — it
is how you ask for a hard one — so it collapses to certainty rather than to a one-unit ramp.

---

## 7. Making it read as one fall

**Bursts and gaps.** The reference rhythm is unambiguous: three to four grains at 46–104 ms spacing,
then 300–730 ms of nothing. A three-second tumble down a hill is *four audible events*. Against
30–60 collisions per knockdown, that is roughly a **10:1 reduction** — and it applies even to the
contacts that pass a sensible loudness bar.

**Hero moments come in ones to threes.** The top events in the references sit within a decibel of each
other, then everything else drops 9–17 dB. Not one dominant hit — a small group of peers, then a
cliff. That suits a faceplant, which genuinely has a knee, a chest and a head arriving within 200 ms.

**Close the event.** One quiet settle cue when energy drops below the floor. Falls that trail off feel
unfinished.

### Repetition, in order of value

1. **Layer, don't select** — four roles at three variants is dozens of distinct composites from a
   handful of files.
2. **Pitch is free and continuous** in this engine. Random ±2–3 semitones per voice plus a systematic
   downward bias with intensity. Zero extra files, and it beats doubling the bank.
3. **Shuffle bag, not random.** Random repeats immediately, and immediate repeats are what people
   notice.
4. **Scatter the layer offsets** a few milliseconds each time, so the composite envelope is never
   identical.
5. **Modulate the loops continuously** — the bed differs every fall by construction and papers over
   the one-shots underneath.

---

## 7a. The slide

A slide is the one stretch of a fall that is **continuous**, and everything else in the design is
about picking moments out of a stream. So it is worth stating separately what it is made of.

**Physics owns all four of when, how loud, where and how long**, and unusually there is nothing left
for design to own but the sample. That falls straight out of §3's falsifiability rule: a slide is
the most checkable thing in the mod. A listener watches a body skid across a floor and can hear
whether the grind lasts as long as the skid and gets quieter as it slows. Nothing about it is a
matter of taste.

**Loudness is the body's speed and nothing else.** No distance ramp, no duration ramp, no
running maximum: the loop sits `fSpeedRangeDb` under its level at `fSpeedForMinGain` and reaches
full level at `fSpeedForMaxGain`, tracking the measured centre of mass continuously. Pitch rides
the same number. The fades stay, because their job is to hide the transition rather than to shape
the level, and a loop that snaps in at its running level is the most obvious thing in the mix.

**Entry is a collision question and exit is a body question.** Grazing — sideways motion instead of
a hit — says a slide has started, and it says it well. It cannot say a slide is still going,
because collisions are dense when a fall is busy and absent when it is not.

**A slide ends three ways, and each sounds different.**

| It ended because | and so |
|---|---|
| the body came to rest | the loop fades out and the ordinary closing cue finishes the event |
| the body left the ground | the loop fades over the shorter **launch fade** — the surface is simply gone, and the ordinary fade drags a grinding rumble out behind a body already in the air |
| **something stopped it** | an **impact goes there**, and if the body was still travelling fast, a hero moment with it |

The third is inferred rather than measured, which is the whole reason it works: a slide that stops
for no reason the body can account for was stopped by something. That collision is regularly missing
from the contact data — a limb catches, reports one glancing row, and the body is simply stopped —
and the old machine's answer to the loudest moment of a slide was to fade a loop out over silence.

---

## 8. Suppressing vanilla

Vanilla plays a body impact on every ragdoll contact and resolves nearly every surface in the game to
the same dirt sample. With ours layered on top, everything doubles and half the mix is out of our
control. So we silence it.

**Method:** `BGSImpactData` holds its two sounds as plain `BGSSoundDescriptorForm*` members (`sound1`
from SNAM, `sound2` from NAM1). Walk the body impact sets — `PHYBodyMedium`, `PHYBodyLarge`,
`PHYBodySmall`, `PHYBodyBones`, `PHYBodyMetalLarge`, `PHYBodyMetalSmall`, plus the armour and meat
sets — collect every distinct `BGSImpactData` they map to (about eight records, since these sets map
dozens of materials onto one to three impacts each), and null those two pointers once at data load.

Why this rather than a hook: no addresses, no thread concerns, decals and hazards keep working, and
form-data edits like this live in memory only — they never reach a save file. Behind
`SuppressVanillaBodyImpacts`, logging every form it touches.

**Consequences to accept.** It is global: it silences body-material impacts beyond ragdolls too —
a dragged corpse, a thrown severed head — which is fine, because we take those over anyway. And any
other mod expecting those descriptors loses them. Both are the right trade for owning the mix, but
they belong in the mod description.

**It also moves the loudness reference.** With vanilla gone we are not sitting next to it any more,
so our levels calibrate against footsteps and combat instead.

**And it is reversible at runtime, which is how the reference gets checked.** Nulling the pointers
keeps the originals in a table beside the flag, so putting them back is a walk over that table —
`RestoreVanillaBodyImpacts`. The testbench's **Use Vanilla Audio** switch flips that and mutes our
renderer together: vanilla's impacts come back, every cue is still made and still recorded but never
becomes a voice, and the two mixes can be heard against each other inside one session on the same
body falling down the same stairs. Either half alone would be a comparison against silence. Going
back re-reads the ini rather than assuming suppression, so an install deliberately running with
vanilla underneath ours is left the way it was; and the mod puts itself back the moment the link
drops, so a closed testbench can never leave a session silently muted.

---

## 9. The player

The player's own ragdoll runs the same ingest and the same strategies — no new plumbing — but gets its
own mix profile, because at zero distance it is a different acoustic problem.

- **Attach voices to the bones** rather than collapsing to a world point. At arm's length the collapse
  trick stops helping and starts sounding like the audio is inside your head.
- **Trim the sub layer.** A 30 Hz boom at zero distance through headphones is overwhelming, and in VR
  low frequency is felt as much as heard. Its own gain offset.
- **Skip the airborne whoosh** by default — you are the one moving, and the visual already tells you.
- Everything in its own `[Player]` config section, so it can be tuned without touching the NPC mix.

---

## 10. Distance

Three tiers, evaluated per actor per tick on the game thread, not per contact.

| Tier | Range | Behaviour |
|---|---|---|
| **Full** | under ~10 m | Everything: composites, grains, loops, bed |
| **Simplified** | ~10–30 m | Hero composites only. No grains, no loops, no bed — nobody resolves the detail at that range anyway |
| **Culled** | beyond ~30 m | Nothing. Stop tracking the actor entirely, drop its crash state |

Culling is not an optimisation bolted on afterwards — it is what keeps a battlefield of ragdolls from
ever becoming a performance question, and it makes the mix cleaner at the same time. Both radii are
config values.

---

## 11. Strategies

Six. Each declares a **manifest** — its sound slots (id, role, description, expected length, variant
count, and the axes it varies over: coverage, surface, size) and its parameters (name, type, range,
default, and one line on what it changes *perceptually*). Slot resolution walks the axes and falls
back, so a missing file is a quieter mod rather than a broken one — which is what lets us ship
thirteen files and grow to twenty-nine without touching code.

The manifest is the real payoff: the testbench builds its whole UI from it, so tuning is sliders
rather than ini edits.

**Strategies propose. The arbitrator disposes.** No strategy modifies another's cues. Claiming a
contact is the only cross-talk.

| Strategy | Job |
|---|---|
| `ImpactComposite` | The core. Builds the timed layer stack for any audible impact, including the surface skin. Intensity drives layer balance, so this covers everything from a quiet tap to an obliterate without tiers |
| `HeadImpact` | Head contacts get their own layer and their own gate |
| `CrunchGore` | The gnarly gate — granular crunch and, above the obliterate tier, the wet layer. Probability-ramped |
| `ScrapeLoop` | Voices the slide: a looping grind whose level and pitch are the body's measured speed |
| `MotionFoley` | The continuous bed: cloth, air, and the airborne anticipation rise as a parameter on it |
| `SettleClose` | The closing cue |

Things that are **not** strategies, because they are fixed rules: the rate cap, chain merge, masking,
burst shaping, spatial collapse, settle suppression, and self-contact routing. Several of these were
strategies in the first draft and did not deserve to be.

---

## 12. Assets

**29 files.** Author everything **dry, punchy and pre-limited**, peaking near full scale — the game
applies the cell's reverb, and a baked tail fights it and turns overlaps to mud. All dynamics come
from runtime gain.

### Impact composite

| Slot | Var | Length | Offset | Character |
|---|---|---|---|---|
| `imp_transient` | 3 | 60–120 ms | 0 ms | Bright, fast attack. The contact itself. Quietest layer |
| `imp_body` | 3 | 150–250 ms | +10–30 ms | Low-mid flesh and mass |
| **`imp_sub`** | 2 | 250–400 ms | **+55–75 ms** | **Pitched boom, ~150 Hz → 30 Hz. Loudest layer. The single most important file in the mod** |

### Surface skins

| Slot | Var | Length | Notes |
|---|---|---|---|
| `surf_wood` | 2 | 120–200 ms | Hollow knock |
| `surf_stone` | 2 | 100–160 ms | Hard, short |
| `surf_soft` | 2 | 150–250 ms | Dull. The default for anything unresolved |

### Grains and texture

| Slot | Var | Length | Notes |
|---|---|---|---|
| `limb_tap` | 4 | 40–100 ms | Burst filler. Quiet, dry, heavily pitch-scattered |
| `crunch_gran` | 2 | 250–400 ms | Dense granular crackle in the low-mid. This is what a bone break actually is — ten times the transient density of a plain thud in that band, not a snap sample |
| `gore_wet` | 2 | 200–400 ms | Squelch. Obliterate tier only |

### Loops

| Slot | Var | Length | Notes |
|---|---|---|---|
| `scrape_loop` | 1 | 1.5–3 s | Low-tilted grinding rumble with grain riding on it. **Not** a hiss |
| `foley_cloth` | 1 | 1.5–3 s | Cloth rustle, no transients |
| `air_whoosh` | 1 | 1–2 s | Low airy movement |

### Accents

| Slot | Var | Length | Notes |
|---|---|---|---|
| `head_impact` | 2 | 300–500 ms | Dull skull thud with a granular edge and a slight ring |
| `settle_rest` | 2 | 200–400 ms | Soft final flop. Closes the event |

### Declared but unfilled

`grunt_impact` and `scream_big` stay in the manifest with no files behind them. Fallback resolution
skips them silently, so adding voice later is a config change.

**First taste: 13 files** — `imp_transient` ×3, `imp_body` ×3, **`imp_sub` ×2**, `limb_tap` ×3,
`scrape_loop`, `foley_cloth`. Build the sub layer in the first pass; without it none of this will
feel like the references.

---

## 13. What the engine gives us

Verified against CommonLibVR. Per voice: **volume** (live), **pitch** (continuous, live), **position**
(updatable), **attach to a bone**, **whole-file looping**, **fade in / fade out over N ms**, and
**play a raw wav straight off disk** — no plugin, no sound records, no Creation Kit.

Not available: per-voice filtering or EQ, reverb send control (the cell's acoustic space applies
automatically, which is what we want), and sample-accurate scheduling — voices start on frame
boundaries, so our layer offsets quantise to 7–20 ms. At 46 ms minimum spacing that is acceptable, and
it doubles as free scatter.

Two consequences. **Anything spectral must be baked or faked with pitch** — stay within ±3 semitones,
and let intensity bias downward, since lower and longer read as heavier anyway. And **the testbench
must render with exactly this feature set**; anything richer goes behind a flag marked not shippable,
or we will tune something the game cannot reproduce.

We also cannot duck other game audio — there is no bus control. So "louder than combat" is achieved
purely by our own gain, which is why it is a config value rather than a mixing decision.

---

## 14. Performance and robustness

A few dozen floats per tracked actor per frame plus a small cue ring. Distance culling means the
actor count stays small regardless of what is happening.

**Voice count is not a budget.** It was assumed to be one, and the assumption was tested: a
voice-limit run started 288 sounds with 224 alive at once and the sound manager holding 257, and
the engine refused none of them — it never found a ceiling, it ran out of sound to play first. So
there is no global cap. What decides how much is heard is the rate cap, the chain merge, temporal
masking and burst shaping, all of which judge the mix; a ceiling on the count judges nothing, and
the one we had could only take sound away from the second and third body in a brawl to guard
against a limit that was not there.

**Measured, not asserted.** The testbench's **benchmark** button under the transport replays the
loaded take through the backend as fast as it will go and reports the fastest run — per run, per
engine tick (a tick is a game frame), and as a multiple of realtime. Tracing is off for it, because
the game never traces and the transport's own "RunOffline + mix" figure is mostly the cost of
filling the timeline. While split A/B it measures both configs back to back and prints the
difference, which is the form the question actually has: not *what does the engine cost* but *does
this setting cost anything*. `--bench` is the headless twin, over every take.

What it says today, over the whole capture set: **about 0.3 µs per frame for an ordinary
knockdown**, rising to 1.8 and 2.8 µs on the two long takes that are several knockdowns rather than
one fall. And the cost barely moves with the cue count — a config emitting 179 cues over
`Proventus_..._log_14` ran within 0.2% of one emitting 471 over the same take. **The work is in
ingest and arbitration, which every contact pays for, not in emission, which only the survivors
do.** That is the right shape: it means the 10:1 reduction of [§7](#7-making-it-read-as-one-fall) is
bought for the mix rather than for the frame time, and that tuning for taste cannot make the mod
expensive.

**Frame rate:** every window in seconds with a floor, never in frames or fixed milliseconds. The unit
that stays stable across frame rates is the collision, not the report.

**Other mods' impulses:** expect speeds far outside anything physical. Soft-clip the intensity curve
rather than rejecting — a silent obliterate is the worst possible outcome.

**Unknown skeletons:** map bones by name, degrade to a size-based generic sound when a name is not
recognised, and re-resolve on every ragdoll rebuild rather than throttling and going deaf.

---

## 15. Still open

- **The get-up window.** Nothing in the capture data measures it, because the run paralyses every
  subject. We need one unparalysed take before picking the silence window after a ragdoll ends. Does
  not block v1 — death ragdolls, which never get up, are the common case.
- **Natural ground.** No dirt, grass, snow, gravel or water contact exists in the capture set at all.
  `surf_soft` covers them acceptably until someone records on snow.
- **Character-on-character.** Three takes tried and all three missed, so the case has no data.
- **The intensity ceiling** is quoted inconsistently across the research docs — the dataset map lists
  the ten-metre fall among the discarded takes while three other docs use it as the clean 960 units/s
  reference. Worth resolving, since the loudness curve calibrates against it.
