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

- [x] `ConfigSchema.cpp` — 124 `ParamDesc` rows over every field of both config structs
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

**Verify at shipping defaults: 71 of 89 checks pass over the 12 takes.** Determinism, the rate cap,
the burst shape, the settle and the sub offset pass everywhere there is data. The three families that
fail are written up in the handover; two of them are the design disagreeing with itself rather than
the code disagreeing with the design.

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
- [x] `--verify` / `--smoke` / `--take` / `--play` / `--config` CLI
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
      no grit, and the `ScrapeLoop` path it feeds is untested by any capture anyway
- [ ] `grunt_impact` / `scream_big` stay declared and unfilled — adding voice later is a config change

## Open questions from the research

- [?] **The get-up window.** `phase.getUpBlendMs` is a guess. Every capture take was paralysed, so
      nothing measures it. Needs one unparalysed take. Does not block v1 — death ragdolls never get up
- [?] **The intensity ceiling.** `intensity.speedRefHigh` = 960 u/s, but the take that would have
      established it was discarded. The docs quote it inconsistently. The loudness curve calibrates
      against this, so it is worth resolving. The 10 m fall recapture is the highest-value one outstanding
- [?] **Natural ground.** No dirt, grass, snow, gravel or water contact exists in the capture set at
      all. `surf_soft` covers them until someone records on snow
- [?] **The scrape.** `tangent_speed` has never seen a real slide — the take named `slide` was an
      extreme push. The whole `ScrapeLoop` path rests on an untested column
- [?] **Character-on-character.** Three takes tried, all three missed. No data
- [?] **Frame rate.** Every take ran at 48–51 fps, so frame-rate invariance is inherited from a
      deleted dataset, not confirmed. Two capped-fps runs would settle it

## Decided, so nobody reopens them

- Vocal layer is **not in v1** — the slots stay declared and unfilled
- **No death-versus-knockdown distinction.** A death ragdoll simply never leaves `Rest`
- Gear (sheathed weapons, shields) is **out of scope**
- Vanilla body impacts are **suppressed**, globally, and that belongs in the mod description
- The rate cap is **global at ~46 ms**, not per-limb
- Layer offsets are **structured with a few ms of scatter**, not random jitter
