# Config naming: what a decibel means

Every gain in this mod is a decibel, and until you know *where* it is applied
you cannot tell what it does. Two parameters can both read "-27.5 dB", produce
the same level at the speaker, and differ in whether the sound plays at all.

This file fixes the vocabulary. It is a naming rule, not a feature.

---

## Terminology

| word | means | here |
|---|---|---|
| **damp** | make it quieter; the event still happens | `fCompanyDampDb` |
| **demote** | move it to a lesser class, not a lower level | `intensity < 0.15` becomes a `limb_tap` |
| **duck** | damp it *because* something else is happening | `fPreImpactDuckDb` |
| **suppress** | stop it existing at all | the arbitrator's drops |
| **gate** | suppress it for falling below a threshold | `fGateFrac`, `fCrunchGateFrac` |
| **mask** | suppress it because something louder covers it | `maskCeilingDb` |
| **cap** | suppress it because a budget is spent | `fRateCapMs`, `iBurstMaxGrains` |
| **merge** | suppress it by folding it into a neighbour | `chain merge`, `bCollapseManifolds` |
| **mute** | suppress its whole class | `Layers:b*` |
| **scale** | multiply a number | `fMaxIntensityScale` |
| **bias** | add an offset to a number | `fPitchIntensityBiasSemis` |
| **clamp** | bound a number to a range | `MixParams::clipCeiling` |
| **compress** | shrink a range, loud more than quiet | `fDynamicRangeDb`, `PostIntensity` |
| **ramp** | turn two thresholds into a 0..1 position between them | `fHeadClearMs`, `fFullTransferFrac` |
| **gain** | a level change made before arbitration | see below |
| **trim** | a level change made after arbitration | see below |
| **weight** | a priority change with no level change | reserved |

Merged away as duplicates: *attenuate* (= damp), *limit* (= clamp). *Roll off* is
not ours - `Emit` leaves distance attenuation to the game's own output model.

## The line

Arbitration (Stage 4) decides which proposals become sound. It reads one number
off each proposal: `Proposal::levelDb`. Everything a strategy folds into
`levelDb` before Stage 4 is therefore **two decisions at once** - how loud, and
how important. Everything added afterwards, in `Emit` (Stage 5), is loudness
only.

```
Stage 3  strategies build Proposal::levelDb   ─┐
                                               │  ← THE LINE
Stage 4  arbitration reads levelDb            ─┘
Stage 5  Emit() adds the rest and renders
```

`Engine.cpp`, the render line, is the whole of the post-arbitration side:

```cpp
cue.gainDb = layer.gainDb + budget.gainTrimDb + masterDb + postShapeDb +
             proposal.postTrimDb + LayerTrimDb(layer.slot, actor.isPlayer) +
             compressCutDb;
```

`Proposal::postTrimDb` is the one channel every strategy trims through - the
glancing cut, the air-time lifts, the head accent's own voicing, all summed. Emit
must not grow a term each time a rule learns to trim.

## The three words

| word | applied | changes loudness | changes arbitration |
|---|---|---|---|
| **Gain** | before the line | yes | **yes** |
| **Trim** | after the line | yes | no |
| **Weight** | before the line | no | **yes** |

- **Gain** is part of what the event *is*. A hard contact has more gain than a
  soft one, and it also deserves to win the rate cap. Use it when the number
  describes the collision.
- **Trim** is a correction applied to the finished decision. "This wav is hot",
  "the sub is too much in VR", "the settle phase should sit back". Use it when
  the number describes the *mix*, not the event.
- **Weight** does not exist yet. It is reserved for the planned
  `Proposal::priorityDb`, so an event can be made more or less important without
  being made louder or quieter.

The suffix carries it: `f…GainDb`, `f…TrimDb`, `f…WeightDb`. A reader must never
have to open `Engine.cpp` to find out which side a slider is on.

## Why it matters

`HeadImpact:fGainDb = -27.5` is a voicing decision - the head_impact wav is hot
and needs pulling down. It sat harmlessly on the pre-arbitration side for as
long as the head accent was an accessory, because accessories skip Stage 4
entirely.

Then `bHeadClaimsOnset` was added. A head impact stops being an accessory and
becomes its own onset - and the instant it does, that -27.5 dB voicing trim
becomes its priority, and the loudest skull landing in the take loses the sort
to a hand.

**The trap is not that the parameter was on the wrong side. It is that a flag
moved it across the line at runtime.**

That flag is now `HeadImpact:bClaimsOnsetOnHero` and it fires on the moment axis
rather than on the head's own air-time ramp - but **the trap is unchanged**. A
better trigger does not fix it; the flag still moves the proposal across the
line, and `fGainDb` still becomes a rank when it does. The only real fix is the
`Proposal::priorityDb` split at the end of this file.

Hence the hard rule below - and the pairs that exist because of it.
`fGainDb`/`fTrimDb`, `fHeadGainDb`/`fHeadTrimDb` and
`fCompanyDampDb`/`fCompanyTrimDb` are each the same decision split across the
line, so voicing can be moved off the sort without giving up the ability to put
it there deliberately. Until `priorityDb` lands, putting head voicing in the
trim is the mitigation.

---

## Rules

1. **The suffix states the side.** `Gain` before the line, `Trim` after it,
   `Weight` for priority alone.
2. **No flag may move a parameter across the line.** If a proposal can be either
   an onset or an accessory depending on config, its level must be assembled the
   same way in both cases, and anything that should not affect priority belongs
   in a `Trim`.
3. **A tooltip's first clause names the side** when it is not obvious from the
   suffix - "after arbitration" or "before arbitration".
4. **When in doubt, `Trim`.** A number that only makes a sound quieter is always
   safe. A number that quietly reorders the mix is the bug this file exists to
   prevent.

---

## Inventory

### Before the line - these set loudness *and* priority

| parameter | what it feeds |
|---|---|
| `Intensity:fDynamicRangeDb` | `onsetGainDb`, the base every proposal starts from |
| `ImpactComposite:fTransientGainAtMinDb` / `AtMaxDb` | a composite layer, and the tap's whole level |
| `ImpactComposite:fSurfaceGainAtMinDb` / `AtMaxDb` | a composite layer |
| `ImpactComposite:fBodyGainAtMinDb` / `AtMaxDb` | a composite layer |
| `ImpactComposite:fSubGainAtMinDb` / `AtMaxDb` | a composite layer |
| `HeadImpact:fGainDb` | the head accent |
| `AirTime:fHeadGainDb`, `HeadImpact:fCompanyDampDb` | the head accent |
| `AirTime:fHeadMaxLevelDb` | a ceiling on the accent once the air-time boost is in |
| `AirTime:fBodyGainDb` | any contact that is not the head, after air time |
| `AirTime:fBodyMaxLevelDb` | a ceiling on that lift |
| `CrunchGore:fCrunchGainDb`, `fGoreGainDb` | the crunch and gore accessories |
| `ScrapeLoop:fGainDb`, `fSpeedRangeDb` | the scrape loop |
| `MotionFoley:fBedGainDb`, `fAirborneRiseGainDb`, `fPreImpactDuckDb` | the bed and the rise |
| `SettleClose:fGainDb` | the closing cue |

`levelDb` for a composite is the **max across its layers**, so the four
`ImpactComposite` pairs above already leak layer voicing into priority. Trim
`fSubGainAtMaxDb` far enough and you change which contacts win the rate cap.
That is live today and is the same confusion in a different place.

### After the line - loudness only

| parameter | applied as |
|---|---|
| `Motion:fLaunchTrimDb`, `fAirborneTrimDb`, `fTumbleTrimDb`, `fSlideTrimDb`, `fRestingTrimDb` and `Hero:fTrimDb` | `budget.gainTrimDb`, picked by `BudgetFor` |
| `Mix:fMasterGainDb` | `masterDb` |
| `Player:fMasterGainDb` | `masterDb`, player only |
| `Mix:fTransientTrimDb`, `fBodyTrimDb`, `fSubTrimDb`, `fSurfaceTrimDb`, `fGrainTrimDb`, `fLoopTrimDb` | `RoleTrimDb` |
| `Player:fSubTrimDb` | `RoleTrimDb`, player only |
| `SlotGain:*` (all 14) | `SlotTrimDb` |
| `HeadImpact:fTrimDb`, `fCompanyTrimDb` | `Proposal::postTrimDb` |
| `AirTime:fHeadTrimDb`, `fBodyTrimDb` | `Proposal::postTrimDb`, via `Contact::modTrimDb` |
| `GlancingImpact:fMaxTrimCutDb` | `Proposal::postTrimDb` |
| `PostIntensity:fExtraRangeDb`, `fCurveExponent` | `postShapeDb` |

The `[Phase]` section is `[Motion]` now, and its hero rows moved to `[Hero]`;
every one of them loads under its old name through `ParamDesc::legacySection`,
so no ini needs touching. The four `AirTime:fHeadHalo*` keys are gone entirely.

Note `PhaseBudget` already splits correctly on its own: `gainTrimDb` is read
only in `Emit`, `maxCuesPerBurst` only in `Arbitrate`. Same struct, both sides,
no ambiguity.

### Neither - a cutoff, not a gain

`Mix:fVoiceFloorDb` discards a **layer** whose rendered level falls under it,
after arbitration has already paid for it. If every layer of a proposal goes
this way the arbitrator rolls its whole budget back, so it does not spend a
burst grain on silence. It is not a gain and should keep its own name.

`[Compress]`, behind `Compress:bEnabled`, is the same kind of thing at the other
end: a **compress**, in the vocabulary above, one threshold per class of cue
plus one shared ratio, and the only compressor on the render line. Four things
about it are deliberate.

- It **squeezes rather than clamps**. See "why not a hard cap" below.
- It is **measured against `Proposal::levelDb`** - the pre-trim number Stage 4
  itself sorted on - and not against the rendered level. See "why not an
  absolute level".
- It is **applied post-arbitration**, so it cannot decide which contacts win the
  rate cap. A held hero hit is still the hero hit of its frame; that is rule 2.
- It is taken **once per proposal**, off the loudest layer, and subtracted from
  every layer of the stack. Holding each layer where it stood would put the
  transient, the body and the sub all together, and the layer balance is what
  decides *what* an impact is made of - so that would be a different impact
  rather than a quieter one.

Which of the nine thresholds applies comes off **layer 0's `CueReason`**, the
layer the proposal was built around. `kSurfaceSkin` has no threshold of its own:
it is only ever a layer inside a composite, which is one moment and takes one
cut. `CompressThresholdDb()` in Vocabulary.cpp is the one mapping, shared by the
engine and by the testbench's explanation of it; `CompressCutDb()` in Config.h
is the one curve.

### Why not a hard cap

Because a hard cap puts everything above the threshold on *exactly* the
threshold, so a threshold set a few dB low replaces a spread of impacts with a
row of identical ones. This is not hypothetical - it is what the first version
of this section did. Measured on `Lennald_..._log_2` with a hard cap at -20 dB:

```
hard cap  -20 dB    4 peers within 1 dB of -26.0, then a 4.0 dB cliff
```

Four separate impacts inside one decibel of each other, where there had been an
11 dB spread. The tumble stops having a shape.

The same take through the compressor, sweeping the ratio at the same threshold:

```
ratio  1:1     1 peer  within 1 dB of  -6.0, then 11.0 dB   (off)
ratio  2:1     1 peer  within 1 dB of -16.0, then  5.5 dB
ratio  4:1     1 peer  within 1 dB of -21.0, then  2.8 dB
ratio  8:1     1 peer  within 1 dB of -23.5, then  1.4 dB
ratio 20:1     4 peers within 1 dB of -25.0, then  5.0 dB   (a cap again)
```

The peer count is the thing to read: it is how many events landed on top of each
other. It stays at one all the way to 8:1 - the loud events get closer together
but stay ordered and stay distinguishable - and only collapses at 20:1, where
the compressor has become the hard cap. That is the whole argument for a ratio,
and it is why `fRatio` tops out at 20 rather than at infinity.

**Why not compress the whole range instead of its top?** Because that already
exists, and it is global: `Intensity:fDynamicRangeDb`,
`PostIntensity:fExtraRangeDb`, `PostIntensity:fCurveExponent` and the two
`fSoftClipKnee`s all shrink the whole span. They move every cue, including the
quiet ones that were already right. The gap this section fills is per-class and
top-only - "taps and head cracks get too loud, everything else is fine" - and
that is a question the global controls cannot answer.

### Why not an absolute level

A threshold written as a rendered level - "hold anything over -6 dB", or a 0..1
post-render amplitude - is an **absolute** one, and an absolute threshold fights
every volume control downstream of it. Push `Mix:fMasterGainDb` up 6 dB to sit
against combat properly and the loudest hits do not move: the mod gets louder
everywhere except at the top, and the compression has silently tightened by
6 dB. You would have to re-tune it after every master-gain change, which is
exactly the kind of coupling this file exists to prevent.

A 0..1 amplitude would also be a fiction. The game applies its own distance
falloff and output model after us, so no cue has a full-scale reference to be a
fraction *of*, and linear amplitude has terrible resolution where the tuning
happens: 0.1 to 0.2 is 6 dB and 0.8 to 0.9 is 1 dB.

Measuring against `levelDb` instead makes the number **relative by
construction**. That scale has a real zero - `onsetGainDb` tops out at exactly
0 dB for the hardest contact the engine can hear, and every layer endpoint sits
at or under it - so `fTapDb = -20` reads as "start holding a tap once it comes
within 20 dB of the loudest thing this mod can produce". Every trim in the two
tables above then applies on top of the compressed value, master included.
Verified at threshold -20, ratio 4: the held events sit at -21, -15 and -9 dB at
master gains of 0, +6 and +12, and the 2.8 dB cliff between them does not move
at all. The mod gets louder; the shape does not change.

Because the input is bounded at 0 dB, a finite ratio still has an exact worst
case, which is the number to quote when the question is "how loud can this
get":

```
loudest possible = threshold + (0 - threshold) / ratio
```

Two consequences, both accepted:

- A slot trim is **not** compressed. `SlotGain:fHeadImpact = -10` for a hot wav
  lands on top of a head threshold of -6, so the cue renders at -16. The
  compressor is about the event, the trims are about the mix, and that is the
  same split the rest of this file is about.
- The cut moves the whole stack, so a low enough threshold with a high enough
  ratio pushes a composite's quietest layer under `Mix:fVoiceFloorDb` and it is
  dropped rather than mixed. That is the floor doing its job - a layer under it
  is not worth a voice however it got there - but it means hard holding thins a
  composite as well as levelling it.

It appears in `Cue::compressCutDb` so the two questions the level alone cannot
tell apart - "as loud as it wanted to be" and "as loud as it was allowed to be"
- are separable downstream. The testbench draws the second as a ghost tick above
the cue on the timeline, at the height the bar would have reached, with the gap
between the two being the compression; the cue table puts the figure in brackets
after the gain; and `EngineStats::compressedCues` counts them for the export's
funnel.

### A stale word: "voice"

The renderer merges a proposal's layers into **one** blob and starts **one**
`BSSoundHandle` per actor for it - `GameRenderer::Update` calls `MixComposite`
and logs "N layers into one M ms voice". So the game's voice count is one per
audible moment, not one per layer.

That ledger used to be per layer, which meant a four-layer composite spent four
slots on the one voice it became, and the budget could run out *inside* a stack
and drop the sub off it. It is now one booking per proposal, sized by the longest
layer and all-or-nothing, which is the only thing a mixed buffer can be. There is
no global ceiling at all - see 00-Design.md section 14 for the measurement that
retired it - so `dropped voice cap` in an export now only ever means one actor
had more than `kVoiceCapPerActor` moments overlapping at once, which across the
whole capture set never happens.

---

## Renames this convention implies

Four parameters are on the post-arbitration side but named `Gain`. They are all
harmless today - they are correct in behaviour, wrong in name - so this is a
readability fix, not a bug fix:

| now | should be |
|---|---|
| `SlotGain:f*` (section and 14 keys) | `SlotTrim:f*` |
| `Mix:fMasterGainDb` | `Mix:fMasterTrimDb` |
| `Player:fMasterGainDb` | `Player:fMasterTrimDb` |
| `PostIntensity:fExtraRangeDb` | keep - "range" is not a gain word and does not invite the confusion |

Renaming breaks existing ini files. `ParamDesc::legacySection` / `legacyKey` is
the answer taken when the lead rule moved to `[AirTime]`: the reader accepts the
old name, and the next save drops that line and writes the key out where it lives
now, tooltip and all. A rename costs a row in the schema, not a tuning session.

## Ceilings and floors

There is no per-slot maximum anywhere, before or after the line. The one
explicit ceiling is `AirTime:fHeadMaxLevelDb`, with `fBodyMaxLevelDb` beside it,
and both are deliberately narrow: each bounds a single contact, only while the
air-time rule is adding to it, and a default of 0 dB is the engine's own natural
ceiling rather than a new one. What otherwise exists:

| | after the line | before the line |
|---|---|---|
| **ceiling** | `MixParams::clipCeiling` (0.98 linear) - a soft clip on **one composite's own sum**, to stop four layers wrapping into a click. Not per-slot, not in the ini, and `Mix.h` says explicitly it "must not grow into" a master limiter. | implicit and hard: `Intensity()` clamps to `[0,1]`, so `onsetGainDb = -fDynamicRangeDb x (1 - intensity)` tops out at exactly **0 dB**. `Intensity:fSoftClipKnee` is the soft approach to it. |
| **floor** | `Mix:fVoiceFloorDb` - a **discard**, not a lift. Under it the layer is dropped; if a proposal loses every layer that way the arbitrator rolls its budget back rather than spending a burst grain on silence. | `Ingest:fMinImpactSpeed` - also a discard. |

The testbench's `bLimiter` checkbox is a monitoring aid, labelled *NOT
SHIPPABLE* in its own tooltip: the game has no bus to limit. Its readout exists
to keep the engine's unlimited summing honest, not to fix it.

Two hazards if either is added:

- **A ceiling flattens dynamics.** Every cue above it renders identically, so a
  400 u/s and a 900 u/s head hit become the same sound. If the goal is "this
  slot is too loud", a `Trim` shifts the range and keeps its shape; a ceiling
  removes the shape.
- **A pre-arbitration ceiling converts loudness into a coin flip.** Two clamped
  proposals tie on `levelDb`, and the sort breaks ties on `sourceSeq` - which is
  the order limbs happened to be reported in. A tie in the sort is not a
  neutral outcome, it is an arbitrary one.

A *lifting* floor after the line is the one to be most careful with. The whole
suppression design assumes most contacts are inaudible - the reduction target is
about 10:1. Lifting quiet cues up to a floor inverts that and fills the mix back
in with exactly the grains the arbitrator spent its rules removing.

## The change this convention is waiting on

`Proposal::priorityDb`, defaulting to `levelDb`, so the split is enforced by the
type system rather than by discipline:

- **priority** - the sort, the rate-cap override, the chain merge, `lastOnsetDb`,
  `chainLastDb`
- **level** - masking and `maskCeilingDb`, which are audibility questions and
  should read the *predicted rendered* level, trims included, rather than the
  pre-trim number they read now

Until that lands, rule 4 is the whole defence: when in doubt, `Trim`.
