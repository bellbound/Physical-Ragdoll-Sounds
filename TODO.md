# TODO

State of the build. See [03-Implementation.md](03-Implementation.md) for what each piece is.

Legend: **[x]** done · **[~]** in progress · **[ ]** not started · **[?]** blocked on a decision or a capture

---

## Scaffold — done

- [x] Project layout: `core/` (portable, built twice) · `plugin/` (SKSE) · `testbench/` (throwaway)
- [x] `core/` public headers — the contract both halves build against
- [x] `CheckPortable.cmake` — fails the build on an `RE/` or `SKSE/` include under `core/`
- [x] CMake + vcpkg manifests for all three, on the **new** CommonLib fork via `plugin/vcpkg-ports/`
- [x] Registered in `mod-registry.ps1` as `Physical-Ragoll-Sounds` → `RagdollSounds.dll` →
      `papyrus/mods/Physical Ragdoll Sounds/`
- [x] `03-Implementation.md`

## Backend — `core/`

- [x] `ConfigSchema.cpp` — 250 `ParamDesc` rows (10 general, 240 algorithm) over every field of
      both config structs
- [x] `ConfigManager.cpp` — ini load/save through the schema, comments preserved, testbench override
- [x] `Log.cpp` — spdlog + startup-rotating sink, the info/debug discipline
- [x] `SlotManifest.cpp` — slot table, shuffle-bag `Resolve`, pure-lookup `Get`, wav length from the
      file's own header, MATERIAL_ID → surface class
- [x] `Recording.cpp` — CSV + YAML replay, implementing the data dictionary's warnings, plus the
      sync-csv slope-and-intercept fit
- [x] `Engine.cpp` — the six stages, strategies behind `IStrategy`. The main work
- [x] `Synth.cpp` — procedural stand-ins, reproducible, the `imp_sub` sweep off 02-SFX §3
- [x] `Offline.cpp` — `RunOffline` + the eight `Verify` checks
- [x] `core/tools/rds-verify.cpp` — headless harness: bank round-trip, config round-trip, `Verify`
      over a folder, `--set Section:Key=value` to sweep any parameter through the schema

**Verify at shipping defaults: 179 of 225 checks pass over the 25 takes** (2026-08-23, after the
two-axis rework; it was 145 of 200 before). Determinism passes on every take and two full runs are
byte-identical, so the total is a usable regression signal - an earlier note here said otherwise and
no longer reproduces. The families that fail are the calibration questions below, and two of them
are the design disagreeing with itself rather than the code disagreeing with the design.

## Game integration — `plugin/`

**The renderer no longer plays a voice per layer.** It mixes each composite itself and hands the
engine one PCM blob through `BSExternalAudioIO::ExternalIOInterface` — the mechanism SkyrimNet
uses for TTS. The reason is arithmetic: voices start on frame boundaries, so at 60 fps the
`+0 / +8 / +20 / +65 ms` layer stack loses its first two layers into the same frame and the
composite the whole design rests on cannot be reproduced. Mixing gets sample-accurate offsets,
per-layer pitch as resampling, gain baked in rather than sent as a droppable `SetVolume` message,
and one engine voice per impact instead of four. It also means the testbench and the game now share
a mixer (`core/Mix.h`) rather than approximating each other.

### M1 — tick and render: **done, not yet heard in game**

- [x] `core/Pcm.h/.cpp` — portable wav reader (one parser now; `SlotManifest`'s own was promoted
      here), `PcmCache` resolving a `(slot, variant)` to mono float and falling back to `Synthesise`,
      and `EncodeWavPcm16Into` writing a RIFF container through a caller-owned buffer
- [x] `core/Mix.h/.cpp` — `MixVoice` / `MixComposite` / `MixLoop`. Pure `std`, so `CheckPortable`
      stays green
- [x] `plugin/FrameHook` — trampoline on the main update, engine frame delta for `FrameTimeSec()`.
      **This is what was making the mod inert: `OnFrame` had no caller**
- [x] `plugin/AudioBlobs` — the external audio interface, chained rather than replacing; blob
      registry with refcount plus grace window, buffers recycled
- [x] `plugin/GameRenderer` — groups by `(actorId, sourceSeq)`, mixes, opens with
      `GetSoundHandle(descriptor, 0x8000|0x0080)`, places and plays. Loops are long buffers that get
      re-issued with a crossfade, because `BSSoundHandle::LoopType` has no setter
- [x] `plugin/TestCue` — one canned composite on a key, so the renderer is provable without the
      contact pipeline. `iTestCueKey`, off by default
- [x] Fixed: `log::Setup` was called twice and the second call is a no-op, so `EnableLogRotation`
      and `MaxLogFiles` were read from the ini and then thrown away
- [ ] **Not verified in game yet.** Nothing below M1 has been heard
- [ ] **No sound category on our voices.** `Resolution::soundCategory` is null, so the player's
      volume sliders do not apply to us. Needs a form id that can only be found with the game
      running; the output model is already a config key (`iOutputModelFormID`) and this wants the
      same treatment

### M2 — vanilla suppression: **done, not yet heard in game**

- [x] Walks `GetFormArray<BGSImpactDataSet>()` and matches on `GetFormEditorID()`.
      **Not** `LookupByEditorID`, which is used nowhere else in `skse/` and needs po3_Tweaks to
      have populated the cache — without it the mod would suppress nothing and say it had
- [x] `impactMap` is keyed by `BGSMaterialType*`, not `MATERIAL_ID`. The same `BGSImpactData` is
      reached from dozens of materials, so records are de-duplicated before being nulled or the
      restore table fills with duplicates and the second restore undoes the first
- [x] Restore table, and a line at info per form touched plus a summary count

### M3 + M4 — the contact feed: **done, not yet heard in game**

- [x] `ContactRing` — Vyukov bounded MPMC, 8192 slots, sequence-stamped. An overflowing producer
      fails its claim outright; a flag ring would strand the dropped position and stop the
      consumer dead at it for the rest of the session
- [x] `RagdollBodies` — `CaptureRagdoll` ported from QuickModMenuNG: graph update lock, active
      graph first, `boneToRigidBodyMap`, the Havok body's own name as the bone name
- [x] `LimbListener` — one per limb, `hkpEntity::AddContactListener` under the world write lock,
      `contactPointCallbackDelay` saved and zeroed. **No code hook and no address.** The callback
      gates on the published phase atomic, then does the sign convention, the world scale, the
      `v + ω × (p − com)` reconstruction (always body[0] − body[1]), the tangent, and the shape
      material with the terrain fallback
- [x] `PublishTick` — phase and terrain material published before anything else in the tick,
      listener from the camera, profile rebuilt on every ragdoll attach, limbs resolved by bone
      **name**, coverage from the worn armour slot
- [x] State rows. The engine opens a knockdown on `ragdoll_start` and closes it on
      `ragdoll_end` / `knock_get_up` / `actor_gone`; without them it would acquire on the first
      contact and never let go, so the crash state would linger and the one summary line per
      knockdown would never be written. They go through the ring so they keep their place in time
- [x] Tracking starts when an actor is **knocked**, not when the ragdoll starts — the lead-in
      states are what get the listeners on in time. The phase gate is what keeps walking NPCs
      silent, and it is the only thing that does

### M5 — the player and distance: **done, not yet heard in game**

- [x] `BoneResolver` wired: the renderer asks the feed for the node, the feed looks it up by the
      limb's bone name on the actor's 3D
- [x] Culling in the feed at the engine's own Simplified radius, read once rather than per frame
- [x] Player mix profile needs no code — `player.attachToBones`, `subTrimDb` and `masterGainDb`
      are config the engine already applies
- [ ] Verify in game at 24 / 60 / 144 fps that no window is secretly frame-count based.
      **Only the game can answer this**: every capture ran at 48–51 fps, so 07 §4's invariance is
      inherited from a deleted dataset

### M6 — shipping

- [x] Pack deployed to `SKSE/Plugins/RagdollSounds/sounds/`, by `deploy-pack.ps1` — scoped to
      `sounds/` only, because everything else in that folder is hand-maintained
- [x] `meta.ini`, and the MO2 junction, which did not exist
- [x] Pristine ini pair under `deployment_files/main/`, generated from the schema by
      `rds-verify --write-config` rather than from whatever the dev machine last ran with, plus
      the `$PackagingRules` entry that overlays it
- [x] Source zip: `takes/` and `framecache/` excluded, and `*.mp4` — 658 MB of capture video
      against the 6 MB of CSV beside it, which stays because it is what `rds-verify` replays
- [ ] Mod description must carry the two accepted consequences of suppression: it is global (a
      dragged corpse, a thrown severed head), and any other mod expecting those descriptors loses
      them

## Testbench — `testbench/` — built and running

`testbench/build/testbench/RagdollSoundsTestbench.exe`, no arguments needed.

- [x] ImGui + GLFW shell, miniaudio playback, cue-list mixing with equal-power pan
- [x] ffmpeg pre-decode to `framecache/`, video panel, sync fit with drift
- [x] Config editor generated from `AlgorithmParams()`, buffer hot-swap at the play position
- [x] Split A/B mode, named config save/load
- [x] Timeline with burst brackets, loop region, cue provenance inspector
- [x] `--verify` / `--smoke` / `--take` / `--play` / `--config` / `--bench` CLI
- [x] **Benchmark button** under the transport — pauses playback, replays the take as fast as it
      will go for a budget on a slider, reports best ms/run, µs per engine tick, realtime factor
      and (split A/B) the percentage between the two configs. Tracing off, because the game never
      traces. `--bench` is the headless twin. Baseline on the capture set: **~0.3 µs per frame**
      for an ordinary knockdown, 1.8–2.8 µs on the two long multi-knockdown takes, and near-flat
      against cue count — the work is ingest and arbitration, not emission
- [ ] **The `Num4/5/6/8/2` keys and the Save button have never been pressed.** Synthetic input
      could not take foreground focus, so the input handling is the one unverified path
- [x] Master limiter behind a not-shippable flag, pre-limiter peak always on screen — the game
      has no bus and no limiter, so a config that only sounds controlled here would clip in Skyrim

### Dynamic sfx assignment — 2026-08-23

- [x] `SfxLibrary` + `SfxAssignments` in `core/` — `sounds/library/` with a `<file>.meta.ini` each,
      and `RagdollSounds_SFX.ini` saying which of them every slot plays. `SoundBank::LoadAssigned`
      falls back **per slot** to the old `<slot>_NN.wav` scan, so a partial ini is a partly
      reassigned pack and no ini at all sounds exactly as it did before
- [x] The plugin reads it: `sounds/library/` + `ConfigManager::Sfx()` at data load
- [x] SFX panel under the config panel, split by a draggable bar. One widget per slot in impact
      order, the slots this take used sorted first, hover lights that slot's cues in the timeline
- [x] Library window, opening as a browser or as a picker for one slot. Search over name, note and
      badges with the match highlighted; arrow keys and space; notes saved on focus loss; picker
      sorts fitting lengths (±25%) to the top and holds the current sound against the highlighted
      one in two preview widgets
- [x] Import through ffmpeg/ffprobe: converts to mono / 48 kHz / 16-bit, measures the way
      `tools/sfx.py measure()` does, detects loops, suggests slots, warns and **never refuses**.
      FMTS name fix on by default. `--import <file>...` is the headless twin
- [x] Preview runs on a second voice in `Player`, mixed over the transport rather than through it
- [x] Assignment edits are undo/redo through the same one history as sliders and loop regions
- [x] One unsaved marker for all four saveables; Ctrl+S writes them; a config save writes the next
      iteration (`config_22_08_4` → `_5`) unless `iterate` is unticked

Three bugs this turned up, all fixed:

- `SoundSource` in the testbench rebuilt `<slot>_<NN>.wav` from the **variant index**, which is a
  position in a sorted set and not the number in the filename. Variant 0 looked for `imp_body_00.wav`,
  never found it, and played a **procedural stand-in for the first variant of every slot**, with
  every later variant off by one. It now resolves through the bank. Anything tuned before today was
  partly tuned against stand-ins
- The first two loop tests called `scrape_loop_01` an event — one because a grainy scrape *does*
  have a loudest grain, the next because it falls out of a 20 dB window between grains. Duty cycle
  over the same 40 dB window `steady` uses is the one that works
- Lead-in measured against the peak flagged `gore_wet` and would have flagged `settle_rest`, which
  is *specified* to start softly. 03-Asset-Status.md §6 records this exact trap for
  `verify_pack.py`; it is an absolute floor now

### Picking by ear — 2026-08-23

- [x] **Disable, in the library.** A `Disabled` flag in each `<file>.meta.ini`, toggled from the
      row and from the `in the slot now` widget. A muted entry keeps its place on every slot that
      names it, drops to the bottom of the browser list so it stops turning up in the search, and
      is skipped by `SoundBank::LoadAssigned` — so it is muted in the game too, not just here.
      The point is the difference from `x` on a slot: that forgets which slot the sound was on,
      this suspends it and remembers all of it
- [x] **The fallback no longer resurrects the pack.** `LoadAssigned` decided “nobody named
      anything for this slot” from whether any variant came out, so a slot whose every named file
      was disabled — or missing — fell through to the `<slot>_NN.wav` scan and answered
      “mute this” by playing the built pack. It reads the ini's own list now
- [x] **Search ranks by where it hit.** Name and filename first, then the note, then the badges.
      Typing `body` meant the sound called body and was getting the ninety the analysis suggested
      for `imp_body` on top of it
- [x] **The in-take audition.** With the picker open for a slot, the highlighted file is dropped
      into that slot and the take is re-mixed, so arrowing down the list plays each candidate under
      the transient that arrives 10 ms before it rather than on its own. Nothing is assigned by it;
      closing the window puts the slot's own sound back. A checkbox, because it costs a re-mix of
      the whole take per keypress
- [x] **`hide self` on the impacts table**, on by default. A ragdoll hits itself constantly and
      those rows are most of a fall while being almost none of what anybody is listening for. The
      count of what is hidden sits beside the box; the timeline's contacts lane still draws them
- [x] **The loop region is draggable.** An edge grabs that edge, inside grabs the whole region and
      slides it at its own length, and the strip anywhere else still draws a new one. The cursor
      says which before the press lands, and a tap on a region you already have no longer clears it
- [x] **The export carries the config, not its name.** `include configs` beside the export
      button, remembered between launches, writes the whole `AlgorithmConfig` as an ini at the end
      of the report — every key with the comment that says what it changes, exactly as it stands
      in memory whether or not it has been saved, and the take line says `EDITED` when those two
      differ. It round-trips: the block extracted from an export and loaded with `--config` gives a
      byte-identical block back. `--export` always includes it, because a headless dump is read by
      somebody who cannot resolve `config_22_08_5` to numbers. `ConfigManager::ToIniText` is the
      one renderer, shared with `SaveFrom`
- [x] **`iVoiceCapPerActor` / `iVoiceCapGlobal` are gone from the config.** They are constants in
      `Engine.cpp` at the values they defaulted to. Neither is tunable by ear: below them the mix
      loses layers it needed, above them the game's own audio engine is what starts dropping
      sounds. Configs that set them — the `config_22_08_*` set carried 8 and 16 — now run at
      12 and 24, which is a real change to what they sound like in a heavy pile-up

### Video sync on a live take — 2026-08-23

A devbench take came out with no `_sync.csv` at all and a flat 2000 ms offset guessed on top of it,
because both halves of the sync path were written for the takes in `Research/` and neither case
applies to a take this program records itself.

- [x] **The take writes its own sync track.** `obs::SampleClock` was ported over with the rest of
      the OBS driver and never called from anywhere. It is now called at the arm, once a second
      through the take, and once more just before the stop — the row is (the game's session clock at
      the midpoint of the round trip, OBS's `outputDuration`, the rtt), rebased onto the take's own
      clock by `WriteTake`'s `TakeWindow` and written as `<stem>_sync.csv` in exactly the shape
      QuickModMenuNG wrote and `Recording.cpp` already reads
- [x] **The game's clock crosses the socket.** The events are stamped on the game's session clock
      and nothing in this process was on it. `GameLink::GameClock` reads it off the 1 Hz heartbeat:
      both ends are `steady_clock` on one machine so only the epoch differs, and the epoch is taken
      as the *smallest* difference any heartbeat has shown, because a packet can only ever arrive
      late. That is a few milliseconds — inside a video frame — and it biases the intercept only,
      never the slope
- [x] **The two sync regimes are told apart by the take, not by a guess.** A `Research/` mp4 is a
      cut of a longer recording whose cut point is written down nowhere, so its intercept is useless
      against the file and the 2000 ms pad is the starting point. A devbench mp4 *is* OBS's whole
      output for that take, so the intercept is the offset outright and there is nothing to nudge.
      The sidecar's `obs:` block says which — `output_path` naming a file the take owns, and no
      `external_recording` — and `RecordingInfo::videoIsWholeOutput` carries it. This also retires
      the duration heuristic for takes that state it, and fixes `Proventus_Avenicci_impacts_log_14`,
      which was uncut and only got the right answer by accident
- [x] A take whose video arrived but whose sync track could not (no game heartbeat) starts at zero
      rather than at a pad that was never applied to it, and the video row says so instead of
      reading as a complaint

### Muting a sound for good — 2026-08-23

- [x] **A slot mute is saved and the game honours it.** It used to be session state in the
      testbench: written nowhere, not pushed over the link, gone at the next launch. It now lives
      in `SlotAssignment::muted` beside the file list, writes a `Muted = ` line into
      `RagdollSounds_SFX.ini`, goes over the devbench link as `<slot>.mute=`, and
      `SoundBank::LoadAssigned` applies it — so a file muted here is a file the mod does not
      play. It undoes and redoes with every other assignment edit and lights the unsaved marker,
      which it also never did
- [x] **A muted file keeps its variant index.** It is added to the slot and then suspended, not
      skipped — the skip is what the library's `disabled` does, and it renumbers everything after
      it, so a cue list recorded before the mute would play different files after it. Unmuting
      puts the take back exactly as it was
- [x] **The mute dies with the sound.** Taking a file off a slot, or replacing it through the
      picker, drops its name from `muted` — otherwise the mute would lie in wait and re-apply
      itself the day that file was assigned there again
- [x] **`rds-verify` checks all four properties**: the ini round-trip, the variant index, that
      400 resolves through both pickers never return it, and that a slot with every file muted
      goes silent rather than falling back to a procedural stand-in
- [x] Forcing is unchanged and still session-only. A pin is a way to listen; a mute is a decision
      about the pack, and that is the whole reason they are now stored in two different places

### Holding one class down, and what a cue is actually playing — 2026-08-23

- [x] **`[Compress]`** — one threshold per class of cue (impact, tap, head, crunch, gore, scrape,
      foley, airborne rise, settle) and one shared `fRatio`. Off by default and every threshold is
      0, which is the top of the range and therefore no holding at all. Taken once per proposal
      off its loudest layer and subtracted from every layer of the stack, so a held impact keeps
      its layer balance and loses only its level. Post arbitration, so it can never change which
      contact wins the rate cap
- [x] **It squeezes, it does not clamp — and the first version did.** A hard cap puts everything
      above the threshold on exactly the threshold. Measured on `Lennald_..._log_2` with a cap at
      −20: **4 events within 1 dB of each other** where there had been an 11 dB spread. Through
      the compressor at the same threshold the peer count stays at **1** at 2:1, 4:1 and 8:1 — the
      loud ones get closer but stay ordered — and only collapses back to 4 at 20:1, where the
      ratio has become a cap again. That sweep is in `config.md`; it is also why `fRatio` stops at
      20
- [x] **Whole-range compression was considered and rejected: it already exists.**
      `Intensity:fDynamicRangeDb`, `PostIntensity:fExtraRangeDb` / `fCurveExponent` and the two
      `fSoftClipKnee`s all shrink the whole span, globally. They move the quiet cues that were
      already right. The gap was per-class and top-only
- [x] **The threshold is relative, not absolute.** Measured against `Proposal::levelDb` — the
      pre-trim number Stage 4 sorted on, whose zero is the hardest contact the engine can hear —
      so every trim applies on top of the compressed value. Verified at threshold −20, ratio 4:
      the held events sit at −21, −15 and −9 dB at master gains of 0, +6 and +12, and the 2.8 dB
      cliff between them does not move at all. A rendered-level or 0..1 threshold would have
      pinned them and silently tightened by 6 dB every time the master went up, and 0..1 would
      also be a fiction — the game applies its own falloff after us, so there is no full scale to
      be a fraction of
- [x] **A held cue says so, and by how much.** `Cue::compressCutDb` carries the amount, because
      the level alone cannot tell "as loud as it wanted to be" from "as loud as it was allowed to
      be". The timeline draws a ghost tick at the height the bar *would* have reached with a
      hairline down to where it actually does — the gap is the compression, which is the one thing
      a number in a table cannot show across a whole take at once — the cue table puts the figure
      in brackets after the gain, hovering either explains the threshold and the ratio, and
      `EngineStats::compressedCues` counts them in the export's funnel
- [x] **The cue table names the file.** A slot is a role, not a sound: which of its variants a cue
      got is a shuffle bag, the contact's own token, or a session pin, and `imp_body` on its own
      never answered "what did I just hear". `App::SoundOf` resolves it through
      `SoundBank::Get` — the pure lookup, never `Resolve`, which would advance the bag and name a
      different file than the one the take was mixed from — and the timeline hover, the
      provenance line and the export's CUES table all read that one answer

### The two-axis rework — 2026-08-23

`01-Architecture.md` §4 landed: the `Phase` enum is gone, split into a `Motion` axis (what the body
is doing, physics owns it, transitions freely) and a `Moment` axis (what the mix is doing, latched
and windowed). See 01-Architecture §3.7 for the measured before/after.

- [x] **`Motion` and `Moment` replace `Phase`.** `PrimaryImpact` was the moment axis all along;
      `Settle` and `Rest` merged into `Resting`, which keeps the quiet budget but is no longer a
      one-way door. `BudgetFor(const CrashState&)` is the one place the two axes meet
- [x] **The hero test is on raw `impactSpeed`**, so the intensity clamp at 1.0 stops mattering and
      `Intensity()` needed no change. Dominance against the pre-tick envelope, floored at the hero
      floor; arrival out of measured flight, gated on pose. A fall may have no hero at all
- [x] **17 config keys retired**: the 7-key air-time budget reset and the 6-key head refund, both
      absorbed by the hero moment's own budget, and the 4-key head halo, absorbed by the hero window
      plus spatial collapse. `bHeadClaimsOnset` survives as `HeadImpact:bClaimsOnsetOnHero`, fired by
      the moment axis. **`Arbitration:fRateCapOverrideDb` was kept** - §3.5 listed it as a workaround
      and it is not one; it reads no phase and no air time, only one level against another
- [x] **`[Phase]` is `[Motion]`**, with the hero rows under `[Hero]`. Every moved row is wrapped in
      `Renamed()`, so an existing ini loads unchanged and migrates itself on the next save. 247 → 240
      keys, verified by diffing the generated ini key list before and after
- [x] **The modifier pipeline is declared**: `Shape()` and `Grant()` carry the two invariants once
      each, `SlideSensitivity` is a `floorFrac` plus a `ShapeLift` plus a `BudgetWaiver` under the
      same ini keys, and `Contact`'s four parallel trim fields collapsed to one. Verified as a pure
      refactor - byte-identical verify output across the change
- [x] **New named check `hero moments`**, passing on all 25 takes: 0-3 heroes per knockdown, and
      `settle in flight` must be 0. The second is 01-Architecture §3.6, where the closing cue fired
      184 ms into a flight and 121 units above the ground
- [x] `rds-verify`'s funnel gained a third line: `heroes N (+M re-anchored) | settle in flight N`

Two bugs this turned up, both fixed, both worth remembering because each is a trap the old code had
already documented and the new code walked straight back into:

- **The `Tumble → Airborne` edge has to be gated on pose.** Without it, "airborne" means "nothing
  has touched recently", so a body lying still reads as flying - the machine never reached Resting
  and **all 25 takes lost their closing cue**. The docs say this about the fallback in as many
  words; it still had to be measured to be believed
- **One burst reset per moment, not per proposal.** A landing is five limbs in one frame carrying
  the same evidence, so granting the reset per proposal turned one burst into five - 14 grains
  against a cap of 5. The retired `ApplyAirReset` had a comment explaining exactly this. It is
  decided in the arbitrator now, where proposals are ordered loudest-first

- [ ] **Listen to the rhythm.** `audible moments` went 4 → 6 failures and `burst shape` 1 → 4: a
      machine that can open a hero late in a fall produces more bursts than one that could not.
      Whether 2.9 bursts/s is right is the "reduction ratio, or bursts per second?" question below,
      and it needs ears
- [ ] **`hero cliff` is scoring against a contradiction** and should be fixed or dropped - see the
      calibration entry below. It is 13 of the 46 remaining failures

## Calibration decisions — need ears, not code

These are why `rds-verify` is not green. Each is a real question the testbench exists to answer.

- [?] **Reduction ratio, or bursts per second?** The design's 10:1 comes from pairing "30–60
      collisions" with "4–6 audible moments" — but 4–6 moments at the measured 1.5/s needs a
      *three-second* tumble, and every take here fits its audible window into 1.1–2.6 s. So we
      produce 2–4 bursts at 1.2–2.5/s (the rate check passes on 11 of 12, dead on the reference)
      and the ratio lands at 12–64:1 by arithmetic. The rate is the measurement that transfers
      between scenarios; the ratio was downstream of one clip's length. **Recommend making the
      rate the assertion and the ratio a reported number.** Also: contacts vary 36→128 across
      byte-identical scripted takes, so the ratio is hostage to a quantity 07 §10 already calls
      unstable, while the rate is not
- [?] **The hero cliff contradicts temporal masking.** Masking drops anything >12 dB under the
      ceiling, so by construction everything played is within 12 dB and a ≥9 dB cliff *among
      played events* cannot exist. The references' cliff sits between heroes and quieter grains
      that still play. Either `maskDropBelowDb` or the cliff figure is wrong
- [?] **The sub layer is rare on this data.** With the shipped endpoints (sub −30→0, body −8→−2)
      the sub only overtakes the body at intensity ≈0.92, i.e. ~900 u/s. Nothing in these takes
      gets near that, so the mod's signature layer almost never leads. That is entangled with
      `speedRefHigh = 960` being a **guess** — the take that would have set the ceiling was
      discarded. Sweep it by ear before trusting either number
- [?] **`fVoiceFloorDb = -48` is silencing whole stacks.** Dropping it to −60 takes the failures
      from 13 to 12 and raises total bursts 21→26, then plateaus. Left at the default deliberately
- [?] **`grazeRatio = 1.5` routes 53 % of above-floor contacts to the scrape path** (median
      tangent/impact is 1.68) — a path 07 §11 says nothing in the dataset exercises. Sweeping it
      to 99 moves the pass count by one, so it is not the limiter, but it wants a decision
- [ ] **`Proventus_..._log_4` genuinely under-fires**: 1 burst at 0.4/s across 2.6 s. The only
      take where the rate check fails, and the only one that looks like a real algorithm bug

## Assets — 29 of 29 built

Briefs are written: [02-SFX-Generation-Prompts.md](02-SFX-Generation-Prompts.md), one prompt per file.
Status and grading: [03-Asset-Status.md](03-Asset-Status.md) — **which still says 14 and is out of
date**.

`assets/sfx/` holds all 29 and every one of them decodes: `rds-verify` now checks that explicitly
rather than trusting the bank's file count, because a file that fails to decode falls back to its
stand-in and the mod stays audible while sounding wrong. Deployed to
`papyrus/mods/Physical Ragdoll Sounds/SKSE/Plugins/RagdollSounds/sounds/`.

Procedural stand-ins still cover any slot that is missing or unreadable, so a partial pack is a
quieter mod rather than a broken one.

- [x] Every slot in the 29 is filled and decodes
- [ ] `scrape_loop_01` still fails its own spec (03-Asset-Status §4) — it plays as a low rumble with
      no grit. Now worth fixing rather than deferring: the path it feeds is exercised by every
      devbench take, and its level and pitch track the body's speed, so grit is the only thing
      missing from a slide sounding like one
- [ ] `grunt_impact` / `scream_big` stay declared and unfilled — adding voice later is a config change

## Open questions from the research

- [?] **The get-up window.** `phase.getUpBlendMs` is a guess. Every capture take was paralysed, so
      nothing measures it. Needs one unparalysed take. Does not block v1 — death ragdolls never get up
- [?] **The intensity ceiling.** `intensity.speedRefHigh` = 960 u/s, but the take that would have
      established it was discarded. The docs quote it inconsistently. The loudness curve calibrates
      against this, so it is worth resolving. The 10 m fall recapture is the highest-value one outstanding
- [?] **Natural ground.** No dirt, grass, snow, gravel or water contact exists in the capture set at
      all. `surf_soft` covers them until someone records on snow
- [x] **The scrape.** Was: `tangent_speed` has never seen a real slide, and the whole `ScrapeLoop`
      path rests on an untested column. It no longer does. Tangent speed opens a slide and nothing
      else: the level, the pitch, the distance clause and all three exits are measured off the
      body. The devbench takes exercise it — `Proventus_..._cut_2` runs 8 slides in six seconds,
      seven of them ending because the body bounced back into the air and one because something
      stopped it at 303 u/s
- [?] **Character-on-character.** Three takes tried, all three missed. No data
- [?] **Frame rate.** Every take ran at 48–51 fps, so frame-rate invariance is inherited from a
      deleted dataset, not confirmed. Two capped-fps runs would settle it
- [x] **The corpus carries pose.** `Research/NewRecordings` holds six takes, each with a
      `_pose.bin` sidecar. The two clauses that were untested code — the hero test's **arrival**
      clause and the `Tumble → Airborne` edge — run for real on them, and so does everything the
      slide rework reads. The 10 m fall is the highest-value capture outstanding again
- [ ] **The slide-end impact wants ears, not a check.** `Hero:fSlideEndFrac` at 0.30 opens a hero
      moment on a body still doing 288 u/s when something stops it, and the corpus says that is
      roughly one slide in eight. Whether an impact belongs there at all, and how loud, is the one
      part of the rework a verifier cannot answer: `bSlideEndImpact` is the A/B

## Decided, so nobody reopens them

- Vocal layer is **not in v1** — the slots stay declared and unfilled
- **No death-versus-knockdown distinction.** A death ragdoll simply never leaves `Rest`
- Gear (sheathed weapons, shields) is **out of scope**
- Vanilla body impacts are **suppressed**, globally, and that belongs in the mod description
- The rate cap is **global at ~46 ms**, not per-limb
- Layer offsets are **structured with a few ms of scatter**, not random jitter
