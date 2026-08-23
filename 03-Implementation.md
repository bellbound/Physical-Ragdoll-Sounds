# 02 — Implementation architecture

How [00-Design.md](00-Design.md) becomes two programs. The design says what the system does; this
says where each part lives, what the seams are, and which of them are load-bearing.

| Doc | What it is |
|---|---|
| [00-Design.md](00-Design.md) | The design. Asset list, strategy list, the decisions and why |
| [01-Reference-Analysis.md](01-Reference-Analysis.md) | Where the layer timings and level budgets come from |
| **03-Implementation.md** (this) | The code shape. Seams, build layout, what each agent owns |
| [02-SFX-Generation-Prompts.md](02-SFX-Generation-Prompts.md) | One text-to-SFX prompt per asset, written against 01's measurements |
| [Research/](Research/) | The capture study. What the engine can tell us and how far it can be trusted |
| [TODO.md](TODO.md) | What is built, what is next |

---

## 1. Two seams, and everything else follows

The whole architecture is two interfaces. Get these right and there is one backend; get them wrong
and there are two that drift.

```
                    ┌──────────────── core/ (portable, no CommonLibVR) ────────────────┐
                    │                                                                  │
   game contact ──▶ │ IFeed ──▶ Ingest ▸ CrashState ▸ Phase ▸ Strategies ▸ Arbitration │ ──▶ ICueSink ──▶ BSSoundHandle
   callback         │                                                                  │
                    │                                                                  │
   CSV + YAML ────▶ │ IFeed ──▶            (the same six stages)                       │ ──▶ ICueSink ──▶ miniaudio
   (a recording)    │                                                                  │
                    └──────────────────────────────────────────────────────────────────┘
```

**`IFeed`** ([Feed.h](core/include/rds/Feed.h)) is where "the game is telling us what happened" and
"a file is" become the same thing. `FeedEvent` is deliberately QuickModMenuNG's `RawEvent` with the
CSV's resolved columns folded back in — same fields, same units, same sign conventions — because the
captures came out of the game in the first place. The live path fills it in the Havok contact
callback; the replay path fills it from a CSV row; `Engine` cannot tell which it got.

**`ICueSink`** ([Cue.h](core/include/rds/Cue.h)) is Stage 5. Arbitration emits an abstract cue list
and something else turns it into voices. Its feature set is exactly what CommonLibVR verifiably
offers (§13 of the design): volume, continuous pitch, updatable position, bone attachment,
whole-file looping, fades, raw wav off disk. **Nothing richer goes in**, or the testbench will tune
something the game cannot reproduce.

The rule is enforced mechanically, not by discipline: `core/cmake/CheckPortable.cmake` runs every
build and fails it on an `RE/` or `SKSE/` include under `core/`. Without that, the mistake still
compiles inside the plugin and only surfaces as a link error in the other build a week later.

---

## 2. Layout

```
skse/Physical-Ragoll-Sounds/
├── core/                 static lib, built twice. THE BACKEND
│   ├── include/rds/      the public headers — the only thing the testbench may touch
│   └── src/
├── plugin/               SKSE plugin → RagdollSounds.dll. Registered in mod-registry.ps1
│   └── src/              the game-facing half: contact hook, suppression, audio renderer
├── testbench/            throwaway exe. ImGui + GLFW + miniaudio. Never shipped
├── assets/sfx/           the built pack, `<slot>_<NN>.wav`
│   └── library/          every sfx that exists, each with a `<file>.meta.ini` beside it
├── Research/             the capture study and the recordings
└── Example/              the four Skate 3 reference clips
```

`plugin/` is what `mod-registry.ps1` builds (`SourceDir = "plugin"`, the same shape virtual-hmd
uses); its CMakeLists pulls `core/` in as a static lib. `testbench/` has its own preset and vcpkg
manifest and is deliberately outside the mod build, so nothing in it can drift into the shipped DLL.

Deploys to `papyrus/mods/Physical Ragdoll Sounds/SKSE/Plugins/` like every other mod here.

---

## 3. What the backend owns

### The sound bank — assigned, not named

A slot's files used to be whatever `<slot>_<NN>.wav` happened to be in the sounds folder, so changing
what `imp_body` plays meant renaming files. That is fine while a python script builds the pack and
wrong the moment the choice is something to audition.

So there are two halves ([Sfx.h](core/include/rds/Sfx.h)):

- **`SfxLibrary`** — `sounds/library/`, every sfx that exists, each with a `<file>.meta.ini` sidecar
  carrying what the importer measured and what the user wrote in the note. The **filename** is the
  identity; the display name is only ever shown, so renaming is free and cannot break an assignment.
- **`SfxAssignments`** — `RagdollSounds_SFX.ini`, one section per slot: which library files it plays,
  in order, and whether it `Looping`s. The order *is* the variant index a cue carries.

`SoundBank::LoadAssigned` fills from the table and **falls back per slot** to the old convention scan
for anything the ini does not name. That is what makes this safe to ship on top of a pack somebody
has already tuned: an ini that reassigns one slot leaves the other twenty-eight exactly where they
were, and no ini at all sounds precisely as it did before.

`Looping` is per slot rather than per file because "is this a loop" is a property of what got
assigned as much as of the slot — a sliding or wind-like sound wants repeating and must not be
complained at for being long.

### Config — object-based, one schema

Two files, two structs:

| File | Struct | What is in it |
|---|---|---|
| `RagdollSounds.ini` | `GeneralConfig` | `Enabled`, `LogLevel`, `EnableLogRotation`, `MaxLogFiles`, `SuppressVanillaBodyImpacts` |
| `RagdollSounds_Algorithm.ini` | `AlgorithmConfig` | Every parameter of the sound engine — nine nested structs, one per stage or strategy |

The engine reads `cfg.arb.rateCapMs`, not `GetFloat("Arbitration:fRateCapMs")`. What makes that work
without hand-writing a parser is [ConfigSchema.h](core/include/rds/ConfigSchema.h): one
`ParamDesc` table maps each ini key to a member offset, and **the ini reader, the ini writer, the
defaults, the clamping, the "which values differ from default" log line and the testbench's entire
slider panel are all walks over it.** Adding a parameter is one field plus one schema row; the file
format and the UI both follow.

Each `ParamDesc` carries a `tooltip` saying what the parameter changes *perceptually*. The design
asks for that explicitly, and it is the difference between a panel of ninety numbers and a panel you
can tune by ear. It is written into the ini as the key's comment too, so the file is
self-documenting.

**The testbench seam is `ConfigManager::PushOverride`.** There is one `AlgorithmConfig` type; the ini
fills one instance, the testbench hands over another, and nothing downstream knows which it holds.
That is the same trick `IFeed` plays on the input side, and it is why there is no second code path.

### Logging

`rds::log::Setup` installs a spdlog default logger the same way the other mods here do, with
SkyrimNet's startup-rotating sink behind `EnableLogRotation` so the log from the session that
crashed survives the next launch.

The rule for what goes where — **a user's `info` log must be enough to tell whether the mod is
running, what it is configured to, and whether it heard the knockdown, without asking them to change
a setting and do it again:**

- **info** — init, both ini paths, every value differing from default, the sound bank's per-slot
  resolution (`imp_sub: 0/2 files, procedural` is exactly the line that explains a thin mix), vanilla
  suppression naming every form it touched, **one summary line per knockdown** (contacts in, cues
  out, bursts, reduction ratio, peak, duration), voice-cap hits, and every refusal to start
- **debug** — the firehose: per-contact admit/reject with the reason, phase transitions, every
  arbitration drop with its margin, every cue emitted
- **warn** — clamped config values with both numbers, unknown ini keys, unrecognised skeletons
- **error** — something the user must fix, and what we did instead

### The engine

[Engine.h](core/include/rds/Engine.h) is the six stages behind one object: feed in, cues out, driven
by a clock the caller owns. The game drives `Tick` from the frame hook with the real clock; the
testbench drives it from a virtual clock as fast as it likes. Same object, same order of operations,
same output — which is the only reason tuning offline means anything about the game.

Three properties the implementation must hold:

1. **Deterministic given a seed.** An A/B between two configs has to compare the configs, not two
   dice rolls. `SlotResolutionConfig::rngSeed` is 0 in the game (seed from the clock) and fixed in
   the testbench.
2. **Every window scales with frame time.** `max(k · frameTime, floor_ms)`, never a fixed
   millisecond count and never a frame count. The target range is 24–144 fps and a fixed window
   behaves as a different system at each end. (The capture set is 48–51 fps throughout and cannot
   test this — 07 §4.)
3. **No allocation in `Tick` on the steady path.** It runs on the game thread every frame.

### Offline

[Offline.h](core/include/rds/Offline.h) is the testbench's *entire* view of the backend:
`RunOffline(recording, config, bank) → cues + stats + trace`. It ticks at the recording's own frame
boundaries — derived from the gaps between contact batches, since a gap over 2 ms starts a new frame
— so the replay steps the way the game did.

This is also what makes "config changes apply in real time without restarting playback" trivial: the
testbench runs the whole recording again in about a millisecond, mixes the new cue list, and swaps
the buffer at the current play position. *"Call the backend twice with different configs and swap
playback source"* is one function called twice.

`Verify()` is the self-check — not a unit test, but the "did we build the right thing" pass. Each
assertion is one of the design's own numbers, so a failure means the algorithm drifted off the
references rather than that the code crashed:

- four to six audible moments per knockdown, not fifteen to thirty
- reduction ratio near 10:1 against the contacts that entered
- no two onsets closer than the rate cap
- bursts of three to five grains inside 200–400 ms, then ≥300 ms quiet
- the top one to three cues within a decibel, then a ≥9 dB cliff
- the sub layer 55–75 ms after its transient, and the loudest layer of the stack
- exactly one settle cue closing each knockdown
- byte-identical output for the same seed and config twice

### Sounds

None of the 29 authored files exist yet, so [SlotManifest.h](core/include/rds/SlotManifest.h)
resolves a slot to a wav *or to a procedural stand-in generated from the slot's own brief*. The sub
sweep in particular is fully specified by 01 §1 (~150 Hz → 30 Hz over 50–80 ms, into a 20–30 Hz
floor by 180 ms) and can be generated exactly. That makes the timing, the layer balance and the
gates audible today; dropping a wav into the bank overrides its slot with no code change, which is
the whole point of resolving through a manifest.

---

## 4. What the game half owns (stubbed for now)

Everything under `plugin/src/`. Deliberately **stubbed behind the interfaces** until the testbench
has tuned the algorithm, because there is nothing to tune it against otherwise:

| Piece | What it does | Notes for when it is written |
|---|---|---|
| `GameFeed` | Havok contact callback → lock-free ring → drained on the game thread | Copy `ImpactRecorder.cpp`'s shape. The callback runs on a Havok worker and **cannot** ask an actor whether it is ragdolling — publish that from the game thread into an atomic and read it with one relaxed load (07 §1) |
| `TickPublisher` | Per-actor, per-tick: ragdoll phase, listener state, distance tier, terrain material | `RE::TES::GetLandMaterialType` is game-thread only |
| `GameRenderer` | `ICueSink` → `BSSoundHandle` | On the new CommonLib fork: `GetSoundHandleByFile`, not `BuildSoundDataFromFile` |
| `VanillaSuppression` | Null `sound1`/`sound2` on the body impact records at data load | Walk `PHYBodyMedium`, `PHYBodyLarge`, `PHYBodySmall`, `PHYBodyBones`, `PHYBodyMetalLarge`, `PHYBodyMetalSmall` plus the armour and meat sets — about eight distinct `BGSImpactData`. In-memory only; never reaches a save. Log every form touched |

---

## 5. What the testbench owns

One exe, ImGui + GLFW + miniaudio, video pre-decoded to frames by ffmpeg into `framecache/`. Its
only contact with the backend is `core/`'s public headers; if it ever needs more, the seam is wrong.

- **Left panel** — the take's video where one exists, scrubbing in lock step with playback.
  `video_time_ms = t_ms + offset`; fit the offset through the low-rtt rows of the `_sync.csv` rather
  than taking the first, because over a long take the two clocks drift. The mp4s are cuts of a longer
  OBS recording whose cut point is *not recorded anywhere*, so the offset needs a per-take nudge in
  the UI that persists.
- **Right panel** — the config editor, built by walking `AlgorithmParams()`. Every change re-runs
  `RunOffline` and hot-swaps the mixed buffer at the current play position. Named configs save to
  `testbench/configs/*.ini` through `ConfigManager::SaveFrom`, so a testbench config *is* a shippable
  `RagdollSounds_Algorithm.ini`.
- **Split mode** — copies the top config to the bottom and alternates A → B → A on each loop, with a
  "continue with this one" button per panel.
- **Keys** — `Num5` play, `Num4`/`Num6` previous/next recording, `Num8`/`Num2` previous/next config.
  (The brief said `Num8`/`Num6` for configs, but `Num6` is already next-recording; `Num8`/`Num2` is
  the up/down pair that matches `Num4`/`Num6` being left/right.)
- **Loop** — auto-loop on by default with a toggle under the video, plus a draggable region on the
  timeline to loop a shorter section.
- **`--verify`** — headless, runs `Verify()` over every recording and exits non-zero on a failure.

### The sfx panel and the library window

The right column is two panels with a draggable bar between them: the algorithm config above, the
**SFX** panel below. They answer different questions — *how loud, how often* against *which sound* —
and you read one while changing the other.

The SFX panel is one widget per slot, in the order the layers of an impact arrive (transient →
surface → body → sub, then the things that ride on top, then the things that close it), with the
slots this take actually used sorted to the front. Each widget carries the slot's recommended length,
a `loop` toggle, and every file assigned to it with a play button and its badges. Hovering one lights
that slot's cues in the timeline and dims the rest — which is how "what does this slot actually do
here" gets answered by pointing rather than by counting.

`change` and `+ add` open the **library window** as a picker: the same window the header bar's
`SFX library` button opens, plus two preview widgets at the top holding what the slot plays now
against what is highlighted, and an order that puts the sounds of a fitting length (±25% of the
slot's range) first. The search box takes the keyboard on open, arrow keys move the highlight, space
auditions it, Enter picks it. The browser owns those keys while it has focus, so space does not also
toggle the take's transport.

Auditioning runs on a **second voice** in `Player` mixed on top of the transport, not through it.
That is deliberate: comparing what a slot plays now against what it would play means hearing them
near each other, and a preview that stopped the take would make that two clicks and a re-seek every
time.

Importing is `SfxImport.cpp`: ffprobe reads the container, ffmpeg converts to the pack's mono /
48 kHz / 16-bit, miniaudio decodes it and `SfxAnalysis.cpp` measures it — `tools/sfx.py`'s `measure()`
ported, over the same band definitions, so a number in the browser can be read against Slots.md §3
directly. **An import never refuses.** Whatever arrives is made playable and what is wrong with it
comes back as a badge; the only blocking case is a file nothing can decode, which is not a judgement.

### Everything unsaved, and Ctrl+S

There are four things that can be unsaved — config A, config B, the assignment ini, and any sfx names
or notes — and the top bar says so with one marker. `Ctrl+S` writes all of them, and works while a
text box has the keyboard, because that is exactly when you reach for it.

A config **save writes the next iteration** rather than over the file it loaded: `config_22_08_4`
becomes `config_22_08_5`, skipping anything that already exists. On by default, off with the
`iterate` checkbox beside the button. The reason to save mid-session is nearly always that the last
one was worth keeping too, and overwriting it is the one thing Ctrl+Z cannot take back.

---

## 6. The numbers that must not drift

Everything in `AlgorithmConfig` is tunable, but these defaults are measurements and changing them
should mean disagreeing with a measurement, not disliking a number:

| Default | Value | Where it comes from |
|---|---|---|
| `arb.rateCapMs` | 46 | The onset-gap floor in three independent reference clips. Global, **not** per-limb |
| `impact.subOffsetMs` | 65 | The sub arrives +64–74 ms after the transient in every hero hit, and is the loudest layer in 5 of 7 |
| `intensity.dynamicRangeDb` | 35 | Onsets span 13–17 dB; the bed sits 30–36 dB under the hero hit |
| `arb.burstMinGapMs` | 300 | Between-burst gaps measure 313–894 ms; inside-burst 46–104 ms |
| `crunch.crunchGateFrac` / `crunchCertainFrac` | 0.52 / 0.73 | Ordinary shove peaks 355–543 u/s, three-metre fall 600–855 — 500 / 700 against the 960 anchor |
| `ingest.blowupDisagreeFrac` | 0.10 | Flags 23 of 1377 rows, 0 % below 20 u/s, 50 % above 2000 |
| `ingest.frameGapMs` | 2 | Median gap inside a frame's callbacks is 1.0 µs, between frames 20.4 ms |
| `phase.settle.gainTrimDb` | −14 | The last twenty contacts of every knockdown are settles and should be nearly silent |

And two that are explicitly **guesses**, flagged so nobody quotes them back as measurements:
`intensity.speedRefHigh` (960 u/s — the take that would have established the ceiling was discarded)
and `ingest.blowupSpeedCeiling` (1000 u/s — "a safe floor for the guess, not a measurement").

---

## 7. Things the data cannot tell us

Carried here so they are not rediscovered as bugs:

- **The get-up window** is unmeasured — every capture take was paralysed and none ever got up.
  `phase.getUpBlendMs` is a guess and needs one unparalysed take.
- **No natural ground** exists in the capture set at all: no dirt, grass, snow, gravel or water.
  `surf_soft` covers them until someone records on snow.
- **No character-on-character contact.** Three takes tried, all three missed.
- **`tangent_speed` has never seen a real scrape** — the take named `slide` turned out to be an
  extreme push. The whole `ScrapeLoop` path is built on an untested column.
- **48–51 fps throughout**, so the frame-rate-invariance claim is inherited, not confirmed.
