# Armour — a proposal

*Built on 2026-08-24, except the assets. Two axes, one config section, and a
testbench control that makes both auditionable on takes recorded in the wrong
clothes. What remains is recording the wavs — the code is silent and inert
until somebody does.*

| Doc | What it is |
|---|---|
| [00-Design.md](00-Design.md) | The design. §12 is the asset list this grows |
| [01-Architecture.md](01-Architecture.md) | The pipeline. §7.1 is the table that decides where each half of this lands |
| [config.md](config.md) | What a decibel means, and which side of arbitration it lands on |
| **Proposal-Armor.md** (this) | What armour adds, what it costs in files, and the order to build it |

Spelling, once: identifiers and ini keys are **`armor`**, to match `[Armor]`,
`armor_heavy` and the game's own `ArmorHeavy` material. Prose is **armour**, to
match the rest of the docs.

> **Built.** Phases −1 through 6 of §9 are implemented, and the line numbers
> below are from the tree as it stood *before* the build, so read them as "the
> thing named". What was not built is the two asset phases, 3 and 7: the four
> `armor_*` slots ship declared and empty, so the mod sounds exactly as it did
> until somebody records a wav.
>
> **The acceptance test passed.** The whole corpus through `rds-verify`, before
> and after, with the seed pinned: every per-take line and all 72 checks are
> byte-identical. The only differences in the whole output are four new bank
> lines saying the armour slots are declared and unfilled, and the schema key
> count moving 330 → 368.
>
> Three places where the tree changed the argument rather than the coordinates
> are marked where they occur: §3.3 (the `levelDb` rule is now enforced by a
> mechanism), §3.7 (damage left the head strategy) and §8.1 (the rework paid the
> slot-table cost once already).
>
> **What differs from the plan as written**, all of it recorded at the point it
> comes up below: the free levers ship at **0** rather than at the recommended
> voicing (§3.6), the airborne rise carries a class rather than a layer
> (§3.4), the slide's armour is its **own loop voice** rather than a second
> layer (§3.2). Conditions were ini-only at first and are not any more: the
> sfx panel grew `+ variant` and the grid behind it on the same day (§4.6).

---

## 1. The one-paragraph version

Armour becomes a **second colour axis beside the surface**, built the same way
and for the same reason: colour is *additive*, so four armour classes cost four
files rather than multiplying every layer in the composite by four. On top of
that, any slot may carry **conditional variants** — a file tagged "stone" or
"heavy" or both, which wins over the plain files when the contact matches and is
invisible when it does not. The first axis makes plate sound like plate
everywhere; the second buys a specific, unmissable sound for the two or three
combinations that actually carry a moment. Both are strictly additive: with no
armour files installed and no conditions set, every cue the mod emits today is
byte-identical.

---

## 2. What is already built

More than I expected, and it changes the shape of the work. The armour axis is
**plumbed end to end and ignored at the last step**.

| Piece | Where | State |
|---|---|---|
| The four classes | `Coverage {kBare, kCloth, kLight, kHeavy}`, `Types.h:137` | Exactly the four you asked for — naked, default, light, heavy |
| Read off the actor | `CoverageForSite`, `plugin/src/GameFeed.cpp:130` | Per body site, through the worn biped slot, with a fallback to the cuirass |
| Site → biped slot | `SlotForSite`, `plugin/src/GameFeed.cpp:101` | head/hands/forearms/feet/calves have their own; torso, upper arm and thigh use the body slot |
| Carried per limb | `LimbInfo::coverage`, `Feed.h:95` | Filled at `GameFeed.cpp:518`, on every ragdoll attach |
| In the recordings | `CoverageFrom`, `Recording.cpp:328` | Read from the take's `coverage:` map; written back by `Capture.cpp:200` |
| Over the link | `core/src/Link.cpp:512` writes, `:565` reads | Field 4 of the positional limb row, live from a running game. **Two files are called `Link.cpp`** — the testbench's has no limb table at all, so reach for the `core/` one |
| Onto the contact | `Engine.cpp:2378` | `contact.coverage = limb != nullptr ? limb->coverage : Coverage::kBare` |
| Onto the proposal | `Engine.cpp:649` | `proposal.coverage = contact.coverage` |
| Onto the slide | `Engine.cpp:2862` | `actor.slideCoverage = contact.coverage` — new with the rework, and it is exactly what §3.2's slide row needs |
| Into the bank | `Engine.cpp:4021` | Handed to `SoundBank::Resolve` as an argument… |
| …and dropped | `SlotManifest.cpp:662, 691` | `(void)coverage;` — twice, once in each picker |

So the data arrives at the resolver intact and the resolver throws it away. Both
axes are about **what happens after that argument arrives**, which is why this is
a smaller job than it sounds.

Three things are missing rather than unused, and each is a line item below:

- **No armour class for an actor-level cue.** The bed and the airborne whoosh
  have no contact and therefore no limb, so they have no coverage to read.
- **`Cue` carries `surface` but not `coverage`** (`Cue.h`), so the timeline can
  never explain why the plate variant played.
- **`SlotAxis {kSurface, kCoverage, kSize}`** is declared in `SlotManifest.h:70`
  and read by nothing — `SlotDesc` has no field for it. It is the first draft of
  axis 2 and it is superseded by it. Delete it.

And one thing the slide rework *added* that this proposal gets for free:
**`SlotDesc::fallback`**. A slot may now name another slot to play when it has no
file of its own, which is what makes `scrape_body_stone` a file drop rather than
a code change. The armour skins do not want it — they have no base skin to fall
back *to*, so all four keep `fallback = kCount` and rely on `expectedVariants = 0`
instead — but it is the slot-level twin of §4.2's "a condition is a preference,
never a mute", and the two rules should be written to read the same way.

---

## 3. Axis 1 — the armour skin

### 3.1 Four slots, and why they are empty by default

Four new slots, alongside the three `surf_*` ones:

| Slot | Class | Character |
|---|---|---|
| `armor_heavy` | heavy armour | Plate rattle. Metallic, short, no pitched ring — the clank *around* the impact, not a bell |
| `armor_light` | light armour | Leather creak with a small buckle jingle riding on it |
| `armor_cloth` | clothing, and anything unresolved | A soft cloth thump. Deliberately close to nothing |
| `armor_bare` | nothing equipped | A flat skin slap. Wet-ish, no snap |

**Every one of them ships with `expectedVariants = 0`**, which is the mechanism
that makes "additive" a fact rather than a discipline.
`SoundBank::Resolve` computes its candidate count as

```cpp
files.variants.empty() ? desc.expectedVariants : files.variants.size()
```

so a slot with no files and no expected variants returns `false`, the layer is
skipped silently, and the composite is exactly the four layers it is today. That
is the same door `grunt_impact` and `scream_big` already sit behind, and
`rds-verify` already prints "declared and unfilled" for it rather than failing.
Drop `armor_heavy_01.wav` into the pack — or name it on the slot in the SFX panel
— and it starts playing. Take it away and the mod goes back.

Do **not** give these slots a procedural stand-in. The synthesiser would make the
feature audible before any asset exists, which sounds like a win and is not: it
would mean an install with no armour files sounds different from today, which is
the one thing you asked this feature never to do.

### 3.2 Where the layer is added

Four places, mirroring the surface skin exactly:

| Cue | Coverage it uses | Config |
|---|---|---|
| The composite (`ImpactCompositeStrategy::Propose`) | the contact limb's | `bEnabled`, `fOffsetMs`, `fGainAtMinDb`/`fGainAtMaxDb` |
| The tap (`ProposeTap`) | the contact limb's | `bOnTaps`, `fTapOffsetMs`, the tap ramp, `fTapHeadroomDb` |
| The scrape loop (`ScrapeLoopStrategy`) | `CrashState::slideCoverage`, already captured on the contact that opened the slide | `bOnSlide`, `fSlideGainDb` |
| The airborne rise (`MotionFoleyStrategy`) | the actor's, see §3.4 | `iActorClassSource` |

Not the foley bed, because there is no longer one — see §12. Plate rattling
during a flight is the one actor-level cue left that wants an armour class, and
it is a good one: it is the only sound in the mod that says *armoured* before
anything has been hit.

The slide row is cheaper than it was when this was written. The rework gave the
slide its own `[Slide]` section and its own surface-coloured slots, and it
captures `slideCoverage` on the contact that opens the slide (`Engine.cpp:2862`),
so the armour half is a gain and a switch in `[Armor]` over a class that is
already sitting there.

`kMaxLayers` is 6 and the composite uses 4 today — transient, surface skin, body,
sub — so the armour skin is layer 5 and the tap's is layer 3. No headroom
problem, but it is now one spare rather than two, and a sixth colour axis would
need the array widened.

The offset should sit **between the transient and the body** — around +12 ms,
just after the surface at +8. The reason is what the layer is: metal moving
because something stopped is a consequence of the contact, not the contact
itself, so it arrives after the strike and before the mass. Land it at 0 and it
fuses with the transient into one brighter click, which is the failure mode to
listen for.

### 3.3 The fixed rule: colour never changes rank

`Proposal::levelDb` is a loudness *and* a rank — it is what the arbitrator sorts
by — so a layer allowed into the `max()` that computes it can put a scuff ahead
of a real impact.

**The rework settled this, and armour inherits the answer rather than arguing it
again.** When this section was first written the surface skin was inconsistent:
folded into `levelDb` on the composite, deliberately excluded on the tap. It is
excluded on both paths now. The composite builds every layer through one lambda
that takes a `ranks` flag (`Engine.cpp:1084`), the surface skin passes `false`,
and the comment above it states the general rule outright — *a floor is what a
contact hit, not how big it was, and a rank assembled from it is `config.md`'s
confusion in a new place.*

The same comment records why that was worth fixing while it changes nothing
audible: body ramps −8..−2 and surface −12..−6 over the same span, so the skin
sits exactly 4 dB under the body at every intensity and can never win the
`max()`. That is "arithmetic nobody wrote down, and it stops being true the
moment either pair is re-voiced."

**So the armour skin passes `ranks = false` too**, and is clamped to
`proposal.levelDb + fHeadroomDb` (negative, default −3). This is now one more
caller of an existing mechanism rather than a new rule to remember. Two reasons
it is the right call for this layer in particular:

- It is the honest reading of `config.md`'s own rule — *when in doubt, `Trim`* —
  and of 01 §7.1's "how big a contact is built and whether it is heard are
  different questions".
- A plate rattle is the layer most likely to be *long and loud* relative to what
  it is colouring, because armour keeps moving after the body has stopped. That
  is exactly the layer you do not want deciding which contact wins a hero slot.

There is no config switch for this. A flag that moves a parameter across the
arbitration line is the pitfall named at the bottom of 01 §7.4, and this is
precisely that flag.

### 3.4 Per-limb, and the two knobs over it

Per-limb is the default and it already works: `contact.coverage` is the class of
the limb that hit. Heavy boots and nothing else means feet get the plate rattle
and the rest get nothing, with no new mechanism.

Two switches over how the class is decided, because the mapping is a judgement
call and you should be able to overrule it:

- **`bPerLimb`** (default on). Off, every contact reads the actor's body-slot
  class instead of its own limb's — a heavy cuirass makes the whole actor clank.
  This is the fast A/B for whether per-limb is worth the complexity at all.
- **`bInheritFromBody`** (default on). This is `CoverageForSite`'s existing
  hardcoded behaviour made switchable: a site with nothing in its own biped slot
  takes the cuirass's class rather than reporting bare. It is right for the
  common case — a cuirass with no separate gauntlets does not mean bare forearms
  — and wrong for the case you named, heavy boots on an otherwise naked body,
  where you want the rest to read naked. Turn it off and an empty slot means
  bare.

The body sound is not a special case and needs no rule: the torso maps to
`Slot::kBody` in `SlotForSite` already, so a heavy cuirass gives torso contacts
the heavy skin.

**The airborne rise does need a rule**, because it has no contact.
`iActorClassSource`, three values: the **body slot** (default — a cuirass is what
you hear moving), the **last contact's limb**, or **off**. This needs one new
field, `CrashState::bodyCoverage`, resolved once from the profile's torso limb
when the ragdoll attaches.

### 3.5 One live-side bug to fix while we are here

`CoverageForSite` reports any worn `TESObjectARMO` that is neither heavy nor
light as `kCloth`. TNG's skin is a real `TESObjectARMO` occupying five slots, so
a naked TNG actor currently reads as **clothed on five sites**. The recording
loader already knows this — `CoverageFrom` implements the rule "nameless and
weightless is bare, not clothing" at `Recording.cpp:340`, straight out of the
data dictionary — and the live path does not. Port the same test into `CoverageForSite`, behind
`bBareIsNaked` (default on).

Without this fix the `armor_bare` slot would essentially never fire on a modded
body, which is the half of the feature you are most likely to notice missing.

### 3.6 The free half: two levers that cost no files

Both default to neutral, so they cost nothing until turned.

**Per-class pitch bias.** Pitch is continuous and free in this engine, and the
design already leans on it (00 §7: "it beats doubling the bank"). Four floats —
`fBarePitchSemis`, `fClothPitchSemis`, `fLightPitchSemis`, `fHeavyPitchSemis` —
added to the composite's existing intensity bias before the same clamp. Heavy at
−1.0 and bare at +0.5 makes an armoured body read heavier and a naked one
lighter **with no armour assets installed at all**. This is the cheapest
dynamic in the whole proposal and I would ship it enabled with those defaults.

**Per-class composite trim.** Four more floats, applied at Trim to the whole
stack. Heavy armour genuinely is louder; this says so without touching what was
chosen. Default 0.

### 3.7 The head accent stays out of this

**No armour skin is layered on `head_impact`, and no armour rule is added inside
`HeadImpactStrategy`.** That strategy is still the most heavily conditioned code
in the mod — `ClassifyHead` (`Engine.cpp:762`), the hero floor relief, and gates
calibrated against measured head contacts — and threading a fourth axis through
it would buy a helmet at the cost of making the one strategy nobody wants to
touch harder to reason about.

**The rework made this argument for me, about a different gate.** Damage used to
be decided inside `HeadImpactStrategy` and is `DamageStrategy`'s now
(`Engine.cpp:1252`), on the stated ground that "how hard the skull landed and
whether it earns an accent are different questions, and a gate raised for voicing
reasons should not quietly take the consequences with it." That is precisely the
shape of this section. It also means an armour toggle at the top of the accent is
now *structurally* unable to reach the crunch — they are no longer the same
strategy — where before it was only a promise that it would not.

Instead, one thing, from outside it:

```ini
bNoHeadOnLight = 0   ; a light-armoured head gets no head accent
bNoHeadOnHeavy = 0   ; a heavy-armoured head gets no head accent
```

Two independent switches, both off by default, evaluated as a **single early
return at the top of `HeadImpactStrategy::Propose`** — one `if`, reading
`contact.coverage`, before any classification runs. It cannot reach the gates, it
cannot reach the crunch and gore thresholds, and it cannot change what those
mean; it decides whether the strategy runs at all, and that is the only shape of
armour rule that does not muddy the head code.

Two things worth being explicit about:

- **This does not silence the contact.** The composite still fires, the surface
  skin still fires, the armour skin still fires, the crunch still fires through
  `DamageStrategy`'s own door — a separate strategy since the rework, so this is
  a fact about the code and not a discipline. What goes away is the *accent* — the dull
  skull thud with the ring on it, which is the layer that reads as bare skull and
  is exactly wrong under a helm. A head in plate landing hard is still loud, it
  just stops sounding like a melon.
- **It is a strategy-level gate, not a phase-level one**, so it does not break
  00-Design's "no state suppresses a contact" rule. That rule is about the motion
  axis, which cannot judge a contact. This one reads the contact in front of it.

If you later want a helmet to *sound* like something rather than merely stop
sounding like a skull, that is axis 2's job — a `head_impact` variant tagged
`heavy` — and it needs no code at all.

---

## 4. Axis 2 — conditional variants

### 4.1 What a condition is

A **variant** — one file on one slot — may carry a condition of two independent
halves, each of which is a specific value or `any`:

```
surface:  any | soft | wood | stone | metal | water | body
armor:    any | bare | cloth | light | heavy
```

`any/any` is what every file carries today, so an unconditioned pack behaves
identically. This is per **slot and variant**, not per sound effect — the same
wav can sit on two slots with two different conditions, and the condition
travels with the *placement*.

### 4.2 The specificity ladder

`Resolve` narrows the candidate set before either picker runs:

1. Drop every variant whose specific half **mismatches** the contact. A file
   tagged `stone` is not a candidate on wood, ever.
2. Of what is left, keep the **most specific non-empty tier**:
   both halves specific → one half specific → `any/any`.
3. Pick within that tier with the existing stable hash or shuffle bag, unchanged.

So `imp_body` carrying `[generic_a, generic_b, plate_on_stone(stone/heavy)]`
plays `plate_on_stone` for a heavy limb landing on flagstone and picks between
the two generics for everything else. Two variants tagged `(stone, heavy)` and
the picker chooses between them normally — a condition narrows the set, it does
not collapse it to one.

**A condition is a preference, never a mute.** If step 2 finds every tier empty —
a slot whose only file is tagged `stone`, on wood — the slot falls back to the
full set ignoring conditions rather than going silent. Muting is what `Muted`
is for, and it is stored separately for exactly this reason. Without this rule,
tagging the only file on a slot would silently delete that layer from most of the
game, which is the bug nobody would find.

### 4.3 What this does not break

**`SoundBank::Get(slot, variant)` stays a pure lookup.** This is the constraint
that could have killed the whole idea and does not. A cue carries `(slot,
variant)` and the renderer turns that back into a file through `Get`, which must
not consult the shuffle bag — otherwise the audio is not the cue list the
arbitrator emitted. A condition changes only *which indices are candidates*
inside `Resolve`; the variant index is still a position in `files`, and `Get`
never learns conditions exist. No change to `Cue`, to the renderer, or to the
recorded-take round trip.

`stableVariants` also survives: the hash is over `(seed, token, slot)` and the
modulus is the tier size, so the same contact under the same config picks the
same file, and an A/B still compares two configs rather than two dice rolls.

### 4.4 Storage

Conditions are **one per placement**, in a third list parallel to `files`:

```cpp
// on SlotAssignment, alongside `files` and `muted`
std::vector<VariantCondition> conditions;
```

> **Amended.** This shipped keyed by filename — by name rather than by index for
> the reason `muted` gives itself, that re-ordering `files` would move a stored
> index onto a different sound. That was wrong, and the way it was wrong is the
> one shape people reach for first: put a file on a slot twice, plain and tagged
> — "keep it in the set, and make it *the* one on stone" — and the tag lands on
> both copies, because both copies are that name. The plain one disappears from
> the general set, the panel lists the file twice under the same heading, and
> there is no way to untag half of it. Nothing re-orders `files` in the end
> (the panel groups the rows for reading and keeps every index), the runtime was
> positional all along (`SoundBank::SlotFiles::conditions`), and the editor
> model is positional now too. `muted` stays by name: a mute is about the sound,
> and there is nothing a per-placement one could say that removing the placement
> does not.

The ini gains one key per slot, comma-joined like the other two:

```ini
[imp_body]
Sfx        = body_a.wav, body_b.wav, plate_stone.wav
Muted      =
Conditions = plate_stone.wav : stone / heavy
Looping    = 0
```

A name listed twice is disambiguated with a 1-based `#N` in `Sfx` order, and
only where it has to be — so a slot without duplicates writes what it always
wrote, and a bare name still means the first placement of it:

```ini
Sfx        = body_a.wav, body_b.wav, body_a.wav
Conditions = body_a.wav#2 : stone / heavy
```

An unrecognised key is already logged at debug and left alone, so an
`RagdollSounds_SFX.ini` written by this version loads in the shipped one and
vice versa.

The devlink carries them too, as `imp_body.cond=plate_stone.wav>stone>heavy`
beside the `.mute` and `.loop` lines it already had. Not an afterthought: the
panel pushes an assignment table to a running game on the frame it is edited,
and a tag that the wire dropped would work in the testbench and be silently
absent in the game — which is the failure this whole file keeps trying to
design out.

### 4.5 What it buys, beyond armour

`SurfaceSlot` maps `kMetal`, `kWater` and `kBody` onto "the nearest thing that
exists" because they have no `surf_*` slot of their own. Conditions run on
`SurfaceClass` directly, not on the skin slot — so **a condition gives metal,
water and body-on-body a voice without adding three more surface slots**. One
`imp_body` variant tagged `water` is the entire splash feature.

It also covers armoured crunches for free: a `crunch_gran` variant tagged `heavy`
is plate buckling instead of bone, on the same gate, with no strategy change.

### 4.6 The panel

The sfx panel's slot row has **`+ variant`** beside `+ add`. It opens a grid of
every surface against every armour class; clicking a cell opens the ordinary
library picker, and the file chosen there lands on the slot already tagged.

The order is the point. What a recording is *for* is the thing you know before
you go looking for it, so the grid comes first and the library second — asking
afterwards would mean picking a sound and then being interrogated about it. The
picker is the same window as always, so the A/B against what the slot plays now
still works; only the landing differs.

Below, the slot's files are drawn in **groups**: the plain ones first, then one
block per condition under a muted line saying what it is for — `- only on
stone, in heavy armour -`. The grouping is a way of reading the list and never a
way of re-ordering it: every row keeps the index it has in `files`, because that
index is the variant a recorded cue carries. Every row also has a small button
showing its tag, or `for...` when it has none, which opens the same grid — "this
one is really the stone take" is a thing you find out by listening, long after
the file was assigned.

Two things the panel says that the ini cannot. A group whose switch is off in
`[Slots]` is drawn in the dirty colour and says `ignored, see [Slots]`, because
a grid full of overrides and a switch off looks exactly like a grid full of
overrides that work. And the tag follows the sound: `change` moves it onto the
new file (the *position* is what is for stone), while `x` and a library delete
take it away with the recording, on the same argument `Unmute` already makes.

---

## 5. The file budget

This is the part you actually asked about: maximum dynamics, minimum sounds.

**The whole feature at full spread is 12 files, and 4 of them make it audible.**

| Tier | Files | What it buys |
|---|---|---|
| **First taste** | 4 | `armor_heavy` ×2, `armor_light` ×2. Plate and leather gain a voice; clothed and naked are bit-identical to today |
| **Full skin set** | +4 | `armor_cloth` ×2, `armor_bare` ×2. All four classes distinct |
| **The overrides** | +4 | The four combinations below |
| **The free levers** | +0 | Per-class pitch bias and per-class trim (§3.6) |

The four overrides worth authoring, in the order I would author them:

| Slot | Condition | Why this one |
|---|---|---|
| `imp_body` | `stone / heavy` | The armoured faceplant on flagstone. The single most cinematic combination in the game, and the one where a generic thud is most obviously wrong |
| `imp_body` | `any / bare` | Naked flesh on anything. The one an unarmoured hit most needs, and it works on every surface |
| `head_impact` | `any / heavy` | A helmet. The head accent's "slight ring" is already what makes it read as a head; a helmet is that ring doubled |
| `crunch_gran` | `any / heavy` | Plate buckling instead of bone snapping |

**Why this is cheap, stated as the rule:** colour is *additive*, so N classes cost
N files. An armour *axis* — a heavy variant of every layer — would cost
23 slots × 4 classes × 2 variants = **184 files** and would not sound better,
because the thing that changes when you put plate on is not the body's mass, it
is that something metallic moved. That is a layer, not a timbre shift on every
existing layer. It is the same argument `SurfaceConfig`'s own header makes about
the floor, and it is the reason both axes fit in one section.

Add to that the multiplier the design already has: four layers × three variants
is dozens of distinct composites, and a fifth layer at four classes multiplies
that again for free.

---

## 6. The testbench: pretending

Every take in the corpus was recorded on one surface with one wardrobe, and three
of the four armour classes have no capture at all. So the feature is unhearable
without a way to lie to the engine.

**A "simulate" row directly under the transport**, next to the monitor level and
the limiter — the same place the other "this is the testbench, not the game"
controls live:

```
simulate    surface [as recorded ▾]    armour [as recorded ▾]  [per-limb ▾]      ⚠ pretending
```

- **as recorded** on both is the default and is a true replay.
- **surface** forces every **world** contact to a class. Self- and body-contacts
  are left alone — `Ingest` routes those by `otherLimb`, and forcing a
  self-collision onto stone would move it into a different branch and change
  behaviour that has nothing to do with the surface.
- **armour** forces every limb's class. The **per-limb** expander gives one combo
  per coverage site — head, hands, forearms, feet, calves, body — each "as
  recorded" by default, so *heavy boots, naked otherwise* is two clicks. That is
  the case that tests §3.4, and it cannot be tested any other way.

**Where it lives, and why not in the engine.** It goes in `OfflineOptions`, not in
`AlgorithmConfig` and not in a debug branch inside `Engine`:

```cpp
struct OfflineOptions {
    // …existing…
    /// Pretend the take happened somewhere else, in something else.
    /// kCount / kAsRecorded means replay it as it was.
    SurfaceClass surfaceAs{SurfaceClass::kCount};
    Coverage     coverageAs[kCoverageSiteCount];  // per site, each "as recorded"
};
```

`RunOffline` applies both **before the engine sees anything**: the coverage
override by rewriting `ActorProfile::limbs[].coverage` after load, the surface
override by rewriting each world contact's `otherMaterial` to a representative
`MATERIAL_ID` for the class (one new helper, `RepresentativeMaterial`, beside
`SurfaceFromMaterial`). The engine stays untouched and cannot tell it is being
lied to — which is the whole point, because a debug branch inside `Ingest` is
something you would eventually tune against.

Three properties it must have:

- **Session-only.** Never written to a config or a UI pref. Coming back tomorrow
  to a testbench that is quietly pretending, and tuning under it, is the worst
  outcome this control can have.
- **Shared by both A/B sides.** It applies to the take, not to a config, so an
  A/B compares two configs in the same pretend world rather than two worlds.
- **Visibly on.** The `⚠ pretending` chip stays lit whenever either is set, in
  the same spirit as the limiter's `NOT SHIPPABLE` note.

Both dropdowns set `dirty` on both sides, so a change re-runs and re-mixes at the
current play position like every other edit.

---

## 7. `[Armor]` — the config section

File order is ini order and slider order, so this is the panel top to bottom.
Placed immediately after `[Surfaces]` and before `[HeadImpact]`, because it is
the same kind of section and the two are read together.

```ini
[Armor]
bEnabled                = 1     ; master. Off, no cue gets an armour skin

; -- how the class is decided ------------------------------------------------
bPerLimb                = 1     ; off: every contact uses the actor's body-slot class
bInheritFromBody        = 1     ; a site with an empty slot takes the cuirass, not bare
bBareIsNaked            = 1     ; a nameless, weightless ARMO (TNG skin) is bare, not cloth
iActorClassSource       = 0     ; the airborne rise: 0 body slot, 1 last contact, 2 off

; -- the head accent (§3.7) --------------------------------------------------
bNoHeadOnLight          = 0     ; a light-armoured head gets no head accent
bNoHeadOnHeavy          = 0     ; a heavy-armoured head gets no head accent

; -- on the composite --------------------------------------------------------
fOffsetMs               = 12.0  ; after the transient, before the body
fGainAtMinDb            = -14.0
fGainAtMaxDb            = -7.0
fHeadroomDb             = -3.0  ; held under the stack. Never enters the rank

; -- on the tap --------------------------------------------------------------
bOnTaps                 = 1
fTapOffsetMs            = 5.0
fTapGainAtMinDb         = -13.0
fTapGainAtMaxDb         = -9.0
fTapHeadroomDb          = -3.0

; -- on the slide ------------------------------------------------------------
bOnSlide                = 1     ; armour rides the scrape loop
fSlideGainDb            = -10.0

; -- the free levers ---------------------------------------------------------
fBarePitchSemis         =  0.5
fClothPitchSemis        =  0.0
fLightPitchSemis        = -0.3
fHeavyPitchSemis        = -1.0
fBareCompositeTrimDb    =  0.0
fClothCompositeTrimDb   =  0.0
fLightCompositeTrimDb   =  0.0
fHeavyCompositeTrimDb   =  0.0

; -- level, after the cue has been chosen ------------------------------------
fTrimDb                 =  0.0  ; the role trim, all four skins together
fBareTrimDb             =  0.0
fClothTrimDb            =  0.0
fLightTrimDb            =  0.0
fHeavyTrimDb            =  0.0

; -- per-skin mutes ----------------------------------------------------------
bBare                   = 1
bCloth                  = 1
bLight                  = 1
bHeavy                  = 1
```

**Axis 2's switches go in `[Slots]`, not here**, because a condition can be
surface-only and has nothing to do with armour. `[Slots]` is the section that
already owns `bShuffleBag`, `bStableVariants` and `iRngSeed` — resolution policy,
which is exactly what a condition is:

```ini
[Slots]
bConditionalVariants    = 1     ; master for the whole condition system
bSurfaceConditions      = 1     ; honour the surface half of a condition
bArmorConditions        = 1     ; honour the armour half
```

Those last two exist for one question: *is the stone-specific set actually doing
anything?* Turn one off and the ladder collapses on that axis while the other
stays, which is the only clean way to hear which half of a condition earned its
file. Say the word and I will move all three into `[Armor]` instead.

---

## 8. Refactorings

You asked for them, and three of these are worth doing **before** the feature
rather than after.

### 8.1 One table for what a slot is — do this first

Adding four slots today means touching four hand-written switches that must all
agree — `LayerMute` (`Config.cpp:5`), `RoleTrimDb` (`Engine.cpp:2150`),
`SlotTrimDb` (`Engine.cpp:2190`) and `LayerAudible` (`Engine.cpp:3896`) — plus
`kImpactOrder` (`AppSfx.cpp:41`), which at least has a `static_assert` guarding
it. Miss one of the four and you get a slot with no mute, or a trim slider over
silence, and nothing tells you.

**The rework just paid this bill, and the receipt is legible.** The six new
scrape slots are grouped by hand in all four switches — `kScrapeBodyWood` and
`kScrapeBodyStone` fall through to `scrapeLoop`'s mute and trim,
`kScrapeLimbWood` and `kScrapeLimbStone` to `scrapeLimb`'s — and each grouping
carries a comment arguing the same point in different words: *a surface-coloured
scrape is the same layer on a different floor, so it answers to the mute of the
loop it is a variant of.* One fact about a slot, written out four times across
three files.

The arithmetic is the other half of the argument. There are **23 slots**, but
only **14 `SlotGain` rows** and **12 `Layers` rows** in the schema. That gap is
not an oversight — it is the grouping above, plus the two declared-and-unfilled
voice slots that deliberately have neither, plus the surface skins whose mute and
trim live in `[Surfaces]` instead. Three different reasons a slot can be missing
from a switch, and nothing in the code distinguishes them: the only way to learn
which applies is to read all four switches and infer it.

**Give `SlotDesc` a `family`** — impact / surface / armour / grain / loop /
accent / voice — and a `mutesWith` / `trimsWith` pair defaulting to the slot
itself. Derive the role trim from `family`, and let the two `*With` fields carry
the fall-through the four switches currently spell out in prose. Adding a slot
becomes one row, and "why does this slot have no mute?" becomes a field you can
read. This is the highest-value change in the document and it is what makes §3
four slots rather than four slots and twenty edits.

### 8.2 Factor the shared half of a colour section

`SurfaceConfig` and the `[Armor]` block above are the same shape: enabled,
offset, ramp, headroom, a tap block, a role trim. Factor the common half into a
`ColourLayerConfig` and let both embed it — `surfaces.skin.offsetMs`,
`armor.skin.offsetMs`. The per-class trims and mutes stay per-section, because
they differ in count and in name.

Nested members already work in the schema (`ingest.blowupDisagreeFrac`), and
**no ini migration is needed**: key names are written literally in
`ConfigSchema.cpp` and only the C++ member path moves. About fifteen rows change
and roughly a hundred lines of `Config.h` go away.

### 8.3 One `AddSkin` in `Engine.cpp`

Half of this landed with the rework. The composite now builds every layer through
one `layer(slot, offsetMs, minDb, maxDb, reason, ranks)` lambda
(`Engine.cpp:1084`), so its skin is a single call with `ranks = false`. The tap's
skin is still a hand-rolled block (`Engine.cpp:1155`) repeating the gain lerp, the
headroom clamp and the pitch scatter inline.

So it is two shapes rather than two duplicates, and adding armour would make it
four. Lift the lambda to a free function both paths call —
`AddSkin(proposal, cfgHalf, slot, intensity, isTap)` — and the "never enters
`levelDb`, always headroom-clamped" rule holds by construction instead of by
remembering. The tap's block already carries that rule in a comment; the point of
the helper is that armour should not need a third copy of the comment.

### 8.4 `Cue` gains `coverage`

One field beside `surface`, filled in `Emit`. The cue table and the timeline
tooltip can then say *why* the plate variant played, which for a conditional
resolution is otherwise unanswerable from the outside. Cheap, and the first thing
you will want the moment a condition fires when you did not expect it.

### 8.5 Delete `SlotAxis`

Declared in `SlotManifest.h:70`, read by nothing, and superseded by §4. The two
`(void)coverage; (void)site;` blocks that reference it in prose go with it.

---

## 9. Build order

Each phase is independently shippable and independently silent if you stop there.

| # | What | Why here |
|---|---|---|
| **−1** | ~~§12 remove `foley_cloth` and the bed~~ | **Done, 2026-08-24.** The slot, the bed, `CueReason::kFoleyBed`, eight schema keys and the deployed `[foley_cloth]` section are gone. Engine, verifier and testbench all build; `rds-verify` totals unchanged against the measured baseline |
| **0** | ~~§8.1 slot-family table, §8.5 delete `SlotAxis`, §8.4 `Cue::coverage`~~ | **Done.** `SlotDesc` gained `family`, `mutesWith` and `trimsWith`; the four switches read them; `SlotAxis` gone; `Cue::coverage` added and compared in `SameCue` |
| **1** | ~~The four slots, `[Armor]`, the composite + tap layer, §8.3 `AddSkin`~~ | **Done**, and verifiably a no-op — see the note at the top. §8.2 (`ColourLayerConfig`) was **not** done: `[Armor]` duplicates `SurfaceConfig`'s shape rather than sharing it |
| **2** | ~~The testbench simulate row (§6), both halves~~ | **Done.** `OfflineOptions` carries it and a `PretendFeed` applies it, so the engine cannot tell it is being lied to |
| **3** | First-taste assets: `armor_heavy` ×2, `armor_light` ×2 | **Not done — this is what is left.** Everything above is waiting on it |
| **4** | ~~Per-limb switches (§3.4), `bBareIsNaked` (§3.5), the slide layer~~ | **Done.** The TNG rule is in `CoverageForSite` now, so the live path and the recording loader finally agree about what a stripped body is wearing |
| **5** | ~~The free levers (§3.6)~~ | **Done, but shipped neutral.** All eight default to 0 so the acceptance test could hold; §3.6's recommended voicing is one edit away |
| **6** | ~~Conditional variants (§4) + the `[Slots]` switches~~ | **Done**, engine, ini and panel (§4.6). `rds-verify`'s `sfx cond` check covers six properties now, including that an unsatisfiable condition falls back rather than silencing, and that a tag survives the devlink |
| **7** | The four override assets (§5) and the remaining skins | **Not done.** Same reason as phase 3 |

Phase 1's acceptance test is a good one and worth writing down: **run the whole
corpus through `rds-verify` before and after and diff the cue lists.** They must
be identical. If they are not, "additive" is not true and something is resolving
where it should have been skipped.

---

## 10. The checklist, and what it costs to forget

Things that must move together. Most of these are the reason §8.1 comes first.

- `SlotId` gains four; `Slots()` gains four rows with **`expectedVariants = 0`**
- `kImpactOrder` in `AppSfx.cpp` — guarded by a `static_assert`, so this one
  cannot be forgotten
- `LayerMute`, `RoleTrimDb`, `SlotTrimDb`, `LayerAudible` — **not** guarded by
  anything today, which is §8.1's whole argument
- `CueReason` gains `kArmorSkin`; `ToString` and the timeline's colour table
  follow
- `Config.h` and `ConfigSchema.cpp` **in lockstep and in file order** — that order
  is the ini's key order and the panel's slider order
- Regenerate the shipped ini — and note that `deployment_files/main/` is relative
  to the **deployed mod**, `papyrus/mods/Physical Ragdoll Sounds/…`, not to this
  source tree. `--write-config` into a directory that already holds an ini loads
  it first and writes it back, so it never regenerates: generate into an **empty
  scratch dir**, diff the value lines against the deployed
  `RagdollSounds_Algorithm.ini`, and copy **only that one file**.
  `RagdollSounds_SFX.ini` is the pack's hand-managed slot assignments and
  regenerating it throws the bank away
- `SfxAssignments::Save`/`Load` gain `Conditions`, and the header comment block
  gains a paragraph — that comment is the only documentation a pack author gets
- `tools/sfx.py`'s `SPEC` table gains four entries, or `eval` will not judge the
  new files and `Slots.md` §3 has four blank rows
- `Slots.md` §2 gains four rows; `00-Design.md` §12 gains the armour block and its
  file count moves from 29
- **Deploy both halves.** `core/` is a static lib linked into the game DLL, so an
  engine change is a DLL change: `build-skse-mods.ps1 -Mod Physical-Ragoll-Sounds`
  for code, `deploy-pack.ps1` for assets. `pwsh` 7, not `powershell.exe`

---

## 11. Open questions

1. **Should `[Slots]`' three condition switches live in `[Armor]` instead?** I put them where they are because a condition can be surface-only,
   but "everything about this feature in one panel" is a real argument the other
   way.
2. **Is `armor_cloth` worth authoring at all?** Clothing is the default case, and
   a cloth thump under every unarmoured contact may just be mud. It reads
   differently now that `foley_cloth` is gone (§12): with the bed removed there
   is no cloth anywhere in the mix at all, so this slot would be the only thing
   that ever says *fabric* — which is an argument for it rather than against.
   Still my lowest-priority file.

**Settled, and recorded so they stop being asked:**

- **The armour skin does not go on the head accent.** §3.7 — two toggles from
  outside the strategy instead, and a `head_impact` condition if a helmet should
  ever have a voice of its own.
- **Conditions do not key on limb site.** Two halves only, surface and armour.
- **`foley_cloth` is removed rather than reused.** §12.

---

## 12. Appendix — removing `foley_cloth` *(done, 2026-08-24)*

Kept as the record of what moved and why, because the same enum and the same four
switches are what §3 extends next — and because §8.1's argument is largely this
appendix, counted.

Not part of the armour feature. It was here because it landed on the same enum,
the same switches and the same doc rows, and doing both in one pass was cheaper
than doing them a week apart.

### 12.1 The evidence that it is unused

`bFoleyCloth = 0` appears in **every saved config in `testbench/configs/`** —
all thirty-seven of them, plus `masson.ini`. The bed has been muted in every
tuning pass that was ever saved. It has files behind it, it emits cues, and it
has never been heard on purpose.

### 12.2 What goes with it

`foley_cloth` **is** the bed — it is the only slot `MotionFoleyStrategy`'s bed
half ever plays — so removing the slot and leaving the bed would leave a loop
with nothing to loop. Both go.

`MotionFoleyStrategy` keeps its name and its `[MotionFoley]` section. What is
left of it is the airborne rise, and an air whoosh driven by how the body is
moving is still honestly motion foley. Keeping the section name also means no
ini migration for the three keys that survive.

| File | What goes |
|---|---|
| `SlotManifest.h` | `kFoleyCloth`; the "first taste" comment that lists it |
| `SlotManifest.cpp` | its `SlotDesc` row |
| `Synth.cpp` | its procedural stand-in case |
| `Cue.h` | `CueReason::kFoleyBed` |
| `Vocabulary.cpp` | `ToString` arm, and the `CompressThresholdDb` arm |
| `Engine.cpp` | `ActorRuntime::bedRunning` / `bedVoice` / `bedLastDb`; the whole `wantsBed` block in `ProposeTick`; the `StopOneLoop` in `StopActorLoops`; the `kFoleyCloth` arms in `RoleTrimDb` and `SlotTrimDb` |
| `Config.h` | `layers.foleyCloth`, `slotGains.foleyCloth`, `compress.foleyDb`, and `MotionFoleyConfig`'s `bedGainDb` / `speedForMinGain` / `speedForMaxGain` / `preImpactDuck` / `preImpactDuckDb` / `preImpactDuckMs` |
| `Config.cpp` | the `LayerMute` arm |
| `ConfigSchema.cpp` | `fFoleyCloth`, `bFoleyCloth`, `fFoleyDb`, and the six `[MotionFoley]` bed rows |
| `AppSfx.cpp` | its entry in `kImpactOrder` — the `static_assert` will catch this one |
| `SfxAnalysis.cpp` | its SPEC row |
| `App.cpp` | its timeline colour |
| `tools/sfx.py` | its `SPEC` entry and the two classifier hints that map prompts onto it |
| `tools/triage_batch.py` | its keyword mapping |
| Docs | `00-Design.md` §12 and the first-taste list, `assets/Slots.md` §2 and §3, `02-SFX-Generation-Prompts.md`, `04-Reference-Analysis.md`, `assets/sfx/skyrim/README.md` |

### 12.3 Three things that need no work

- **The saved configs.** An unrecognised ini key is logged at debug and left
  alone, so all thirty-eight keep loading with `bFoleyCloth`/`fFoleyCloth`/
  `fFoleyDb` sitting in them inertly. No migration, no `Renamed()` wrapper.
- **The library sidecars.** Forty-odd `*.meta.ini` files carry
  `Slots = …, foley_cloth, …` under `[Suggested]`. That parser matches names
  against `Slots()` and silently drops anything it does not recognise, so the
  stale suggestion just stops being a suggestion. Nothing to strip, no warnings
  at load.
- **The wavs.** `foley_cloth_01.wav` and friends stay in the library as unused
  files. They are the right shape for `scrape_loop`, which several of them are
  already suggested for.

### 12.4 The one hand edit

The deployed pack's `RagdollSounds_SFX.ini` carried a `[foley_cloth]` section
naming `foley_cloth_01.wav`. An unknown slot section is ignored with a debug log,
so leaving it would have been harmless — but that file is **hand-managed and must
never be regenerated**, so it was cleaned by a targeted deletion of that six-line
section in
`papyrus/mods/Physical Ragdoll Sounds/deployment_files/main/SKSE/Plugins/RagdollSounds/`
and nothing else.

Note the trap next to it: the `[scrape_loop]` section names a *file* whose name
begins `foley_cloth_…`. That is a filename, not a slot, and it must stay.
