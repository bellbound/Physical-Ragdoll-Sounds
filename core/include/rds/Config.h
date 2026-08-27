#pragma once

// The two config objects, as plain structs.
//
// Object-based rather than key-value: the engine reads `cfg.arb.rateCapMs`. The
// ini file and the testbench's sliders both come off the description table in
// ConfigSchema.h, so a new parameter is one field plus one line of schema.
//
// Everything here must stay standard-layout and trivially copyable: the schema
// addresses members by offset, and the testbench swaps whole configs between
// audio callbacks by copying the struct.
//
// Where a default came from a measurement the comment says which one.

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "rds/SlotManifest.h"
#include "rds/Types.h"

namespace rds {

// ═════════════════════════════════════════════════════════════════════════════
// RagdollSounds.ini - general mod settings
// ═════════════════════════════════════════════════════════════════════════════

enum class LogLevel : std::int32_t { kTrace = 0, kDebug, kInfo, kWarn, kError, kOff };

/// Vanilla plays a body impact on every ragdoll contact and resolves nearly
/// every surface to the same dirt sample. Where we play the collision ourselves
/// that is a double with half the mix out of our control, so there we drop it -
/// and only there, claimed per actor per tick by the actor's phase.
/// VanillaGate.h holds the rule; VanillaSuppression.h the global fallback for a
/// runtime where the per-call hook cannot be placed.
struct SuppressionConfig {
    bool suppressVanillaBodyImpacts{true};

    /// How far from a claiming actor a body impact still counts as theirs, in
    /// world units. The play carries a world point and no reference (see
    /// VanillaGate.h), so this is what turns "an actor is down here" into "that
    /// collision was one of theirs". 150 is a bit over two metres: it reaches a
    /// flung arm and still leaves a bystander their own footsteps.
    float suppressionRadius{150.0f};
};

/// The two engine-side numbers that cannot be guessed from here: the right
/// answer is a property of the load order rather than of the design.
struct AudioConfig {
    /// The BGSSoundOutput every voice of ours is opened with, as a form id.
    ///
    /// LOAD-BEARING. Without one a sound is a flat 2D voice with no falloff, no
    /// reverb send and no VR spatialisation. We run no reverb of our own, so this
    /// record's send is the whole of how wet we sound.
    ///
    /// Default is Skyrim.esm's `SOMMono02000` (0x0007F80C): 3D/HRTF, 150-2000
    /// units, 30% send - the dry half of the pair vanilla's body impacts use. The
    /// wet half rang long after the body stopped. The log lists candidates at
    /// debug level.
    std::int32_t outputModelFormId{0x0007F80C};

    /// The model taps are opened with instead, or 0 for the same one as everything
    /// else. Reverb send is a property of the record, not a knob on the voice, so
    /// "this layer drier than that one" can only be said by opening under a
    /// different model. 0 today because both ends of the pair sit at 30% send.
    ///
    /// Per voice and therefore per composite, never per layer. See
    /// GameRenderer::Update for what a group must be for this to apply.
    std::int32_t tapOutputModelFormId{0};

    /// DirectInput scancode that fires one canned impact composite at the player.
    /// 0 is off, the shipping value. It exists so the renderer can be proved on
    /// its own, without the whole contact pipeline in front of it.
    std::int32_t testCueKey{0};
};

/// The testbench link. Off in a shipping install: it opens a loopback socket,
/// streams every contact out of the process, and lets another program replace
/// the algorithm config and sfx table live. With `enabled` false the plugin
/// makes no socket call at all. Failure is always silent - the testbench not
/// being up is the ordinary case.
struct DevbenchConfig {
    bool enabled{false};

    /// Loopback only. Nothing here is reachable from another machine.
    std::int32_t port{27860};

    /// Where OBS Studio lives, so the testbench can start it. Read by the
    /// *testbench*, which loads the same file; the game never launches anything.
    /// Here rather than in the algorithm ini because that one is A/B'd and this
    /// is a property of the machine.
    char obsPath[260]{};
};

/// What the mod costs the frame it runs on, measured in the game. The testbench
/// times `RunOffline` and nothing else, so it cannot see the two halves that exist
/// only in the game: publishing a tick, and handing cues to Skyrim's sound
/// manager. It also reports a worst frame rather than a whole-take average.
///
/// Off in a shipping install: one bool test per span, no clock read.
struct BenchmarkConfig {
    bool enabled{false};

    /// How many knockdowns to measure before reporting and switching off for the
    /// session - the figure wanted is what an ordinary knockdown costs.
    std::int32_t knockdowns{2};

    /// Ceiling on frames sampled, so a ragdoll that never lets go cannot hold the
    /// report for ever; reached, it reports what it has and says it was cut short.
    /// 20000 is ~3.5 minutes of continuous ragdoll at 90 Hz.
    std::int32_t maxFrames{20000};

    /// Frames over this are counted separately: a mean says what the mod costs,
    /// this says whether it ever spiked. 250 us is a fortieth of a 90 Hz frame -
    /// far above anything measured (0.3-2.8 us per tick) and far below anything a
    /// player could feel.
    float slowFrameUs{250.0f};
};

/// Deliberately thin. The design's decisions all live in the algorithm file;
/// this one is the handful of things a user touches when something is wrong.
struct GeneralConfig {
    /// Master switch. Off means the plugin loads, logs that it is off, and hooks
    /// nothing - so it can be disabled without uninstalling.
    bool enabled{true};

    /// kInfo is the shipping level and is meant to be liberal: init, config
    /// deltas, slot resolution, one summary line per knockdown. kDebug is the
    /// per-contact firehose and is for development.
    LogLevel logLevel{LogLevel::kInfo};

    /// Rotate the log on startup instead of truncating it, so the log from the
    /// session that crashed survives the next launch.
    bool enableLogRotation{true};

    /// Rotated logs to keep. 0 keeps all of them.
    std::int32_t maxLogFiles{5};

    SuppressionConfig suppression;
    AudioConfig audio;
    DevbenchConfig devbench;
    BenchmarkConfig benchmark;
};

// ═════════════════════════════════════════════════════════════════════════════
// RagdollSounds_Algorithm.ini - the sound generation engine
// ═════════════════════════════════════════════════════════════════════════════

// ── Game integration ─────────────────────────────────────────────────────────

/// What the mod is allowed to hear, and when. The only section that asks about
/// the *game* rather than about sound.
///
/// The mod assumes an actor makes these noises while ragdolling and at no other
/// time - which is why a village of walking NPCs costs nothing, since the
/// ragdoll bodies collide the whole time an actor is animated. This section
/// takes the assumption away. Nothing below is reachable with `animatedMode`
/// off, which keeps the recorded corpus replaying byte-identically.
struct GameIntegrationConfig {
    /// Bypass every ragdoll check, in the feed and the engine both: track any
    /// actor in the cull radius, let the contact callback through whatever the
    /// phase, and admit `kAnimated` contacts at ingest. An experiment, not a
    /// shipping mode - ordinary movement is a great deal of collision and every
    /// rule downstream was tuned against falls.
    bool animatedMode{false};

    /// Whether the sliding grind may run while the actor is animated. A dragging
    /// foot is the likeliest false slide in ordinary walking, and a grind is a
    /// loop - it holds until the motion axis leaves `kSlide`.
    bool animatedSlide{true};

    /// Whether the cloth rustle may run while the actor is animated. This is the
    /// one layer that arguably belongs in normal gameplay - a body walking is a
    /// body whose clothes are moving - and it is the reason the pose is still
    /// published for an animated actor when any of these three is on.
    bool animatedRustle{true};

    /// Whether air time may be measured while the actor is animated: a jump, a
    /// fall off a ledge that never knocks them down. Off, `FlightFor` returns
    /// nothing and every rule that ramps on air time sees a grounded body.
    bool animatedAirTime{true};

    /// How long an animated actor may go without an admitted contact before the
    /// engine lets go. A knockdown ends on `ragdoll_end` and resets the burst and
    /// hero budgets; animated mode has no such edge, so without this an actor
    /// acquired on their first footstep is held until they leave the cull radius
    /// with a hero budget that ran out minutes ago. Only consulted for an actor
    /// animated *now*, so a quiet stretch mid-fall cannot end a knockdown early.
    float animatedIdleReleaseMs{3000.0f};
};

// ── Stage 0: Ingest ──────────────────────────────────────────────────────────

struct IngestConfig {
    /// Contacts below this never enter the pipeline. The capture's own floor was
    /// 5 u/s and produced a great deal of nothing; 20 is where the data's
    /// blow-up check first has anything to disagree about.
    float minImpactSpeed{20.0f};

    /// How far the floor above comes down for a contact that is sliding rather
    /// than arriving.
    ///
    /// The floor gates on *closing* speed and a clean flat slide has almost none.
    /// Measured on Nazeem_devbench_1: 686 of 2332 ragdoll contacts sit under the
    /// floor, 89% grazes by ratio, 43% carrying over 200 u/s of tangent.
    ///
    /// Gates on the contact's own tangent speed, not on whether a slide is running
    /// - which ingest cannot honestly know (01 §7.4). It only ever *admits*.
    /// **1.0 disables it outright** and is the default.
    float slideFloorFrac{1.0f};

    /// Tangent speed at which `slideFloorFrac` is fully applied. The relief
    /// ramps from `minImpactSpeed` up to here, so a contact barely moving
    /// sideways gets none of it and one skidding at speed gets all of it.
    float slideFloorAtTangent{300.0f};

    /// A row where the solver's closing speed and our reconstruction of it
    /// disagree by more than this fraction is a row the rigid-body arithmetic
    /// cannot reproduce - which is what a blow-up is (07 §2). Compare
    /// magnitudes: the sign is wrong on 23 % of good rows.
    float blowupDisagreeFrac{0.10f};

    /// Backstops, only for when normalSpeed is absent. Both of the old guards
    /// were far too tight - 700 u/s rejects two clean takes, 25 rad/s rejects
    /// 8.3 % of good rows. These are where failures actually become common.
    float blowupSpeedCeiling{1000.0f};
    float blowupAngularCeiling{200.0f};

    /// Half of all contacts are one limb touching another limb of the same body.
    /// An arm brushing your own thigh makes no impact sound, so a self-contact
    /// below this is dropped at ingest; above it, a genuine self-hit gets
    /// through.
    float selfContactThreshold{250.0f};

    /// Every self-collision fires twice - 624 of 624 ordered pairs had their
    /// mirror in the same frame. Drop the copy whose own body sorts second.
    bool dropMirroredSelfContacts{true};

    /// A gap larger than this starts a new frame bucket. Measured: the median
    /// gap inside one frame's batch of callbacks is 1.0 us, between batches
    /// 20.4 ms. 2 ms separates them with three orders of magnitude to spare.
    float frameGapMs{2.0f};

    /// Publish every ragdoll limb's position and velocity once every N ticks -
    /// the only measurement of where the body actually is. Without it air time is
    /// inferred from gaps between contacts, which reads a body that has merely
    /// stopped touching anything as airborne.
    ///
    /// 1 is every tick and is what the engine wants; higher decimates, 0 puts it
    /// back on the inference. A knob because it trades capture size against how
    /// well a fall can be measured.
    std::int32_t bodySampleEveryNTicks{1};

    /// Contacts on one (limb, other body) pair inside a frame collapse to their
    /// max. Do not use manifoldFirst/manifoldLast to bracket this - the flags
    /// do not pair up.
    bool collapseManifolds{true};

    /// tangentSpeed / impactSpeed above which a contact is a graze rather than a
    /// thud, and feeds the scrape path instead of the impact path. Grade B as an
    /// interpretation: nothing in the capture set is actually a slide.
    float grazeRatio{1.5f};

    /// Closing speed above which nothing is a graze, whatever the ratio says: a
    /// body arriving this fast is being hit by the world and the sideways
    /// component is something it does as well, not instead. Without it a fast
    /// skid classifies as a scrape and the impact path never sees the loudest
    /// contacts in the take.
    float grazeMaxImpactSpeed{220.0f};
};

// ── The modifier contract ────────────────────────────────────────────────
//
// The pipeline is declared in four stages with a contract each:
//
//   Admit   veto, speed floor            nothing today
//   Shape   intensity, onsetGainDb       glancing, head air, body air
//   Budget  grains, gaps, caps, masking  the hero moment
//   Trim    loudness only                post-intensity, role and file trims,
//                                        the motion/hero trim, the compressor
//
// Two invariants:
//
//  - **Anything that changes rank is bounded, and never touches `rawIntensity`.**
//    A level added before arbitration is also a rank, and Stage 2 must read the
//    untouched figure or a lifting rule walks the actor into a different motion
//    state and quietens everything after it.
//  - **Anything at Trim cannot change what was chosen** - slot, layer balance and
//    pitch are all decided by then.

/// What one Shape-stage rule may move.
///
/// `maxLevelDb` bounds where the contact *lands*, not how much is added -
/// `gainDb` already bounds that. Zero is the natural ceiling, since `onsetGainDb`
/// tops out at 0 dB for the hardest contact the engine can hear. Only a lift is
/// capped; a cut is left alone.
struct ShapeLift {
    float intensity{0.0f};   ///< class and layer balance, and therefore rank
    float gainDb{0.0f};      ///< level and rank, before arbitration
    float maxLevelDb{0.0f};  ///< where the lift may land
    float trimDb{0.0f};      ///< loudness only, after arbitration
};

/// What one Budget-stage rule may waive. Never a level, for the reason above.
///
/// Both fractions are *how much is waived*, so 0 asks for nothing and 1 waives
/// the rule entirely. They must point the same way: `Grant` combines two asks by
/// taking the more generous.
struct BudgetWaiver {
    float burstGapFrac{0.0f};
    /// How much of the global rate cap is waived. A scale rather than a switch:
    /// contacts on successive frames land 18 ms apart and read as a cluster,
    /// contacts in the same frame land 0.1 ms apart and merely sum (+6 dB) and
    /// clip. Nothing in the corpus lands between 1 and 17 ms, so a scaled cap in
    /// that gap keeps every cluster and rejects every stack.
    float rateCapFrac{0.0f};
    bool resetsBurst{false};
};

// ── Stage 2: the motion axis ─────────────────────────────────────────────────

/// What one state is allowed to spend. The trim is in dB against the actor's
/// mix, so a nearly silent resting phase is one number rather than a special
/// case in the arbitrator. Used by both axes - a motion state has one and so
/// does the hero latch, and `BudgetFor` picks between them.
struct PhaseBudget {
    float gainTrimDb{0.0f};
    std::int32_t maxCuesPerBurst{4};
};

struct MotionConfig {
    /// How long off the ground before Airborne is believed. (Height is the wrong
    /// question - the ground reference is the last floor contact, stale the
    /// moment a body starts travelling. `freeFallFrac` below needs no ground.)
    float airborneMinTimeMs{120.0f};

    /// How much of free-fall acceleration counts as unsupported. Gravity is
    /// 9.8 m/s^2 against 69.99 units/metre, about -686 units/s^2. Measured at
    /// -675 across the real fall in Vayne_impacts_log_2_cut_4, and +229 across
    /// that take's opening scuff - which the old air-time rule scored as fully
    /// airborne.
    ///
    /// A fraction so a gravity mod moves one value. Steeper than free fall still
    /// counts: a body driven downward has nothing holding it up either.
    float freeFallFrac{0.55f};
    float gravityUnitsPerSec2{686.0f};

    /// How long the acceleration has to look like free fall before the flag is
    /// believed, and how long it survives a tick that disagrees. One noisy
    /// sample either way is a solver artefact, not a landing.
    float freeFallMinMs{40.0f};
    float freeFallHoldMs{60.0f};

    /// How long nothing may have touched the body before Tumble hands it back to
    /// Airborne - hysteresis between the two motion edges.
    ///
    /// Without it the edges take turns one per tick: `Airborne` drops on any
    /// touch, `Tumble` returns on the acceleration latch alone, which stays on
    /// through a bounce train. Measured on Eldawyn_devbench_1 alternating every
    /// tick for 300 ms, putting a 6 dB square wave on both loops.
    ///
    /// **Gates the motion label only** - `airborne`, `AirTimeMs()`,
    /// `fallDropUnits` and `driven` are untouched. 0 restores the old edge.
    float landedHoldMs{140.0f};

    /// The time constant on the motion trim as the bed hears it. The budget is a
    /// step (-3 dB tumbling, -6 sliding, -9 airborne), which is right for a
    /// one-shot and wrong for a loop held open across the state change.
    ///
    /// Bypass proposals only. Contact-derived values still take the discrete step,
    /// and `maxCuesPerBurst` is never smoothed. 0 restores the step exactly.
    float bedTrimGlideMs{90.0f};

    /// Whether a flight something is pushing is treated differently from one that
    /// is only falling. Off, the residual below is still measured (a subtraction
    /// and a square root per tick) but nothing acts on it.
    bool drivenEnabled{true};

    /// How far the measured acceleration may sit from gravity before the body
    /// counts as *driven* rather than falling. A body nothing is touching
    /// accelerates at exactly gravity; anything else means something is pushing,
    /// and the engine deliberately does not ask which mod did it.
    ///
    /// Measured on a leash yank in Proventus_Avenicci_devbench_3: the genuine fall
    /// sits at 197-284 u/s^2 of residual, a body being hauled at 900-1600. A body
    /// on the floor reads ~686 (the ground is a force too), which is why this is
    /// only consulted while free-fall already holds.
    ///
    /// Zero would call every flight driven, so it is treated as off; the switch
    /// is `drivenEnabled` and this is only the threshold.
    float drivenResidual{450.0f};
    /// Kept true this long past the last driven tick, so one quiet frame in the
    /// middle of a sustained pull does not read as the flight going ballistic.
    float drivenHoldMs{80.0f};

    /// How long after a flight ends a contact still counts as its landing.
    ///
    /// A landing rule asks about a flight that is *over*, and the flag has
    /// usually dropped by the time the arrival is judged. Reading the live clock
    /// instead is wrong at both ends: it pays out on mid-air clips and reads zero
    /// on the one contact the rules exist for.
    ///
    /// Two to six frames: longer than the flag's own hysteresis, shorter than a
    /// corpse lying still still being paid for the fall that put it there.
    float landingWindowMs{120.0f};

    // ── the slide ────────────────────────────────────────────────────────
    //
    // Entry is measured from the contacts, exit from the body. A graze is a good
    // signal that a slide has started and a bad one that it is still going, since
    // collisions are dense when a fall is busy.
    //
    // Two exits: the body left the ground (`airborne`), or the graze stream dried
    // up while it was still on it (`SlideExit::kEnded`). The second says nothing
    // about *why*. `slideHoldSpeed` is the one thing that may put it off.

    /// Sustained tangential contact for this long opens Slide.
    float slideMinTangentSpeed{120.0f};
    float slideMinDurationMs{150.0f};

    /// ...or this far travelled along the surface, whichever comes first. Time
    /// alone misses a fast skid: devbench_3 crosses two metres of floor inside
    /// the 150 ms the time gate wants.
    ///
    /// Measured off the centre of mass where a take carries pose, integrated from
    /// tangent speed where it does not. The contact *point* is unusable - it hops
    /// between manifold points and limbs, and its displacement is mostly jitter.
    float slideMinDistance{45.0f};

    /// How long the graze stream may dry up before the slide is over. Which exit
    /// it took is decided by the body, not by the silence; this only says that a
    /// slide with no grazes in it at all has stopped being one.
    float slideGraceMs{140.0f};

    /// The exit speed, asked of the **body** - the counterpart to the entry gate
    /// `slideMinTangentSpeed` (re-applying that one ends slides on the contact
    /// hold's decay curve). Over it the slide outlives its own grazes; under it
    /// `slideGraceMs` has the last word, so set under `slideMinTangentSpeed` the
    /// two are hysteresis.
    ///
    /// Reads `bodySpeed` off the pose sidecar, so it does nothing on a take with
    /// none. Zero is off.
    float slideHoldSpeed{0.0f};

    /// ...and the longest a slide may be held that way with no grazes at all.
    /// Backstop for a pose stream that says the body is drifting when nothing is
    /// touching it. Counted from the last graze, so a slide with grazes in it
    /// never reaches this timer.
    float slideHoldMaxMs{2000.0f};

    // The slide-end lift and its `[Hero]` clause are gone: they stood in for the
    // exit being inferred from the contact stream drying up. `slideHoldSpeed`
    // measures the body, so a grind ends when the body does and the contact that
    // ends one is ordinary (01-Architecture §7.4).

    PhaseBudget launch{-6.0f, 2};
    PhaseBudget airborne{-9.0f, 1};
    PhaseBudget tumble{-3.0f, 4};
    PhaseBudget slide{-6.0f, 2};
};

// ── Stage 2: the moment axis ─────────────────────────────────────────────────

/// When the mix is allowed to have a hero moment, and what one is worth.
///
/// Everything here is measured on raw `impactSpeed`, not intensity: intensity
/// clamps at 1.0, so above `speedRefHigh` every contact reads the same and the
/// test meant to find the biggest moment of a fall was blind at the top of its
/// own range. Closing speed is unclamped and 608 u/s is plainly bigger than 294.
/// (The old `frameEnergy >= 0.35 * energyAccum` test against an ever-growing
/// total was structurally unsatisfiable after the opening of a fall, and spent
/// the moment on a 44.7 u/s thigh scuff at 696 ms in Vayne_impacts_log_2_cut_4.)
struct HeroConfig {
    /// Off, the moment axis stays Ordinary for ever and every budget comes from
    /// the motion state - which is the old machine minus its hero phase.
    bool enabled{true};

    /// The absolute floor, as a fraction of `IntensityConfig::speedRefHigh` - a
    /// fraction so it stays true when the anchor moves (00-Design §6). At the
    /// shipped anchor of 960 this is 288 u/s: above the 44.7 scuff that used to
    /// steal the moment, below the 294 head that should have had it.
    float floorFrac{0.30f};

    /// Dominance: how much louder than the decaying recent peak a contact must be
    /// to be the event rather than part of one. Measured against the same
    /// peak-hold the settle rule reads, taken from before this tick's contacts
    /// were folded in - a contact cannot be 1.3x itself.
    float dominanceRatio{1.30f};

    /// Arrival: a contact landing out of a genuine measured flight this long is a
    /// landing whatever else is going on. Only available on a take that carries
    /// pose - the old inference reads a take's first contact as maximally
    /// airborne, since nothing had ever touched.
    ///
    /// 250 ms sits between the two sprawls in Vayne log_2 (16 and 35 ms) and its
    /// dive (597 ms) with room on both sides.
    float arrivalMinAirMs{250.0f};
    /// ...and had come down at least this far, in units. Zero asks nothing of
    /// the drop and judges on flight time alone.
    float arrivalMinDropUnits{0.0f};

    // The slide-end clause went with the lift it rode: it was a way in for the
    // contact dominance could not judge fairly at the end of a graze stretch.
    // `Motion:fSlideHoldSpeed` keeps a slide open on the body's own speed, so
    // whatever hits at the end is an ordinary contact into an ordinary quiet
    // stretch, which is what dominance is for.

    /// How long the latch holds. The references' hero moments are a small group
    /// of peers inside a couple of hundred ms, not one hit - a faceplant
    /// genuinely has a knee, a chest and a head.
    float windowMs{220.0f};

    /// A contact this many times the open window's anchor speed re-anchors it:
    /// the window restarts, the burst budget resets and the collapse point moves.
    /// Re-anchoring rather than opening a second moment is what makes a landing
    /// read as one event with peers. It is also why the moment tracks its peak
    /// rather than its first grain - log_2's 608.7 u/s slam arrives 132 ms after
    /// the 294 u/s head that opened the window.
    float reanchorRatio{1.15f};

    /// What a hero moment is worth. Overrides the motion state's budget outright
    /// while the latch is open - `BudgetFor` is the one place that is decided.
    PhaseBudget budget{0.0f, 5};

    /// Opening or re-anchoring closes whatever burst was open and starts the
    /// moment's own with the grain count at zero. What a long fall most often
    /// lost to was not a gate but a burst some scuff had opened and filled three
    /// hundred milliseconds earlier, while the body was still in the air.
    bool resetsBurst{true};

    /// Fraction of `ArbitrationConfig::burstMinGapMs` waived for the contacts of
    /// a hero moment, so the landing is not dropped for arriving too soon after
    /// the burst it just closed. 1.0 waives it entirely.
    float burstGapFrac{1.0f};

    /// The burst window a moment's own burst runs on, in ms. 0 uses
    /// `ArbitrationConfig::burstWindowMs`, the default.
    ///
    /// The two want opposite things. An ordinary burst wants a *wide* window:
    /// shorter than the spread of a fall's contacts and it shuts before the loud
    /// one lands, which is how two taps at 0.03 and 0.07 intensity locked out a
    /// 465 u/s hit seventy ms later. A hero burst wants a *tight* one - its grains
    /// arrive together, so the window's job is to close before whatever comes next
    /// smears the crash into a roll (00-Design §7).
    float burstWindowMs{0.0f};

    /// How much of `ArbitrationConfig::rateCapMs` is waived inside the window, so a
    /// moment's peers may cluster. Outside one the 46 ms cap spaces onsets evenly,
    /// which reads as a metronome; the reference rhythm is three or four grains
    /// inside 20-40 ms then silence (00-Design §7).
    ///
    /// 0.75 of 46 ms is ~11 ms, in the empty part of the measured distribution:
    /// successive-frame contacts at 18 ms survive, same-frame ones at 0.1 ms do
    /// not. Towards 1.0 it walks back into stacking.
    float rateCapFrac{0.75f};

    /// Hero moments per knockdown. **0 is unlimited**, which is the default and
    /// the whole point of the rework: a body bouncing down a staircase has more
    /// than one real landing in it. Set it to 1 to get the old machine's "one
    /// hero per fall" back.
    std::int32_t maxPerEvent{0};
};

// ── Stage 4: Arbitration ─────────────────────────────────────────────────────

struct ArbitrationConfig {
    /// No two impact onsets closer than this, regardless of limb. The floor in
    /// three independent reference clips, and about where hearing stops
    /// resolving two impacts as separate. Global, not per-limb.
    float rateCapMs{46.0f};

    /// ...unless the newcomer is this much louder than the onset holding the cap,
    /// in which case it opens its own. The cap assumes close onsets add mud
    /// rather than detail, which only holds between comparable levels: a body
    /// slamming down 42 ms after a foot and 7 dB above it is a second event.
    float rateCapOverrideDb{6.0f};

    /// Contacts on one limb chain inside this window collapse to one cue at the
    /// strongest - a strong hand impact silences the elbow and plays one arm flop.
    float chainMergeWindowMs{60.0f};

    /// A decaying loudness ceiling per actor. Anything proposed more than
    /// `maskDropBelowDb` under it is dropped entirely, not played quietly -
    /// which is what turns a dozen simultaneous contacts into one event with
    /// texture.
    float maskDropBelowDb{12.0f};
    float maskDecayDbPerSec{60.0f};

    /// Bursts of three to five grains inside 200-400 ms, then real silence.
    /// The measured rhythm is 46-104 ms inside a burst, 313-894 ms between.
    std::int32_t burstMaxGrains{5};
    float burstWindowMs{300.0f};
    float burstMinGapMs{300.0f};

    /// During a hero moment place every layer at one point: several points read
    /// as several events, one point reads as one event with detail. Confirmed by
    /// the references' 0.95-0.97 stereo correlation.
    bool spatialCollapseOnHero{true};
    float spatialCollapseWindowMs{200.0f};

    /// Windows scale with frame time so the system behaves the same at 24 and
    /// 144 fps: every window is max(k * frameTime, the floor above).
    float frameScaleK{2.0f};

    // ── priority: rank without loudness ──────────────────────────────────────
    //
    // A `Weight` is added to `Proposal::priorityDb` and nothing else: it moves a
    // contact up the arbitrator's sort and the rate cap's override comparison, and
    // is invisible to the mix.
    //
    // `levelDb` answers "how big was this" and the arbitrator was using it for
    // "which of these matters". Over the thirteen devbench takes 398 torso
    // proposals were dropped with a limb holding the window, 63 of them to a limb
    // that was *quieter* on closing speed, another 67 by under 3 dB.
    //
    // Defaults are 0. 3 dB of torso weight keeps the ~130 cases where the arm was
    // barely ahead and leaves the ~270 where it really was the event; a weight is
    // not a veto, so at 3 dB a limb genuinely 6 dB louder still wins. Binned
    // through `DamageSiteFor`.
    float torsoWeightDb{0.0f};  ///< spine, COM and the neck - the column
    float headWeightDb{0.0f};   ///< the skull. `HeadImpact:bHeroFloorRelief` is already a partial one of these
    float limbWeightDb{0.0f};   ///< arms, legs, and anything off a skeleton we could not read
};

// ── Intensity: speed to loudness ─────────────────────────────────────────────

/// The same shaping again, applied after the layers have been chosen.
///
/// Intensity decides both what a contact is *made of* and how loud that comes
/// out, so widening the dynamic range because knockdowns are too quiet also
/// re-balances every composite. These are the same three numbers re-applied at
/// Stage 5, where loudness is the only thing left to change.
///
/// Every default is neutral. Contact-derived cues only.
struct PostIntensityConfig {
    /// Added to the dynamic range for the level only. Positive widens the gap
    /// between the quietest contact and the loudest without touching what either
    /// one is built from; negative closes it up.
    float extraRangeDb{0.0f};

    /// Where the level sits between the two anchors, on top of the curve
    /// intensity already applied. 1.0 changes nothing.
    float curveExponent{1.0f};

    /// Compresses the top of the level range only, so an absurd impulse gets
    /// loud rather than enormous while still being built as an obliterate. 1.0
    /// is off.
    float softClipKnee{1.0f};
};

struct IntensityConfig {
    /// The calibration anchors. An ordinary shove peaks at 355-543 u/s, a
    /// three-metre fall at 600-855, a ten-metre fall at ~960 - though that last
    /// take was discarded, so the ceiling is a guess (07 §11).
    float speedRefLow{20.0f};
    float speedRefHigh{960.0f};

    /// The whole range maps onto about 35 dB. The references span 13-17 dB
    /// across their onsets with the bed 30-36 dB under the hero hit; a naive log
    /// curve would give 60 dB and sound wrong at both ends.
    float dynamicRangeDb{35.0f};

    /// Curve shape between the anchors. 1.0 is linear in dB.
    float curveExponent{0.75f};

    /// Other mods' impulses arrive far outside anything physical. Soft-clip
    /// rather than reject: a silent obliterate is the worst possible outcome.
    float softClipKnee{0.85f};

    /// How much the limb's own size moves the intensity, using our nominal mass
    /// table and objectRadius rather than the solver's asymmetric masses.
    float massWeight{0.35f};
    float radiusWeight{0.20f};

    /// Above this the contact is an obliterate, as a multiple of `speedRefHigh`.
    /// Sits above anything a fall can produce. Relaxes the damage limits rather
    /// than gating under them - see `DamageConfig::obliterateBudgetBonus`.
    float obliterateFrac{1.46f};

    PostIntensityConfig post;
};

/// How much mass is behind a contact.
///
/// `NominalMass` says how heavy a limb is *on its own*, but a body landing
/// feet-first delivers the whole body's momentum through the foot - the foot is
/// the doorway, not the thing coming through it, and pricing the doorway made
/// log_11's foot landing 23 dB quieter than its hip landing at the same speed. A
/// foot clipping a wall while the body sails past really is only the foot, so limb
/// identity cannot answer the question.
///
/// What can: **how much of the limb's motion is translation rather than
/// rotation.** On log_11 the foot landing is 536 u/s at 1.8 rad/s and the hip 563
/// at 5.2, both the body arriving, while the calf whipping past is 451 at 25.0 and
/// the loose foot 168 at 42.6.
struct EffectiveMassConfig {
    /// Off by default so it can be A/B'd. On, loudness follows how much mass is
    /// arriving; off, which limb happens to be touching.
    bool massDeterminesLoudness{false};

    /// The mass a fully coupled contact is worth, on NominalMass's scale (the
    /// reference is 2.5, the trunk 7.5). Every coupled contact gets it
    /// regardless of limb - which is the entire point.
    float bodyMass{5.0f};

    /// Rotation at the limb's surface (angularSpeed x limbRadius) this close to
    /// its linear speed counts as pure flail. Below it the limb is translating
    /// with the body and the coupling opens up.
    float rotationRefRatio{1.0f};

    /// How much of the coupling is allowed to move the mass. At 0 the toggle
    /// does nothing; at 1 a fully coupled contact uses bodyMass outright.
    float couplingWeight{1.0f};

    /// A limb moving slower than this is not delivering anything however
    /// straight its path is. Stops a slow settling contact being priced as a
    /// body landing.
    float minBodySpeed{120.0f};
};

// ── Stage 3: the six strategies ──────────────────────────────────────────────

/// The core. Every audible impact is a timed stack, and the biggest part of it
/// arrives late - that late sub is the single biggest contributor to the gnarl.
struct ImpactCompositeConfig {
    bool enabled{true};

    /// Below this intensity a contact is burst filler and plays as a single tap
    /// rather than a four-layer composite. The quiet nine of every ten contacts
    /// live here. The only place a level decision changes a cue's *class*.
    float tapBelowIntensity{0.15f};

    /// Layer offsets from the contact frame, in ms. Measured in the references:
    /// transient at 0, body at +8..34, weight at +46..100, sub at +64..74.
    /// Structured, not random - random jitter smears the shape rather than
    /// building it.
    float transientOffsetMs{0.0f};
    float bodyOffsetMs{20.0f};
    float subOffsetMs{65.0f};

    /// A few ms of scatter so the composite envelope is never identical. Voices
    /// start on frame boundaries anyway, which quantises offsets to 7-20 ms.
    float offsetScatterMs{4.0f};

    /// Loudness comes from layer balance, not tiers: a light contact is mostly
    /// transient with almost no sub, a heavy one sub-dominant with the transient
    /// on top. Two endpoints per layer, interpolated by intensity.
    float transientGainAtMinDb{-5.0f};
    float transientGainAtMaxDb{-18.0f};
    float bodyGainAtMinDb{-8.0f};
    float bodyGainAtMaxDb{-2.0f};
    float subGainAtMinDb{-30.0f};
    float subGainAtMaxDb{0.0f};

    /// Pitch is free and continuous here, and beats doubling the bank. Random
    /// scatter per voice plus a downward bias with intensity, so a heavier impact
    /// starts lower and reads bigger.
    float pitchScatterSemis{2.5f};
    float pitchIntensityBiasSemis{-3.0f};

    /// Stay inside this or the pitch trick starts sounding like a pitch trick.
    float pitchMaxSemis{3.0f};
};

/// Air time: how long the body had been clear of the world when a contact
/// arrived, and what that is allowed to do to the sound.
///
/// A dive and a sprawl produce head contacts closing speed cannot tell apart -
/// Vayne log_2 has a 402 u/s spine-whip tip and a 294 u/s genuine faceplant - and
/// air time separates them: 16 and 35 ms for the sprawls, 597 for the dive.
///
/// Two halves spend it, both **Shape**-stage rules: the **head** half (gate, level
/// and crunch chance) and the **body** half, the same idea for every other limb,
/// where the difference between a limb that came down at the end of a fall and one
/// clipping the floor mid-tumble is air time and nothing else can see it. A third,
/// the budget reset, is gone - whether a landing is *heard* belongs to the moment
/// axis, so air time now buys a hero moment instead.
///
/// Every measurement comes from before this tick's contacts were folded in, so an
/// arm arriving with the head reads as part of the same strike.
struct AirTimeConfig {
    // ── The head half: the head arriving first, with the body still clear ────

    bool headEnabled{false};

    /// How long the body must have been clear of the world for the head's air
    /// time to count fully. The ramp is linear from zero to here.
    float headClearMs{250.0f};

    /// A hand is what you put out in front of a dive, so one touching down with
    /// the head is part of the same strike. Vayne log_2's faceplant lands its
    /// left hand in the same millisecond as its skull, and counting it collapses
    /// the air time to zero. Head measurement only - hands still count as company
    /// and for the body half.
    bool headExcludeHands{true};

    /// ...but only for this long: a *larger* value forgives more. Keeps a body
    /// that broke its fall on one arm, rolled, and clipped its head a second
    /// later from reading as a dive. Zero forgives nothing, so `headExcludeHands`
    /// then only discounts contacts inside the same frame bucket.
    float headHandGraceMs{200.0f};

    /// How much more willing the head gate is at full air time, on the same
    /// scale as `HeadImpactConfig::headDownBonus`.
    float headGateBonus{0.45f};

    /// Level added to the head accent at full air time.
    float headGainDb{9.0f};

    /// The ceiling that boost may lift the accent to - where it *lands*, not how
    /// much is added. The boost runs before arbitration, where a level is also a
    /// rank, so an uncapped one makes the skull outrank every other proposal in
    /// the frame. Zero is the natural ceiling (`onsetGainDb` tops out at 0 dB).
    /// Only a positive boost is capped; company damping runs after this.
    float headMaxLevelDb{0.0f};

    /// The post-arbitration half of the head boost, on the same ramp. Uncapped -
    /// a trim is not a rank - so it is the one to reach for when the accent is
    /// simply too quiet and the sort is already right.
    float headTrimDb{0.0f};

    // The head halo (four numbers that lifted the contacts *around* a led head)
    // is gone: structurally it was a peer group plus a spatial collapse
    // re-implemented per contact, and the moment axis has both. All four
    // defaulted to 0.
    //
    // One capability went with it: the halo added *intensity*, which moves layer
    // balance and the composite/tap class, while the hero window moves only
    // budget and trim. If a dive comes out thin, put that back as a lift on the
    // hero window rather than on the head.

    /// The crunch gate a head at full air time is held to, replacing that
    /// contact's `DamageTierConfig::atFrac` - a crunch on a slow dive without one
    /// on every fast sprawl. Only ever lowers the bar; zero leaves it alone.
    /// Moves *whether*, never *how loud*: the level ramp is still measured from
    /// the tier's configured threshold.
    float headCrunchGateFrac{0.55f};

    /// The chance such a head actually crunches. Flat rather than the tier's own
    /// ramp, which runs from the tier threshold up and would leave a dive that
    /// opened the gate below that threshold almost never firing. 1.0 is always.
    float headCrunchProbability{1.0f};

    // `headClaimsOnset` moved to `HeadImpactConfig::claimsOnsetOnHero`, fired by
    // the moment axis rather than by the head's own air-time ramp.

    // ── The body half: everything that is not the head ───────────────────────
    //
    // Was this limb falling, or already down? A thigh touching the floor 20 ms
    // after a shoulder is the second grain of a landing; the same thigh at the
    // same speed after 600 ms of air is the landing, and closing speed cannot
    // separate them.
    //
    // Hands count here, both as the peer that ends the air time and as a limb that
    // can be lifted by it. The head is excluded: it has its own half above.

    bool bodyEnabled{false};

    /// How long the body must have been clear for a limb's air time to count
    /// fully. Linear ramp from zero, so half way up gets half of everything.
    float bodyClearMs{250.0f};

    /// What a limb's air time buys, as a Shape lift. Intensity is the loud knob -
    /// it moves layer balance, pitch bias and the composite/tap class - so a
    /// small number here does more than a large one in dB. `rawIntensity` is
    /// untouched, so lifting a landing cannot walk the actor into a different
    /// phase and make everything after it louder.
    ShapeLift bodyLift{};

    // ── Budget reset: gone, absorbed by the hero window ──────────────────────
    //
    // Seven numbers handed a long-enough landing a fresh arbitration budget once
    // per knockdown - which is a second hero moment implemented in the
    // arbitrator. Its evidence is now `HeroConfig::arrivalMinAirMs` (measured
    // rather than inferred), its effect `HeroConfig::resetsBurst`,
    // `burstGapFrac` and `rateCapFrac`, and its cap `HeroConfig::maxPerEvent`,
    // which defaults to unlimited because a staircase really does have more than
    // one landing in it.
};

struct HeadImpactConfig {
    bool enabled{true};

    /// Head contacts get their own layer and gate, as a multiple of
    /// `IntensityConfig::speedRefHigh` rather than a speed - "the top third of
    /// what this mod ever hears" stays true when the anchor moves (00-Design §6).
    float gateFrac{0.31f};

    float gainDb{-2.0f};
    /// The post-arbitration half of the same voicing. `gainDb` is a rank as well
    /// as a level, so pulling down a hot `head_impact` wav there also costs the
    /// accent its place in the sort. Put voicing here instead.
    float trimDb{0.0f};
    /// Head-down attitude at the moment of contact makes the gate more willing.
    float headDownBonus{0.25f};

    // ── Company: the head arriving inside a pile ─────────────────────────────
    //
    // A head landing with four other limbs in the same frame and only 1.13x
    // faster than the fastest is the end of a lever - the loudness belongs to the
    // body. A head landing with one hand and 1.43x faster than it is the strike.

    bool companyEnabled{false};

    /// More other limbs than this hitting the world in the same frame and the
    /// contact is treated as part of a sprawl.
    std::int32_t companyMaxPeers{2};

    /// ...and the head must be at least this much faster than the fastest of
    /// them, or it is treated as a sprawl whatever the count says.
    float companyLeadFrac{1.30f};

    /// What a sprawl costs: the gate goes up by this fraction of itself...
    float companyGateFrac{0.20f};

    /// ...and the accent comes down by this much before arbitration, where the
    /// level is also the rank, so a sprawled head both sounds smaller and stops
    /// outranking the limbs the loudness actually belongs to.
    float companyDampDb{-9.0f};

    /// ...and by this much after it, where it is loudness alone. Damping purely
    /// here keeps the head's place in the sort while taking it down in the mix.
    float companyTrimDb{0.0f};

    // Budget refund - six numbers buying a hard head strike out of the
    // arbitrator's budgets - is gone: it wrote the same four proposal fields the
    // air-time reset did, and under the hero test the contact it existed for
    // (log_1's 537.8 u/s head, dropped on `burst gap`) clears the floor and the
    // dominance ratio comfortably.

    // ── Hero floor relief: the head as the start of a moment ─────────────────
    //
    // The moment axis's floor is limb-blind, and a faceplant is where that is
    // wrong: the strike a fall is *about* can sit under the floor, leaving the fall
    // with no hero moment at all. So a head over a threshold of its own gets the
    // floor lowered, for that contact only. The rule lives in `AdvanceMoment`
    // (01-Architecture §7.1) and is configured here because this is where a head is
    // tuned. Composes with `claimsOnsetOnHero`, which needs a head able to anchor.

    bool heroFloorRelief{false};

    /// How hard the head has to hit to earn it, as a fraction of
    /// `IntensityConfig::speedRefHigh` - the same scale as `gateFrac` and
    /// `HeroConfig::floorFrac` (00-Design §6). Defaulted to the accent's own
    /// gate: a head worth an accent is the natural candidate for a moment.
    float heroFloorReliefAtFrac{0.31f};

    /// ...and how much comes off the hero floor when it does, on the same scale,
    /// so the two subtract: a 0.30 floor with 0.10 relief is a head floor of
    /// 0.20. Clamped at zero.
    ///
    /// It also moves the dominance reference, which measures against
    /// `max(floor, energyRecentBeforeTick)` - that half is what lets a head
    /// *anchor* a moment rather than merely survive the floor test. Both halves
    /// are deliberate; see `AdvanceMoment`.
    float heroFloorReliefFrac{0.10f};

    /// A head impact that anchors a hero moment stops being an accessory and
    /// becomes its own onset. Normally the accent rides along with the composite
    /// and dies with it. The trigger is the moment axis rather than the head's own
    /// air-time ramp, which said nothing about whether the *mix* considered this a
    /// moment. It does not fix the underlying trap: moving a proposal between
    /// ride-along and onset still puts `fGainDb` on the arbitrator's sort.
    bool claimsOnsetOnHero{true};

    // Damage moved out: it used to be here plus a differently-shaped
    // `CrunchGoreConfig` for the rest of the body, two gates and two level ramps
    // over one shared budget. Whichever rule reached a contact first spent from
    // it, so switching the body's crunch on could silence the head's with no
    // counter moving to say so. Now one `DamageConfig` below, tuned three times,
    // with the budget split per part and per tier.
};

// -- Damage -------------------------------------------------------------------

/// One tier of damage - a crunch, or the gore above it - for one part of the
/// body.
///
/// Discrete: you cannot have thirty percent of a bone break, and one played
/// quietly sounds like a bug. A tier that should not be certain is softened with
/// *probability*, never volume; the level ramp underneath says how bad it was.
struct DamageTierConfig {
    bool enabled{true};

    /// Where the tier opens and where its ramps top out, both as fractions of
    /// `IntensityConfig::speedRefHigh`, so the tier structure moves with the
    /// range. Above the cap a harder contact has nothing more to say through this
    /// tier. A cap at or under the threshold is not an error - it is how you ask
    /// for a step instead of a ramp, and collapses to "full from the threshold
    /// up".
    float atFrac{};
    float capFrac{};

    /// The chance the tier actually fires, at its threshold and at its cap. Both
    /// default to 1. Pull `probAtGate` down and the bottom of the range becomes a
    /// *maybe* that firms up as the contact gets worse - reach for it when a tier
    /// fires too reliably to feel alive; pull `probAtCap` down too and the whole
    /// tier is a coin flip.
    ///
    /// A tier left at 1/1 draws no random number at all, so switching one part's
    /// damage on does not re-roll every variant and scatter after it.
    float probAtGate{1.0f};
    float probAtCap{1.0f};

    /// The level ramp, in dB against the contact's own onset level, over the same
    /// span the probability ramps over. The quiet end sits well down: crossing a
    /// threshold should not itself be an event.
    float quietDb{};
    float loudDb{};

    /// How long after the impact this lands, measured from the composite's own
    /// body offset so moving the stack moves it too. Not zero: a crunch is what
    /// the bone *did*, and reads as consequence when it arrives a beat late.
    float delayMs{};

    /// How many of these one knockdown may produce, and how close together.
    ///
    /// One budget per part per tier, never shared: a shared counter lets arrival
    /// order decide which layer you hear, and a limb crunch dropped on
    /// arbitration could spend the skull's slot on the way past.
    ///
    /// Resets on `Motion::kLaunch`, the only event boundary the engine has.
    /// `spacingMs` is the finer control: 0 is off, and any value stops one sprawl
    /// arriving as a burst of breaking sticks without capping a long tumble.
    std::int32_t budget{};
    float spacingMs{0.0f};
};

/// One part of the body, and what a contact on it is worth in broken bone. See
/// `DamageSite` for why the body is three parts and not nine.
struct DamagePartConfig {
    bool enabled{true};

    /// What speed the tiers here are judged on: 0 is the contact's own closing
    /// speed, 1 the actor's recent-energy envelope, anything between a blend.
    ///
    /// The honest answer differs per part: a skull is judged on its own arrival
    /// (hence 0 there), while a leg can snap in a slam it barely touched. The
    /// envelope has already taken the max with this frame's contacts, so the blend
    /// can only loosen a gate, never tighten one.
    float bodyForceShare{0.0f};

    DamageTierConfig crunch;
    DamageTierConfig gore;
};

/// Crunch and gore, for every part of the body, in one rule.
///
/// It replaces two - the head's deterministic level ramp and the body's
/// probability gate - and keeps both shapes: a tier at 1/1 probability *is* the
/// deterministic rule. What the merge removed was the second gate, the second ramp
/// and the shared budget.
///
/// Three parts, two tiers, independent: gore is not nested inside crunch, so a
/// contact bad enough to be wet still sounds wet on a frame where the crunch
/// budget is gone.

/// How much the violence of the fall itself moves the damage rule.
///
/// Not "how hard was this contact" - `impactSpeed` and `bodyForceShare` answer
/// that twice over - but *what kind of fall is this contact happening inside*.
///
/// **Windowed, never point-in-time.** Reading the thrash on the contact tick is
/// the failure 01 §7.4 records for the `driven` gate: a limb striking stone has
/// its velocity reversed inside one solver step, so that thrash *is* the contact
/// restated. `ActorRuntime` holds a peak-hold that decays every tick and rises
/// only on contact-free ticks - the free thrashing between collisions.
///
/// Off by default, with usable voicing already set, so `bEnabled` is the whole of
/// turning it on.
struct DamageViolenceConfig {
    bool enabled{false};

    // -- its own measurement, sharing nothing with the garment ---------------
    //
    // Same ramp shape as `[Rustle]`, deliberately not the same values. The garment
    // weights by cloth, times body speed, plus an airborne term - all wrong here,
    // since a naked body breaks as well as a clothed one and a long fall is not
    // itself an injury. Violence weights by `NominalMass`. Sharing them also meant
    // tuning the rustle moved how often bones broke.
    //
    // Always relative to the body: everything on a falling body accelerates at g
    // together, and free fall is not violence.

    /// The two raw signals, each through its own ramp and then blended: `thrash`
    /// is relative limb acceleration in u/s^2, `tumble` limb surface speed from
    /// rotation in u/s. See `[Rustle]` for why they cannot be added unnormalised.
    ///
    /// **Measured on the thirteen takes**, mass-weighted, on contact-free ticks:
    /// long takes 469-726 mean / 3157-9928 peak, short cuts 630-1596 / 3893-8210.
    /// 600 to 4500 puts a settled body under the floor and a real slamming near the
    /// top, yielding a violence of **0.08 to 0.46** across the corpus - the spread
    /// this term lives or dies by. A first pass borrowed the garment's ramps and
    /// read 1.00 on twelve takes of thirteen.
    ///
    /// `rds-verify` prints mean and peak per take whenever the term is on.
    float thrashWeight{1.0f};
    float thrashFloor{600.0f};
    float thrashFull{4500.0f};
    /// Zero on every existing take: the pose sidecar only started carrying
    /// rotation at v2, so this half waits on a re-capture exactly as the
    /// garment's does.
    float tumbleWeight{0.8f};
    float tumbleFloor{40.0f};
    float tumbleFull{400.0f};

    /// Per-limb clamp on acceleration before the sum, against a solver blow-up
    /// and against one limb striking stone dominating the whole body.
    float thrashCeiling{10000.0f};

    /// How fast the memory of a violent stretch fades - long enough to bridge the
    /// bounces of a staircase, short enough that a settled fall reads settled.
    float holdMs{450.0f};

    /// How much of the answer comes from *this limb* rather than the body. 0 is
    /// body-wide (how bad is this fall), 1 the contact's own limb (how hard is
    /// this arm being whipped about). They come apart - a body sliding to a stop
    /// with one leg still cartwheeling is quiet on the first, loud on the second.
    /// The limb half is **not** fabric-weighted: a bare arm breaks as well as a
    /// sleeved one.
    float limbShare{0.5f};

    /// How far the tier's threshold drops at full violence, as a fraction of the
    /// span between the tier's threshold and its cap.
    ///
    /// **Moves the gate and never the ramp**, as the air-time head gate does: a
    /// contact admitted only because the body was thrashing arrives at the quiet
    /// end of the level ramp. A fraction of the tier's own span, so one number
    /// suits all six tiers and can never invert the gate.
    float gateDropFrac{0.35f};

    /// Added to the tier's firing chance at full violence - the conservative half
    /// of "occurrence": how often already-eligible contacts fire, rather than
    /// which ones are eligible.
    ///
    /// **Dead at the shipped defaults**: all six tiers ship at
    /// `probAtGate = probAtCap = 1`, so this adds to a probability clamped at 1.
    /// It goes live the moment any tier's `probAtGate` is pulled down.
    float chanceBonus{0.25f};

    /// Added to the layer's level at full violence. A tier is discrete, so this
    /// only makes a break that was already happening sit further forward.
    float levelBonusDb{2.5f};

    /// Extra budget slots at full violence. **The lever that actually moves
    /// occurrence**: over the corpus the gate drop at maximum gives +8 proposals
    /// against a 9235 baseline, while crunch budgets +8 each gives +65. The tiers
    /// are budget-limited rather than threshold-limited by roughly eight to one.
    ///
    /// Measured in crunch/gore *events* (156 at the shipped defaults): gate drop
    /// alone 160, budget bonus 2 alone 164, everything at bonus 3 gives 184 (+18%).
    /// Superadditive - a lowered gate finds the candidates and a raised budget
    /// affords them.
    ///
    /// A relaxation, never a waiver. Rounded rather than truncated, or a term
    /// peaking near 0.5 would grant nothing.
    int budgetBonus{3};

    /// How far the spacing shrinks at full violence. **Also dead at the shipped
    /// defaults** - every tier ships with `spacingMs = 0`, and scaling zero is
    /// zero. Kept because "crunch in quicker succession" is a separate ask from
    /// "crunch more times".
    float spacingScale{0.6f};
};

/// One rung of the accumulated-damage ladder. Discrete like the tiers: a rung
/// that should be less certain sits further up the ladder, never quieter.
struct AccumDamageStageConfig {
    /// Accumulated damage on one limb at which this rung fires. **0 disables the
    /// rung**, which is how a three-step ladder is expressed without a switch of
    /// its own - and how the shape of the ladder is chosen rather than fixed.
    float atDamage{};
    /// Which layer this rung plays: the part's crunch, or the gore. A ladder is
    /// built by choosing these - `crunch, crunch, gore, gore` is the default,
    /// `crunch, gore, gore` and `crunch, crunch, gore` are two edits away.
    bool gore{};
    /// On top of the contact's own onset level. Rising up the ladder is what
    /// makes the last rung the loud one.
    float levelDb{};
};

/// Damage that arrives from being worked on rather than from one bad landing.
///
/// In `Proventus_Avenicci_devbench_7` a head is bashed against a wall twenty-four
/// times over sixteen seconds, peaking at 371 u/s against a head crunch gate of
/// 432, so not one of them can ever crunch.
///
/// So this counts hits instead of judging them: every admitted contact adds to a
/// pool on its own limb, the pool heals over seconds, and crossing each rung of a
/// ladder plays a break. Repetition is the whole signal, which is why it cannot
/// fold into the tiers - they are a function of one contact, this of a history. It
/// shares nothing with them except which *slot* a site's crunch plays.
struct AccumDamageConfig {
    bool enabled{false};

    /// Only the head accumulates, and only the head breaks.
    ///
    /// The two scopes are not the same feature. Left open, every limb of a long
    /// tumble is being worked on, so eighteen pools walk up their ladders and the
    /// knockdown ends in a bag of breaking sticks. Closed to the head it is "a
    /// skull bashed against something gives way", the measured case above.
    ///
    /// Binned through `DamageSiteFor`, so this is the head *and the neck*. Applied
    /// before the pool is touched, so limb pools do not move at all.
    bool headOnly{false};

    /// Only while the player has this body in their hands. **VR, and HIGGS.**
    ///
    /// The ladder was written for a player holding a ragdoll by one hand and
    /// driving its head into a wall - a thing the player *does*. Everything else a
    /// knockdown contains should sound like a fall, not a beating.
    ///
    /// The hold arrives as `held_start`/`held_stop` state rows from the game
    /// thread, like `ragdoll_start`, so it keeps its place in time on replay.
    ///
    /// **On flat Skyrim, and in VR without HIGGS, nothing is ever held**, so
    /// turning this on there switches the ladder off - the switch says *require*.
    /// Older recordings carry no hold rows either, hence off by default.
    bool requireHeld{false};

    /// How long the pool takes to drain. An injury healing rather than an
    /// envelope releasing: long enough to bridge the ~1.2 s between one bash and
    /// the next, short enough that two falls are two histories.
    float healMs{7000.0f};

    /// Contacts under this intensity add nothing. Without it the settling
    /// scrabble at the end of a knockdown fills the pool and a body at rest
    /// eventually breaks a bone for no visible reason.
    float ignoreBelowIntensity{0.10f};

    /// What each qualifying contact adds, as a multiple of how far its intensity
    /// clears the floor. Turn it up to make a limb wear out faster without
    /// moving the ladder.
    float perHitScale{1.0f};

    /// The pool's ceiling. A limb worked on indefinitely stops climbing rather
    /// than banking damage to spend later, which stops a long beating firing the
    /// whole ladder at once.
    float maxPool{4.0f};

    /// How hard the blow that actually breaks the limb has to be, as intensity.
    ///
    /// **Reaching a rung arms a limb; this fires it.** Without it the break lands
    /// on whichever contact tipped the arithmetic over - in a beating, very often
    /// the twenty-fourth gentle scuff.
    ///
    /// In intensity rather than closing speed, so it reads against the same scale
    /// the pool is filled from. Well above `fTapBelowIntensity` (0.15): a contact
    /// that would have been a single tap has no business breaking anything.
    float breakIntensity{0.35f};

    /// How far the pool must fall below a rung before it can fire again, as a
    /// fraction of its threshold. **Load-bearing hysteresis**: without it a pool
    /// near a threshold steps down and back up on alternate contacts, firing the
    /// same rung over and over - 51 breaks on one corpus take, 25 on another, all
    /// the same bone on a loop. 1 is no hysteresis; 0 never re-arms inside a
    /// knockdown.
    float rearmFrac{0.55f};

    /// The floor under how often one limb may produce a break. The rungs space
    /// themselves, so this guards the case they cannot: a limb sitting exactly
    /// on a threshold while contacts keep arriving.
    float minGapMs{220.0f};

    /// How many breaks one limb may produce in a knockdown, and how many the
    /// whole body may. **The body cap is not a formality**: with a per-limb cap
    /// alone the first pass produced 132 breaks on one long tumble and 90 on
    /// another. Three per limb because a limb has three interesting states -
    /// cracked, broken, ruined.
    int maxPerLimb{3};
    int maxPerActor{5};

    /// The ladder, bottom to top. Defaults to **crunch, crunch, gore, louder
    /// gore**; set a rung's `atDamage` to 0 to drop it, or flip its `bGore` to
    /// change the shape.
    AccumDamageStageConfig stage1{.atDamage = 0.60f, .gore = false, .levelDb = -7.0f};
    AccumDamageStageConfig stage2{.atDamage = 1.20f, .gore = false, .levelDb = -3.0f};
    AccumDamageStageConfig stage3{.atDamage = 1.90f, .gore = true, .levelDb = -4.0f};
    AccumDamageStageConfig stage4{.atDamage = 2.80f, .gore = true, .levelDb = 1.0f};
};

struct DamageConfig {
    /// The master switch for all six tiers. Every part and every tier has its own
    /// on top of it.
    bool enabled{true};

    /// How much the violence of the fall moves all six tiers. See the struct.
    DamageViolenceConfig violence;

    /// The head. All three parts' thresholds come off measured data: a tier pitched
    /// near the old obliterate frac could not fire on any head ever recorded.
    ///
    /// Of 409 head contacts, 0.45 is the top 8.3% (a hard landing, not a knock)
    /// and 0.65 the top 4.4%. 0.80 sat just above the hardest head then recorded;
    /// the corpus has since grown a 1099 u/s outlier.
    ///
    /// Crunch cap and gore threshold are the same number on purpose: past there a
    /// harder strike has nothing more to say through the crunch.
    DamagePartConfig head{
        .enabled = true,
        .bodyForceShare = 0.0f,
        .crunch = {.atFrac = 0.45f, .capFrac = 0.65f, .quietDb = -18.0f, .loudDb = -4.0f,
                   .delayMs = 25.0f, .budget = 2},
        .gore = {.atFrac = 0.65f, .capFrac = 0.80f, .quietDb = -20.0f, .loudDb = -6.0f,
                 .delayMs = 40.0f, .budget = 1},
    };

    /// The neck and the torso: the column that carries everything else.
    ///
    /// 2019 spine contacts topping out at **837 u/s** (0.87 of the loud anchor), so
    /// a tier pitched by eye lands above the part's own ceiling and never fires.
    /// 0.45 is the top 1.0% against the head's 8.3% - a body landing on its back is
    /// the commonest contact a knockdown produces and must not crack every time.
    /// 0.72 is the top 0.30%, 0.88 just above the hardest spine there is.
    ///
    /// A third of the blend comes off the body envelope: a spine contact is most of
    /// the body by definition.
    DamagePartConfig spine{
        .enabled = true,
        .bodyForceShare = 0.35f,
        .crunch = {.atFrac = 0.45f, .capFrac = 0.72f, .quietDb = -20.0f, .loudDb = -5.0f,
                   .delayMs = 22.0f, .budget = 1},
        .gore = {.atFrac = 0.72f, .capFrac = 0.88f, .quietDb = -20.0f, .loudDb = -6.0f,
                 .delayMs = 38.0f, .budget = 1},
    };

    /// Arms, legs, hands, feet - and anything on a skeleton we could not name.
    ///
    /// The highest thresholds: 6853 limb contacts against 409 heads, so a gate that
    /// suits a skull turns an ordinary tumble into a bag of breaking sticks. 0.68
    /// is the top 0.83% (57 crunches across the corpus), 0.95 the top 0.12%; they
    /// reach 1099 u/s, which is what makes a gore tier up here reachable at all.
    ///
    /// A limb is also the part whose own closing speed says least about whether it
    /// broke, hence the half share off the body envelope.
    DamagePartConfig limb{
        .enabled = true,
        .bodyForceShare = 0.50f,
        .crunch = {.atFrac = 0.68f, .capFrac = 0.95f, .quietDb = -22.0f, .loudDb = -8.0f,
                   .delayMs = 18.0f, .budget = 2},
        .gore = {.atFrac = 0.95f, .capFrac = 1.15f, .quietDb = -22.0f, .loudDb = -8.0f,
                 .delayMs = 34.0f, .budget = 1},
    };

    // -- Past the obliterate point --------------------------------------------
    //
    // `IntensityConfig::obliterateFrac` used to be a second gate ANDed under the
    // body's gore, which made the most extreme contacts the mod can see *harder*
    // to hear. It relaxes the limits instead now: budget raised and spacing
    // shortened for that contact only, without touching what the tier is worth in
    // dB. A relaxation and not a waiver - a raised budget is still a budget, so a
    // ridiculous impulse from another mod cannot machine-gun the layer.

    /// Extra slots granted to a tier's budget when the contact is past the
    /// obliterate point. 0 holds an obliterate to the ordinary budget.
    std::int32_t obliterateBudgetBonus{2};

    /// What a tier's `spacingMs` is multiplied by for such a contact. 1 leaves the
    /// spacing alone; 0 removes it entirely for an obliterate.
    float obliterateSpacingScale{0.25f};
};

/// Sustained grazing contact drives looping voices attached to the limbs doing the
/// grinding. Two kinds, running *together* rather than one being chosen - a body
/// that rolls from flat onto a shoulder would audibly snap between two files if
/// the mod picked:
///
///  - **The body loop**, the full-weight grind, levelled off how much of the body
///    is on the surface (the contact fraction below). Silent under
///    `fBodyFracStart`, so a corpse dragged by one ankle does not sound like one
///    lying flat.
///  - **The limb loops**, light and dry, one per chain on the bone doing the most
///    rubbing. Several run at once.
///
/// The only coupling is the optional duck: with the body grind up, the limb loops
/// can be pulled under it so a full slide is one sound rather than five.
struct ScrapeLoopConfig {
    bool enabled{true};

    // -- ownership -----------------------------------------------------------

    /// Whether a running slide gives its harder contacts back to the impact path
    /// instead of spending them as scrape.
    ///
    /// **A graze outside a slide is never claimed**, and that is no longer a
    /// switch: claiming one unconditionally deleted about half an ordinary tumble's
    /// worthwhile contacts from the mix with nothing in their place.
    ///
    /// What is left is: *inside* a slide, is a rub that is also a hit still the
    /// slide's to spend? Off, it is - which keeps a skid from thudding every frame.
    /// On, `claimBelowIntensity` draws the line.
    bool slidesDontClaim{false};

    /// Where that line sits: below this intensity a slide keeps the contact, at or
    /// above it the impact path gets it. **Only consulted while `slidesDontClaim`
    /// is on.**
    ///
    /// The boolean asks "may a grind give anything back"; this asks "is *this*
    /// contact a hit as well as a rub". A body grinding down a staircase produces
    /// spine and thigh contacts at 107-164 u/s whose *intensity* is 0.28-0.49, and
    /// every one was spent as a scrape grain because the ratio said rub.
    /// `grazeMaxImpactSpeed` cannot reach them: it caps closing speed.
    ///
    /// A contact that falls through is voiced by the impact path and gets no grain:
    /// one collision is one onset.
    ///
    /// The default is the tap threshold rather than 1.0, which would hand back
    /// nothing and leave the switch reading as free while never firing (the
    /// dead-gate trap, 01 §4).
    float claimBelowIntensity{0.15f};

    /// How long the grind takes to arrive - long enough to be the arrival and not
    /// just a de-click.
    ///
    /// The loops are the *voicing* of `Motion::kSlide` and nothing else; when a
    /// slide starts and stops is decided once, on the motion axis, under the slide
    /// keys in this section. A slide is not declared until 150 ms or 45 units into
    /// a grind that is already happening, so the loop always opens into a body that
    /// has been scraping for a moment.
    ///
    /// **This does nothing in the game today.** `GameRenderer::StartLoopVoice`
    /// bakes a loop's gain into its PCM buffer and opens it with `Open`; nothing
    /// calls `FadeInPlay`, so `fadeMs` on a `kStartLoop` is read only by the
    /// offline mixer.
    float startFadeMs{140.0f};

    /// What is left to hide once the speed ramp does the fading. With
    /// `fSpeedRangeDb` set so the ramp bottoms out at the voice floor, a grind
    /// that ends in friction has already faded to nothing; this is for the abrupt
    /// endings - actor released mid-skid, body out of earshot, mod switched off.
    float stopFadeMs{110.0f};

    /// The fade used when the slide ended because the body left the ground. That
    /// ends the instant the surface does, and the ordinary fade drags a grinding
    /// rumble out behind a body already in the air.
    float launchFadeMs{45.0f};

    // -- the body loop -------------------------------------------------------

    bool bodyEnabled{true};

    /// Raised with the ramp below rather than independently: deepening the speed
    /// dependence pulls the middle of the range down, so the top has to come up.
    /// At the shipped pair a full-speed skid is 4 dB louder than it used to be and
    /// a 300 u/s one about 5 dB quieter.
    float gainDb{-16.0f};

    /// Loop level and pitch track the *body's measured speed* continuously between
    /// these - a slide is exactly as loud as the body is fast, which is the one
    /// thing about it a listener can check against what they see.
    ///
    /// **The bottom is also where the grind stops**, so it has to be a speed the
    /// body genuinely reaches. At the old 120 u/s the grind was switched off
    /// partway down its own ramp, 16 dB clear of the voice floor. 40 u/s is a body
    /// that has all but stopped.
    float speedForMinGain{40.0f};
    float speedForMaxGain{600.0f};

    /// How far under `fGainDb` the loop sits at `fSpeedForMinGain` - the depth of
    /// the speed dependence. At 0 dB the slide is one level whatever it is doing.
    ///
    /// **Set so `fGainDb + fSpeedRangeDb` lands on `Mix:fVoiceFloorDb`**: -16 and
    /// -32 make -48, the floor. A body slowing to a halt then fades out on its own
    /// measured speed and reaches silence as it reaches the bottom of the ramp, so
    /// the stop has nothing to cut. Move either number and the grind is cut
    /// mid-ramp. (It was -12, a 16 dB step `fStopFadeMs` had to cover - which is
    /// why a slide sounded switched off rather than run down.)
    float speedRangeDb{-32.0f};

    float pitchPerThousandUnits{0.15f};

    // -- the contact fraction ------------------------------------------------
    //
    // Why the grind used to fire on almost nothing and then play at full body
    // volume. Every body site carries an anatomical mass (07 §6, `NominalMass`), so
    // summing the ones demonstrably rubbing gives how much body is on the floor:
    // one dragging foot is 1.5%, both feet and shins ~13%, a body flat on its back
    // and skidding 60%+.
    //
    // It shapes **the loop only** and may never silence an impact. Where the sites
    // cannot be resolved the fallback is the quieter answer - limb-only.

    /// Off, the body loop is a function of speed alone - what the mod did before
    /// this existed, and the fastest A/B for whether the fraction fixed it.
    bool fractionEnabled{true};

    /// Where the body grind begins to come in and where it is at full weight, as
    /// a fraction of total body mass in contact. Below the first it does not play
    /// at all; the limb loops are what carry the bottom of the range.
    float bodyFracStart{0.20f};
    float bodyFracFull{0.55f};

    /// How far under `fGainDb` the body loop sits at `fBodyFracStart`, the same
    /// shape as `fSpeedRangeDb`. Deep: crossing a threshold must not be an event.
    float bodyFracRangeDb{-15.0f};

    /// How long a limb goes on counting as "in contact" after its last graze. A
    /// fraction computed from one tick's contacts flickers between 45% and nothing
    /// at solver frequency; this turns a stream of collisions into a state, and
    /// holds the limb loops open too.
    float contactHoldMs{140.0f};

    /// Whether the body loop hangs on the bone nearest the contact rather than the
    /// actor's root (roughly the pelvis, which is not where the sound is even for
    /// a genuine full-body slide). Nearest-to-contact rather than lowest, because
    /// a body can grind along a wall, down a staircase, or across a ceiling.
    bool bodyFollowsContact{true};

    // -- the limb loops ------------------------------------------------------

    bool limbEnabled{true};

    /// How many limb loops may run at once, of the five chains that can have one
    /// (two arms, two legs, the head), ranked by how hard each chain is rubbing.
    /// Zero switches the limb half off.
    ///
    /// A mix decision, not a budget one: five limbs grinding at once is more sound
    /// than the picture supports, not more than Skyrim will take.
    std::int32_t maxLimbLoops{3};

    float limbGainDb{-30.0f};

    /// The limb loop's own speed response, measured on how fast *that chain* is
    /// rubbing rather than on the body - a dragging foot behind a body that has
    /// otherwise stopped is still a dragging foot.
    float limbSpeedForMinGain{80.0f};
    float limbSpeedForMaxGain{500.0f};
    float limbSpeedRangeDb{-12.0f};
    float limbPitchPerThousandUnits{0.22f};

    /// How hard a chain has to be rubbing before it gets a loop of its own.
    float limbMinTangentSpeed{100.0f};

    /// How hard a chain that already has a loop must *stay* rubbing to keep it.
    /// Below `fLimbMinTangentSpeed`, so the pair is hysteresis - which stops two
    /// crossing legs swapping a voice back and forth tick to tick.
    float limbHoldTangentSpeed{60.0f};

    /// Whether a limb loop hangs on the limb doing the rubbing. Off, it collapses
    /// onto the body like every loop used to. Matters most for the player, whose
    /// ragdoll is at arm's length and for whom a collapse to one point sounds like
    /// the audio is inside your head.
    bool limbFollowsLimb{true};

    /// How long a different bone in the same chain must be the one rubbing before
    /// the loop moves onto it. A scrape hopping between bones frame by frame
    /// smears instead of tracking; this is the cure, applied to the hop.
    float limbHoldMs{200.0f};

    // -- the head ------------------------------------------------------------

    /// Whether the head chain's loop is tinted rather than the plain limb grind.
    /// A skull dragging on stone is a small contact patch, so it is the limb file
    /// - but it is a *skull*. Same trick as the head impact's faint ring: the same
    /// file, pitched down a little, with its own trim. A proper head variant is a
    /// file drop, not a code change.
    bool headTint{true};
    float headPitchScale{0.90f};
    float headGainDb{-2.0f};

    // -- the body loop over the limb loops -----------------------------------

    /// Whether the body grind pulls the limb loops down under it. Scaled by the
    /// body loop's own weight, so it arrives with the grind rather than switching
    /// on: nothing at `fBodyFracStart`, all of `fLimbDuckDb` at `fBodyFracFull`.
    bool bodyDucksLimbs{true};

    /// How far down, at full body weight. Deep enough is suppression rather than
    /// damping: a limb loop ducked under the voice floor is stopped, not played at
    /// silence.
    float limbDuckDb{-9.0f};

    // -- the entry catch -----------------------------------------------------
    //
    // One `scrape_grain` at the moment a grind *starts*: the scuff of the limb
    // arriving on the surface, under the head of the loop it introduces. (The slot
    // brief still describes the old catch layer, which fired through a whole slide
    // on any graze harder than its recent average. The references do put sixty-five
    // grit peaks a second on the rumble, but that is *texture* and belongs in the
    // file at 65 Hz, not in the cue list at 12 Hz.)
    //
    // The moment that genuinely is an event is the entry, because a slide is not
    // declared until 150 ms or 45 units into a grind that is already happening.
    // One grain on the front is the fix, the same shape as `imp_transient`.
    //
    // The stream-only gates are gone rather than defaulted off - `fGrainCatchRatio`,
    // `fGrainMinGapMs` and `fGrainProbability` have no meaning for a once-per-grind
    // event. Their keys are dropped rather than `Renamed`.
    //
    // Still never an inference: a loop only enters on a chain that has really been
    // grazing inside `fContactHoldMs`.

    bool grainEnabled{true};

    /// How loud the entry scuff is against the grind it introduces - the *loop's*
    /// level, not a contact's onset, since an entry is the front of the sound it
    /// opens. It scales with the grind, so a limb barely dragging scuffs quietly
    /// and a body arriving at speed scuffs hard.
    float grainGainDb{-6.0f};

    /// How much the pitch of an entry scuff is scattered, either way. Still earns
    /// its place at one grain per grind: a body that grinds, launches and lands
    /// three times enters three times, and three entries at one pitch read as one
    /// sample repeating.
    float grainPitchScatter{0.18f};

    /// Whether the **body** grind gets an entry scuff as well as the limb grinds.
    ///
    /// Off by default: a limb arriving on a surface is a scuff, a torso arriving
    /// flat is a *fall*, and the impact composite is already voicing it with a
    /// full stack in the same 100 ms.
    ///
    /// On is for the entry the composite does not cover: a body already down and
    /// still, then dragged. No collision there for the impact path to have voiced.
    bool grainOnBody{false};

    // -- the level's own movement --------------------------------------------

    /// How much of the *contact* speed is blended into the level's own speed. Body
    /// speed is smooth, so a level following it alone reads as a constant; contact
    /// tangent speed is spiky as limbs load and unload, and a little of it makes
    /// the grind breathe. A wobble *around* the body speed and never a replacement
    /// - one limb's tangent is the speed of a limb. 0 is pure body speed.
    float contactSpeedBlend{0.25f};

    /// How much the level must move before a running loop is told about it. A loop
    /// re-cueing every frame buries the cue list; the shared 0.75 dB deadband was
    /// most of what a breathing grind does, so the scrape got its own.
    float levelDeadbandDb{0.25f};

    // -- surfaces ------------------------------------------------------------

    /// Whether the loops pick a surface-coloured file. Off, everything plays the
    /// default grind. On, a slot with no recording behind it falls back to the
    /// default anyway, so this costs nothing until the files exist.
    bool surfaceVariants{true};

    // -- the rumble bed ------------------------------------------------------
    //
    // The mass under the grind, as its own voice: `scrape_loop_rumble` held open
    // for the life of a slide, at the body grind's anchor, with a pitch that
    // deliberately does not move.
    //
    // **Why a layer and not a better grind file.** Against GTA 4's slide events our
    // grinds are 35-45 dB out on the bass-to-hiss balance and in the opposite
    // direction. Grain rates match, so density was never the problem, and no EQ
    // rescues it: there is nothing under the shelf to boost.
    //
    // **Why its own voice and not baked in.** `MixLoop` applies pitch as a resample
    // of the whole source, so a composite file forces the bed to pitch with the
    // speed - and pitching bass down at a crawl is flubby. It would also be six
    // files to re-render on every change, and two incoherent bass beds at partial
    // gain during the body/limb crossfade.
    //
    // The bed also runs longer than the grinds, so the pair does not repeat on a
    // common period.

    /// Off, the slide is the grinds alone - what the mod was before this existed,
    /// and the fastest A/B for whether the bed fixed it.
    bool rumbleEnabled{true};

    /// The bed's level at full weight and full speed.
    ///
    /// Over the body grind's own `fGainDb` rather than under it: in the references
    /// the sub band is the *loudest* band of a slide and the grit rides on top -
    /// the same inversion `imp_sub` has against `imp_transient`.
    ///
    /// **-13 against the grind's -16 is measured**, both layers at the top of their
    /// ramps. Summed with the assigned `scrape_loop` and peak-normalised, the pair
    /// comes to tilt +14.1 and a centroid of 5444 Hz - inside GTA 4's +10 to +21
    /// and 4355-5714 Hz, from a grind that measures -36.3 and 9102 Hz alone.
    ///
    /// That settles the standing argument about swapping in the library's bass-led
    /// grinds (+17.6 tilt): one of those plus this bed comes to **+27** tilt and a
    /// 2431 Hz centroid, past the window on both axes, because the mass is in the
    /// mix twice. **The two decisions are one decision.**
    float rumbleGainDb{-13.0f};

    // The bed has no speed ramp of its own. It rides the limb grinds'
    // (`fLimbSpeedForMinGain`, `fLimbSpeedForMaxGain`, `fLimbSpeedRangeDb`),
    // measured on `slideTangent`. `fRumbleSpeedRangeDb` and `fRumbleSpeedCurve` are
    // gone: 30 dB of depth with a squared track under it, fed by the body's smooth
    // speed, gave a layer that was inaudible or fully on with nothing between.
    //
    // The cost: tuning the limb grinds' depth now moves the bed's with it.

    /// A fixed pitch for the bed - **the one ramp in this section that is
    /// deliberately not a ramp.** Floor and body resonance do not move with how
    /// fast the body is going; GTA 4's slide events hold a static spectrum and
    /// swell in level. Leave at 1 unless a recording needs transposing.
    float rumblePitch{1.0f};

    /// ...and the ramp it is not, available so the claim above can be tested by
    /// ear. 0 is the design's answer; wind it up and listen for flubbiness at the
    /// bottom of the speed range, which arrives before the movement reads as speed.
    float rumblePitchPerThousandUnits{0.0f};

    /// Whether a limb-only slide gets a bed, and how much less of one. A dragging
    /// foot still loads the floor, so the bed runs for both and the difference is a
    /// trim - interpolated on the same contact fraction the body grind's weight
    /// uses: all of `fRumbleLimbGainDb` at `fBodyFracStart`, none at `fBodyFracFull`.
    ///
    /// **With no body grind running at all the trim is applied whole**, whatever
    /// the fraction says: with `bBodyEnabled` off, a body lying flat and skidding
    /// measured a fraction near 1, cancelling the trim and playing the bed at full
    /// body level under nothing but limb grinds - mass with none of the grit.
    bool rumbleOnLimbs{true};
    float rumbleLimbGainDb{-9.0f};
};

/// The airborne anticipation rise, and nothing else any more. It used to own a
/// continuous cloth bed too, muted in all thirty-eight saved configs
/// (`bFoleyCloth = 0` in every one), so that went with its slot.
struct MotionFoleyConfig {
    bool enabled{true};

    /// The airborne anticipation rise. On by default at a low level.
    bool airborneRise{true};
    float airborneRiseGainDb{-24.0f};
};

/// The garment: a continuous fabric and armour layer riding a knockdown.
///
/// **Off by default, and off means byte-identical** - nothing is measured, no
/// proposal made and no random number drawn while `bEnabled` is 0. Every slider
/// under it ships at a usable voicing rather than a neutral one.
///
/// Three things were structurally wrong with the old `MotionFoley` cloth bed: its
/// level was a function of *body speed*, which is highest when airborne and
/// lowest through the tumble this exists for; it had no armour class; and nobody
/// had framed what it was filling. The mod discards about nine contacts in ten
/// and the body thrashes on through the gaps, which body speed cannot see.
///
/// If this one ends up at 0 in every saved config too, delete it.
struct RustleConfig {
    bool enabled{false};

    // ── what counts as movement ──────────────────────────────────────────────
    //
    // Two independent signals, each with its own ramp in its own units, and a
    // weight to blend them. Two ramps because the quantities are not
    // commensurable - u/s^2 against u/s - and adding them before normalising would
    // hide a unit conversion inside a weight.

    /// **Thrash** - how hard the limbs are accelerating *relative to the body*,
    /// fabric-weighted across the skeleton, u/s^2.
    ///
    /// Not the centre of mass: the COM of a body bouncing down a staircase traces
    /// a fairly smooth arc between treads, so a COM-driven layer would be nearly
    /// flat through the most chaotic part of a fall.
    float thrashWeight{1.0f};
    /// The ramp, in u/s^2. Below the floor there is no rustle at all, which keeps a
    /// body lying still silent - the most important number here.
    ///
    /// **Measured on the thirteen takes**, fabric-weighted mean relative limb
    /// acceleration per tick: long takes 583-793 mean / 6300-18300 peak, short cuts
    /// 945-2565 / 6000-18300. So an active tumble sits around 1000-2500 and an
    /// impact frame reaches 6000-18000. (A first pass guessed 140/1100, at which
    /// every tick of every take saturated.)
    ///
    /// 500 to 4000 puts settled stretches under the floor, an ordinary tumble a
    /// third of the way up and every real impact at the top. `rds-verify` prints
    /// `rustleThrashMean` and `rustleThrashPeak` per take when the feature is on.
    float thrashFloor{500.0f};
    float thrashFull{4000.0f};

    /// **Tumble** - how fast the limbs are rotating, as surface speed
    /// (angularSpeed x radius), fabric-weighted, u/s. Cannot be folded into the
    /// thrash: a limb rotating at a *constant* rate drags its sleeve with no
    /// acceleration at all, so a steady cartwheel has a large tumble and a small
    /// thrash and a body slamming between poses has the reverse.
    ///
    /// **These three are still guesses.** The `_pose.bin` sidecar never stored
    /// `angularSpeed`, so the term measured exactly 0 on all thirteen takes while
    /// being live in the game. The sidecar is v2 now; **the corpus has to be
    /// re-captured before this can be tuned offline.**
    float tumbleWeight{0.8f};
    float tumbleFloor{40.0f};
    float tumbleFull{400.0f};

    /// Per-limb clamp on the thrash term, u/s^2, applied *before* the sum.
    ///
    /// The position matters. Pose has no blow-up rejection, so a limb the solver
    /// teleports produces an arbitrary acceleration and the layer screams. Per
    /// limb rather than after the sum also fixes the ordinary case: one limb
    /// striking stone is held sane while the other seventeen contribute their real
    /// motion, so an impact *lifts* the drive instead of saturating it.
    float thrashCeiling{10000.0f};

    /// Whether the body's own acceleration is subtracted from each limb's. On is
    /// right - fabric moves with motion *relative* to the body it is on, and
    /// everything on a falling body accelerates at g together. Off is the naive
    /// version, kept as a one-key A/B.
    bool relativeToBody{true};

    /// How much a fast body multiplies the two terms above. A multiplier, never an
    /// addend: the same thrash at 600 u/s displaces more cloth, but a body
    /// drifting fast and limply must not rustle at all. 0 is the A/B baseline.
    float speedWeight{0.30f};
    float speedForFull{600.0f};

    /// The airborne term, and the only addend. **Off by default:** a cloak does
    /// flap in a long drop and the relative measurement reads nothing there, but
    /// `air_whoosh` already fires on this state. Present so the comparison can be
    /// made; if the fabric version wins, retire the whoosh rather than layer it.
    float airWeight{0.0f};
    float airSpeedForFull{500.0f};

    /// Below this drive the loop is stopped rather than held at silence. No
    /// separate hold time: an exponential decay never reaches zero, so what ends a
    /// rustle is this threshold and how long `fReleaseMs` takes to fall through it.
    float silenceDrive{0.03f};

    // ── how it moves ─────────────────────────────────────────────────────────

    /// Asymmetric on purpose, and the asymmetry is the whole voicing. Fast attack
    /// because cloth responds immediately and a slow one puts the rustle behind
    /// the impact. Slow release because that is what a garment does - and it fills
    /// the gaps between bounces so a tumble reads as continuous, and puts a
    /// decaying fabric tail behind every impact without inferring anything.
    float attackMs{30.0f};
    float releaseMs{280.0f};

    /// The loop's pitch across the drive range. Narrow: a wide range here is the
    /// fastest way to make a sustained layer sound synthetic.
    float pitchAtFloor{0.94f};
    float pitchAtFull{1.07f};

    /// A slow wobble on the level, so a two-second file does not read as one. A
    /// deterministic sine, never a random walk - anything drawing a number per
    /// tick re-rolls every variant and scatter downstream. Phased off the actor's
    /// start time so two bodies falling together do not breathe in unison.
    float wanderDepthDb{1.5f};
    float wanderHz{0.23f};

    /// How far the level moves before a running loop is re-cued. The scrape's
    /// value and reason: without it a loop emits an update every frame.
    float levelDeadbandDb{0.25f};

    /// Longer than the scrape's at both ends. Fabric has no transient, and a
    /// rustle that snaps in is the most obvious thing in the mix.
    float startFadeMs{90.0f};
    float stopFadeMs{240.0f};

    // ── how loud ─────────────────────────────────────────────────────────────

    /// The layer's level at full drive. A bed: the old cloth layer measured
    /// 30-36 dB under a hero hit, and this belongs at the quiet end of that.
    float gainDb{-26.0f};

    /// How far under `fGainDb` the layer sits at the bottom of the drive ramp.
    /// Deep, so crossing the floor is not an event in itself - the same shape as
    /// `ScrapeLoop:fSpeedRangeDb`.
    float driveRangeDb{-20.0f};

    /// Per-class voicing, on top of whichever conditional variant the class
    /// selected. Unlike the armour skins these do not ship at 0: one slot carries
    /// all four classes, so the bare trim is what makes a naked body quiet.
    float bareTrimDb{-9.0f};
    float clothTrimDb{0.0f};
    float lightTrimDb{1.0f};
    float heavyTrimDb{2.0f};

    /// Your own ragdoll is at arm's length and its layers follow bones, so the
    /// garment is closer to the ears than any NPC's will ever be.
    float playerTrimDb{-3.0f};

    /// How far a running body grind pulls the rustle down at full body weight,
    /// scaled by the grind's own weight as `bBodyDucksLimbs` scales `fLimbDuckDb`.
    /// A slide already has a body loop, up to three limb loops and a grain layer
    /// describing the same motion; a fabric bed under all of that is mud. Deep
    /// enough to be suppression rather than damping.
    float slideDuckDb{-14.0f};
};

struct StrategiesConfig {
    ImpactCompositeConfig impact;
    /// Not a strategy of its own - a measurement three of them read. It lives
    /// here because it is tuned with them and because the head half was part of
    /// `head` until the body half wanted the same timestamp.
    AirTimeConfig airTime;
    HeadImpactConfig head;
    DamageConfig damage;
    ScrapeLoopConfig scrape;
    MotionFoleyConfig foley;
    RustleConfig rustle;
    /// Beside `damage` rather than inside it: it answers a different question
    /// off a different measurement and shares no tuning with it.
    AccumDamageConfig accum;
};

// ── Surfaces ─────────────────────────────────────────────────────────────────

// Everything that decides how the floor colours a sound, in one place.
//
// A surface skin is an extra layer stacked on an impact, not a variant of one.
// Colour is additive, so six surfaces cost six files where a surface *axis*
// would multiply every layer in the composite by six. It is also the honest
// model: landing on boards does not replace the sound of a body arriving, it
// adds a hollow knock. A missing skin is a body landing with no floor named -
// quieter, not wrong.
//
// Every row here carries the ini key it had before the move from four scattered
// sections, so an older ini still loads.

/// One surface's own settings.
///
/// Thirteen of these live in `SurfaceConfig`, one per `SurfaceClass`, and only the
/// *opened* ones are written to the ini. A closed block is never read as zeroes -
/// `Resolve` fills it from the nearest opened ancestor or the section defaults -
/// so opening one is free.
///
/// A field is here if it is a property of the *material*; it stayed in
/// `SurfaceConfig` if it is a property of the *system*.
struct SurfaceSkinConfig {
    /// Muted at render like every other mute, so muting one leaves every
    /// arbitration decision identical and silences only what came out.
    bool enabled{true};

    /// This skin alone, summed on top of `SurfaceConfig::trimDb`. Thirteen
    /// separately recorded sounds arrive at thirteen different levels.
    float trimDb{0.0f};

    /// When the colour arrives, relative to the contact frame. Close to the
    /// transient or it stops reading as the same event - but not identical across
    /// materials: a hollow knock has a resonant delay glass does not, and a splash
    /// lags both.
    float offsetMs{8.0f};

    /// The ramp, interpolated by intensity: a brush of the floor barely names it,
    /// a body dropped on it names it clearly. The widest genuine difference
    /// between materials - glass is nearly silent at a brush and a shatter at
    /// speed, where carpet is flat across the range.
    float gainAtMinDb{-12.0f};
    float gainAtMaxDb{-6.0f};

    /// Colour the burst filler with this material too. Per-surface because water
    /// is the counter-example: nine of every ten contacts are taps, and a splash
    /// on every one is absurd where wood and gravel want tap colour badly.
    bool onTaps{true};

    /// The tap's ramp for this material. The tap's *offset* and its headroom
    /// clamp stay global - they describe the 40-100 ms grain being coloured
    /// rather than the floor doing the colouring.
    float tapGainAtMinDb{-12.0f};
    float tapGainAtMaxDb{-8.0f};
};

struct SurfaceConfig {
    /// Off, nothing gets a surface skin and the mod plays the same body on marble
    /// as on moss. The per-class mutes do the same one surface at a time.
    bool enabled{true};

    // -- the chain root -------------------------------------------------------
    //
    // What a class inherits when neither it nor any ancestor has a block of its
    // own, which on a fresh install is all thirteen. They keep their original ini
    // keys, so a file written before the surfaces list existed loads unchanged.

    /// When the colour arrives, relative to the contact frame. Close to the
    /// transient, or it stops reading as the same event.
    float offsetMs{8.0f};

    /// The ramp, interpolated by intensity like every other layer: a brush of
    /// the floor barely names it, a body dropped on it names it clearly. If the
    /// surface reads as a separate sound rather than as colour, it is too loud.
    float gainAtMinDb{-12.0f};
    float gainAtMaxDb{-6.0f};

    // -- on the tap -----------------------------------------------------------
    //
    // A tap was a single grain, and so the one cue that could not say what it hit.
    // Nine of every ten contacts are taps, so a body sliding down a wooden
    // staircase spent nine tenths of itself sounding like a fall down nothing in
    // particular. Scuffs are where a floor gets identified; hero hits confirm it.

    /// Colour the burst filler too, not only the four-layer composite.
    bool onTaps{true};

    /// The tap's own offset, tighter than the composite's: a tap is 40-100 ms of
    /// grain, so a skin arriving 8 ms in and running 200 ms would outlive what it
    /// is colouring and read as a second cue.
    float tapOffsetMs{4.0f};

    /// The ramp, measured rather than guessed. At -20/-14 the colour landed under
    /// `Mix:fVoiceFloorDb` on nearly every tap that survived to render - a tap is
    /// already the quietest cue in the mod, so there is no room underneath for
    /// something 15 dB down. A tap's colour rides close or not at all, and
    /// `tapHeadroomDb` is what stops "close" turning into "level with".
    float tapGainAtMinDb{-12.0f};
    float tapGainAtMaxDb{-8.0f};

    /// How far under its own tap the colour is held, whatever the ramp says -
    /// "colour, not dominate" as a number. Negative, always: 0 lets the colour tie
    /// with the grain and positive lets it win. The tap's rank comes from the tap
    /// alone regardless, so this is about what is heard, not what is chosen.
    float tapHeadroomDb{-3.0f};

    // -- level, after the cue has been chosen ---------------------------------

    /// The role trim - every skin together. Up makes the floor material obvious,
    /// down makes every surface sound the same (which is what vanilla does).
    /// Was `Mix:fSurfaceTrimDb`.
    float trimDb{0.0f};

    // -- the list -------------------------------------------------------------

    /// Which classes have a block of their own in the surfaces ini. The whole of
    /// the `+` in the panel: opening sets the bit and nothing else happens, since
    /// `Resolve` already left the block holding what it was inheriting. Closing
    /// clears it, re-resolves, and the ini section disappears on the next save.
    bool opened[static_cast<std::size_t>(SurfaceClass::kCount)]{};

    /// Every class's *effective* settings, inherited or owned. `Resolve` runs
    /// after every load and edit, so the engine indexes this directly and never
    /// walks a chain on the audio path - and reading an entry without checking
    /// `opened` is correct.
    SurfaceSkinConfig skins[static_cast<std::size_t>(SurfaceClass::kCount)]{};

    static constexpr std::size_t kClasses = static_cast<std::size_t>(SurfaceClass::kCount);

    /// What this class actually plays with. Total - every class has an answer.
    [[nodiscard]] const SurfaceSkinConfig& Skin(SurfaceClass c) const {
        const auto i = static_cast<std::size_t>(c);
        return i < kClasses ? skins[i] : skins[0];
    }

    [[nodiscard]] bool Opened(SurfaceClass c) const {
        const auto i = static_cast<std::size_t>(c);
        return i < kClasses && opened[i];
    }

    /// The settings a closed class inherits: the section defaults above, with
    /// `enabled` and `trimDb` neutral - those two have no global to take, since
    /// `SurfaceConfig::enabled` is the master switch and `trimDb` sums on top.
    [[nodiscard]] SurfaceSkinConfig RootSkin() const {
        SurfaceSkinConfig s{};
        s.offsetMs = offsetMs;
        s.gainAtMinDb = gainAtMinDb;
        s.gainAtMaxDb = gainAtMaxDb;
        s.onTaps = onTaps;
        s.tapGainAtMinDb = tapGainAtMinDb;
        s.tapGainAtMaxDb = tapGainAtMaxDb;
        return s;
    }

    /// Push the section defaults down the parent chain into every class with no
    /// block of its own. Run after every load and edit; cheap (thirteen classes,
    /// chains at most three deep) and idempotent.
    void Resolve() {
        const SurfaceSkinConfig root = RootSkin();
        for (std::size_t i = 0; i < kClasses; ++i) {
            if (opened[i]) {
                continue;
            }
            // The nearest ancestor that owns its settings, or the root. Per class
            // rather than in dependency order, so declaration order does not matter.
            const SurfaceSkinConfig* from = &root;
            for (SurfaceClass p = SurfaceParent(static_cast<SurfaceClass>(i));
                 p != SurfaceClass::kCount; p = SurfaceParent(p)) {
                if (opened[static_cast<std::size_t>(p)]) {
                    from = &skins[static_cast<std::size_t>(p)];
                    break;
                }
            }
            // `enabled` and `trimDb` come along: a muted or trimmed parent takes
            // its unopened children with it, which is what makes muting `stone`
            // worth doing now that ice and glass hang off it.
            skins[i] = *from;
        }
    }
};

// -- Armour: the second colour axis -------------------------------------------

/// What the body was wearing, as a layer rather than a timbre shift.
///
/// Same shape as `SurfaceConfig`: colour is additive, so four armour classes cost
/// four files where an armour *axis* would cost 23 slots x 4 classes x 2 variants.
/// What changes when you put plate on is not the body's mass, it is that something
/// metallic moved.
///
/// Nothing here does anything until `armor_*` files exist - all four slots ship
/// with `expectedVariants = 0`.
struct ArmorConfig {
    /// Off, no cue gets an armour skin and the class is not even read. The four
    /// mutes at the bottom do the same one class at a time.
    bool enabled{true};

    // -- how the class is decided ---------------------------------------------
    //
    // The mapping from a body to a class is a judgement call; these are the three
    // places it can be overruled.

    /// Per contact limb rather than per actor. On - the default - heavy boots
    /// and nothing else means feet get the plate rattle and the rest get the
    /// bare skin, with no new mechanism: `contact.coverage` is already the class
    /// of the limb that hit. Off, every contact reads the actor's body-slot
    /// class instead, so a heavy cuirass makes the whole actor clank.
    bool perLimb{true};

    /// A site with nothing in its own biped slot takes the cuirass's class rather
    /// than reporting bare. Right for the common case, wrong for heavy boots on an
    /// otherwise naked body. `CoverageForSite`'s hardcoded behaviour, switchable.
    bool inheritFromBody{true};

    /// A worn ARMO with no name and no weight is bare, not clothing. TNG's skin is
    /// a real TESObjectARMO on five slots, so without this a naked modded body
    /// reads as clothed and `armor_bare` essentially never fires. Mirrors
    /// `CoverageFrom` in the recording loader.
    bool bareIsNaked{true};

    /// Where the airborne rise gets a class from, having no contact and
    /// therefore no limb. 0 the actor's body slot - a cuirass is what you hear
    /// moving - 1 the last contact's limb, 2 off.
    int actorClassSource{0};

    // -- the head accent ------------------------------------------------------
    //
    // The only armour rule anywhere near the head, read once before any
    // classification. It takes away the *accent* - the dull skull thud with the
    // ring on it, which reads as bare skull and is wrong under a helm. The
    // composite, the skins and damage all still fire, so a head in plate landing
    // hard is still loud; it just stops sounding like a melon.

    bool noHeadOnLight{false};
    bool noHeadOnHeavy{false};

    // -- on the composite -----------------------------------------------------

    /// When the rattle arrives - between the transient and the body, since metal
    /// moving because something stopped is a consequence of the contact. At 0 it
    /// fuses with the transient into one brighter click.
    float offsetMs{12.0f};

    /// The ramp, interpolated by intensity like every other layer.
    float gainAtMinDb{-14.0f};
    float gainAtMaxDb{-7.0f};

    /// How far under the stack the skin is held, whatever the ramp says. Negative,
    /// always. The skin never enters `levelDb` (it passes `ranks = false`), so
    /// this is about what is heard. It matters more here than for surfaces: armour
    /// keeps moving after the body has stopped, so a plate rattle is the layer
    /// most likely to be long and loud relative to what it is colouring.
    float headroomDb{-3.0f};

    // -- on the tap -----------------------------------------------------------

    /// Colour the burst filler too. Nine of every ten contacts are taps, so this
    /// is most of where armour would be heard at all.
    bool onTaps{true};
    float tapOffsetMs{5.0f};
    float tapGainAtMinDb{-13.0f};
    float tapGainAtMaxDb{-9.0f};
    float tapHeadroomDb{-3.0f};

    // -- on the slide ---------------------------------------------------------

    /// Armour rides the scrape loop, at the class the contact that opened the
    /// slide was wearing (`CrashState::slideCoverage`). Plate dragged over
    /// flagstone pays for itself with a single file.
    bool onSlide{true};
    float slideGainDb{-10.0f};

    // -- the free levers ------------------------------------------------------
    //
    // Neither costs a file. Pitch is continuous and free here, and the design
    // already leans on it (00 §7, "it beats doubling the bank").
    //
    // All eight ship at 0 so an install with no armour assets is exactly today's
    // mod. The voicing worth trying first is heavy -1.0 and bare +0.5, which makes
    // an armoured body read heavier and a naked one lighter with no assets at all.

    float barePitchSemis{0.0f};
    float clothPitchSemis{0.0f};
    float lightPitchSemis{0.0f};
    float heavyPitchSemis{0.0f};

    /// Per-class trim over the whole composite, not just the skin. Heavy armour
    /// genuinely is louder; this says so without touching what was chosen.
    float bareCompositeTrimDb{0.0f};
    float clothCompositeTrimDb{0.0f};
    float lightCompositeTrimDb{0.0f};
    float heavyCompositeTrimDb{0.0f};

    // -- level, after the cue has been chosen ---------------------------------

    /// The role trim - all four skins together, the twin of `Surfaces:fTrimDb`.
    float trimDb{0.0f};

    /// One trim per file, summed on top of the role trim, because four
    /// separately recorded sounds arrive at four different levels.
    float bareTrimDb{0.0f};
    float clothTrimDb{0.0f};
    float lightTrimDb{0.0f};
    float heavyTrimDb{0.0f};

    /// Per-skin mutes, applied at render like every other mute - so muting one
    /// leaves every arbitration decision identical and silences only what came
    /// out.
    bool bare{true};
    bool cloth{true};
    bool light{true};
    bool heavy{true};
};

// ── Mix, player, distance ────────────────────────────────────────────────────

struct MixConfig {
    /// With vanilla's body impacts suppressed, the level calibrates against
    /// footsteps and combat instead. There is no bus control in this engine, so
    /// loudness relative to combat is achieved purely by our own gain.
    float masterGainDb{0.0f};

    /// Per-layer trims, applied after the composite's own balance.
    float transientTrimDb{0.0f};
    float bodyTrimDb{0.0f};
    float subTrimDb{0.0f};
    float grainTrimDb{0.0f};
    float loopTrimDb{0.0f};

    /// Below this a cue is not worth a voice and is dropped before the cap sees it.
    float voiceFloorDb{-48.0f};
};

// -- Balance: the composite's shape, per body part ----------------------------

/// One part's share of the composite, as four trims over `MixConfig`'s global
/// ones. A torso landing and a forearm landing are built from the same four
/// layers in the same proportions today and should not be: the mass layer carries
/// a body, the sub is a body's boom, and out on a stick there is less of both.
///
/// Every field is 0, so the block is inert until somebody tunes it - nothing here
/// is measurable from a recording. Which contact was *loud* is physics and the
/// corpus answers it; how much sub a forearm should have is taste (00-Design §3).
struct LayerBalanceConfig {
    float transientTrimDb{0.0f};
    float bodyTrimDb{0.0f};
    float subTrimDb{0.0f};
    /// The surface skin - what it *hit*, coloured by which part hit it. Out on a
    /// hand the floor is most of what there is to hear, so this is the one most
    /// likely to want to go up rather than down.
    float surfaceTrimDb{0.0f};
};

/// Per-part trims on the composite's four layers, at the Trim stage.
///
/// It lands in `LayerTrimDb`, after arbitration, so it cannot change what was
/// chosen - the sort, the rate cap, the chain merge and the masking drop all read
/// `Proposal::levelDb`, which this never touches (01 §5, `config.md` rule 4).
///
/// **The range is asymmetric.** A cut is safe in the strong sense: the mix ends up
/// quieter than the arbitrator predicted. A boost only avoids reordering, and it
/// widens the gap `Proposal::priorityDb` is waiting on - masking, `maskCeilingDb`
/// and `compressCutDb` all read the pre-trim number, so a layer boosted 10 dB sits
/// over the ceiling that judged it. Hence -18 to +6 in the schema.
///
/// **The one way a Trim here can still change what plays.** `Emit` drops a layer
/// under `Mix:fVoiceFloorDb`, and a proposal whose every layer falls under it
/// emits nothing - rolling the whole provisional commit back (01 §6). Measured on
/// `Proventus_Avenicci_devbench_5` with all four limb trims at -18:
///
/// ```
///   default   proposed 348 | rate 115 chain 5 mask 12 burst 54 | emitted 185
///   -18 all   proposed 348 | rate  98 chain 4 mask 15 burst 57 | emitted 135
/// ```
///
/// `proposed` does not move, but every budget downstream of a rolled-back stack
/// does. A boost cannot do this. **Cuts can restructure and boosts cannot; boosts
/// drift the arbitrator's prediction and cuts cannot.**
///
/// Not here on purpose: the crunches, the gore and the head accent are per-part
/// one layer up, where they stop a cue being *proposed*. The armour skin is a
/// different axis with its own trim and clamp.
struct CompositeBalanceConfig {
    /// One switch over all twelve, so an A/B is one key rather than a dozen. With
    /// every trim at 0 the feature is already inert, so this is for turning a
    /// *tuned* balance off in one gesture.
    bool enabled{true};

    /// The skull. It plays the torso's mass layer (see `BodySlot`), so this is the
    /// only place a faceplant's composite can be shaped apart from a back-slam's.
    LayerBalanceConfig head{};
    /// The neck and the torso: the column, and the part with a body behind it.
    LayerBalanceConfig spine{};
    /// Arms, legs, and anything off a skeleton we could not name.
    LayerBalanceConfig limb{};
};

// -- Compression: holding the top of one class down ---------------------------

/// How much of its own range each kind of moment is allowed to use, *on the mod's
/// own scale*.
///
/// A compressor and deliberately not a ceiling: with a hard cap everything above
/// it arrives at exactly the cap, so a threshold a few dB too low turns a dozen
/// distinct impacts into a dozen identical ones - measured on log_2 at -20, four
/// separate impacts landed within 1 dB of each other where there had been an 11 dB
/// spread. Above `threshold` the range is squeezed instead: a ratio of 4 means
/// four decibels in become one out, 1 is off, and a large ratio approaches a cap.
///
/// Compressing the *whole* range rather than its top is
/// `Intensity:fDynamicRangeDb`, `PostIntensity:fExtraRangeDb` and the two
/// `fSoftClipKnee`s. The gap this fills is per-class and top-only.
///
/// **The unit.** Thresholds are measured against `Proposal::levelDb` - the number
/// arbitration sorted on, before any trim - not the rendered level, which would
/// fight every volume control downstream. `levelDb` has a real zero, since
/// `onsetGainDb` tops out at exactly 0 dB, so
///
///     fTapDb = -20  means  "start holding a tap once it comes within 20 dB of
///                           the loudest thing this mod can produce"
///
/// and the setting means the same thing at every volume. Bounded input also gives
/// an exact worst case: `threshold + (0 - threshold) / ratio`.
///
/// Two consequences of measuring pre-trim: a slot trim is *not* compressed, and
/// arbitration has already run on the uncompressed level, so this can never change
/// which contact wins the rate cap (config.md rule 2).
struct CompressConfig {
    /// Off by default and behind a flag rather than parked at values nothing
    /// reaches. Every threshold below is 0, which is already a no-op, but leaving
    /// that implicit would make this a section that looks inert and is not.
    bool enabled{false};

    /// Decibels in per decibel out, above a class's threshold. 1 is off, 4 is an
    /// ordinary musical squeeze, 20 is near enough a hard limiter.
    ///
    /// One ratio for every class rather than nine: it is the *character* of the
    /// holding, and wanting that to differ between a tap and a crunch is rarer
    /// than wanting the thresholds to. Defaults to 4 rather than 1 so lowering a
    /// threshold does something the first time.
    float ratio{4.0f};

    // ── the impact composite, band by band ───────────────────────────────────
    //
    // Four lines rather than one, the cut taken **per layer against that layer's
    // own level**. Over one 6.6 s window of `Proventus_Avenicci_devbench_7` the
    // loudest each reached was
    //
    //     imp_body       -9.2 dB      <- owns the composite's peak
    //     imp_sub       -11.7 dB
    //     imp_body_limb -15.5 dB
    //     imp_transient -17.4 dB
    //     surf_wood     -21.7 dB
    //
    // so one threshold over the stack could only hold the transient to pay for the
    // body's peak. **This reshapes the stack as level rises, deliberately**: these
    // are a split by frequency, so holding them independently is multiband
    // compression and a hit that would have gone dull all over loses the part that
    // was actually too big.
    //
    // Every one defaults to 0, the top of the range.

    /// The click. The layer that says *when*, and the one least worth holding: 8
    /// dB under the body already, and holding it is how an impact stops reading
    /// as a strike.
    float transientDb{0.0f};

    /// The mass. **The line to reach for first** - it owns the composite's peak
    /// at every intensity, so the headroom is here or it is nowhere.
    float bodyDb{0.0f};

    /// The pitched sub at +65 ms. The longest layer and the lowest, so it holds
    /// the peak longest even where it does not set it, and the layer whose
    /// excursion a soft clip mangles most audibly.
    float bassDb{0.0f};

    /// The same mass out on an arm or a leg. Its own line because it is drier and
    /// 6 dB quieter, so a threshold that holds the torso never reaches it. Read
    /// off the slot the *proposal* named, so it works before anybody has recorded
    /// an `imp_body_limb`.
    float bodyLimbDb{0.0f};

    /// Every other layer of a composite: the surface and armour skins. Both ride
    /// the body ~12 dB under it and neither is ever the layer that clips, so they
    /// share one catch-all.
    float impactDb{0.0f};

    /// The burst filler - the nine of every ten contacts under
    /// `tapBelowIntensity`, and the line most worth pulling down: a tap that can
    /// reach the top of the range is a tumble with no shape to it.
    float tapDb{0.0f};

    float headDb{0.0f};
    float crunchDb{0.0f};
    float goreDb{0.0f};
    float scrapeDb{0.0f};
    float airborneDb{0.0f};
};

/// How many dB the class compressor takes off a moment at this level, as a
/// negative number, or 0 when it is under the threshold or switched off.
///
/// Inline rather than a member of the engine because three places need the same
/// arithmetic: the engine applies it, the testbench explains it, and the timeline
/// draws the height a held cue would have had.
[[nodiscard]] inline float CompressCutDb(const CompressConfig& cfg, float thresholdDb,
                                         float levelDb) {
    if (!cfg.enabled || levelDb <= thresholdDb) {
        return 0.0f;
    }
    // Under 1:1 the compressor would be an expander.
    const float ratio = cfg.ratio > 1.0f ? cfg.ratio : 1.0f;
    const float over = levelDb - thresholdDb;
    return -(over - over / ratio);
}

/// At zero distance the player's own ragdoll is a different acoustic problem,
/// so it runs the same ingest and the same strategies with its own mix.
struct PlayerConfig {
    bool enabled{true};

    /// At arm's length the spatial collapse stops helping and starts sounding
    /// like the audio is inside your head, so attach to the bones instead.
    bool attachToBones{true};

    /// A 30 Hz boom at zero distance through headphones is overwhelming, and in
    /// VR low frequency is felt as much as heard.
    float subTrimDb{-9.0f};

    /// You are the one moving and the visual already tells you.
    bool skipAirborneWhoosh{true};

    float masterGainDb{-3.0f};
};

struct DistanceConfig {
    /// Under this: everything. Between: hero composites only - nobody resolves the
    /// detail at that range. Beyond: stop tracking the actor entirely. Culling is
    /// what keeps a battlefield of ragdolls from becoming a performance question.
    float fullRadius{700.0f};        ///< ~10 m
    float simplifiedRadius{2100.0f};  ///< ~30 m

    // No rolloff parameter: Skyrim attenuates a positioned voice itself through
    // the BGSSoundOutput model, so a second curve of ours double-counted. These
    // radii are budget boundaries - what to spend a voice on - never gain.
};

/// One mute per sound slot, applied at Stage 5 - after arbitration.
///
/// Not the same thing as a strategy's `enabled` flag: those gate at proposal
/// time, so turning one off hands its rate-cap and burst budget back and other
/// cues move in to fill it. Muting here leaves every arbitration decision
/// byte-identical and only silences what comes out, which is the only honest way
/// to A/B a layer. Use `enabled` for "should the mod ship this feature" and these
/// for "is the late sub really doing the work".
struct LayerMuteConfig {
    // the impact composite, layer by layer
    bool impTransient{true};
    bool impBody{true};
    bool impSub{true};  ///< mute this one and the design says the gnarl should leave with it

    // grains and texture. The spine and limb crunch files have no mute of their
    // own - same layer on a different bone, so they answer to the crunch they are
    // a variant of. Silencing one part's damage is `Damage:b<Part>Enabled`, which
    // stops the cue being *proposed* rather than muting one already made room for.
    bool limbTap{true};
    bool crunchGran{true};
    bool goreWet{true};
    bool scrapeGrain{true};

    // loops. The surface-coloured scrape variants have no mute of their own:
    // they are the same layer on a different floor, so they answer to the mute
    // of the loop they are a variant of.
    bool scrapeLoop{true};
    bool scrapeLimb{true};
    /// The bed under both grinds. Its own mute rather than the grind's: silencing
    /// the mass and silencing the grit is the A/B this layer exists for.
    bool scrapeLoopRumble{true};
    bool airWhoosh{true};

    // accents
    bool headImpact{true};
    bool settleRest{true};
};

/// One gain trim per sound slot, applied at Stage 5 beside the mutes.
///
/// MixConfig's trims are per *role*, which is right for balancing the composite
/// against the accents and wrong for a bank: two files in one role are two
/// recordings at two levels. So one number per slot, summed on top of the role
/// trim. Like the mutes it lands after arbitration, so it cannot change what was
/// chosen. (The surface skins' trims live in `SurfaceConfig`.)
struct SlotGainConfig {
    float impTransient{0.0f};
    float impBody{0.0f};
    /// The limb body layer. Its own trim, `imp_body`'s mute. Until somebody
    /// records `imp_body_limb_01.wav` it trims the `imp_body` file the limb
    /// composite falls back to, which sits every limb impact back without
    /// touching the torso's.
    float impBodyLimb{0.0f};
    float impSub{0.0f};

    float limbTap{0.0f};
    /// Three separate recordings at three different levels, so each gets its own
    /// trim even though they answer to one mute.
    float crunchGran{0.0f};
    float spineCrunch{0.0f};
    float limbCrunch{0.0f};
    float goreWet{0.0f};
    float scrapeGrain{0.0f};

    float scrapeLoop{0.0f};
    float scrapeLimb{0.0f};
    float scrapeLoopRumble{0.0f};
    float airWhoosh{0.0f};

    float headImpact{0.0f};
    float settleRest{0.0f};
};

/// Bake the fall-through order for the sound bank. A missing file is a quieter
/// mod rather than a broken one.
struct SlotResolutionConfig {
    /// Random selection repeats immediately, and immediate repeats are what
    /// people notice. A shuffle bag does not.
    bool shuffleBag{true};

    /// Fixed seed so a testbench A/B compares two configs and not two dice
    /// rolls. 0 means seed from the clock, which is what the game wants.
    std::uint32_t rngSeed{0};

    /// Draw every random choice for a cue from its own contact rather than from
    /// one running stream. A single stream makes an A/B useless: adding or
    /// removing one cue early in a take re-rolls the variant and pitch of
    /// everything after it, so two exports differ everywhere instead of where the
    /// change bit. Changing `rngSeed` still re-rolls the whole take.
    bool stableVariants{true};

    // -- conditional variants -------------------------------------------------
    //
    // A file on a slot may ask something of the contact before it is a candidate
    // - "only on stone", "only in plate", or both - and where it applies it beats
    // the plain files rather than joining them. The pack carries the conditions;
    // these three decide whether they are honoured. In [Slots] rather than [Armor]
    // because a condition can be surface-only.

    /// Master. Off, every file on a slot is a candidate everywhere, exactly as
    /// before conditions existed.
    bool conditionalVariants{true};

    /// Honour the surface half, and the armour half. Two switches so "is the
    /// stone-specific set earning its files?" can be answered - turning one off
    /// collapses the ladder on that axis while the other keeps working. A half
    /// switched off has no opinion, which is what `any` already means, so this
    /// needs no separate path through the resolver.
    bool surfaceConditions{true};
    bool armorConditions{true};
};

/// A foot the body lands squarely on takes the fall; a foot that clips the floor
/// on the way past does not. They differ only in the angle of arrival, so the
/// measure is `impactSpeed / bodySpeed` - the cosine of the angle of incidence.
///
/// Measured across three captures of one knockdown: 0.995 for a foot the body
/// drops squarely onto, 0.737 for a knee taking some of it, 0.579 for a foot
/// clipping the floor while the body carries on into a whiplash.
///
/// Scoped to the lower body, and that is not optional: a head dive comes in at
/// 0.611 and a thrown-out hand lower still, and both should stay loud.
struct GlancingImpactConfig {
    bool enabled{false};

    /// Transfer at or above this is a square landing and is left alone; at or
    /// below `noTransferFrac` the reduction is full. Between them it ramps.
    float fullTransferFrac{0.90f};
    float noTransferFrac{0.55f};

    /// What intensity is multiplied by at full reduction. Intensity carries the
    /// *class* of the event - the composite/tap split branches on it, so a bad
    /// enough glance demotes itself to a tap - and it is what the arbitrator ranks
    /// by. 1.0 leaves intensity alone and uses only the trim below.
    float maxIntensityScale{0.35f};

    /// Level taken off at full reduction, applied *before* arbitration, so the
    /// landing is quieter and also ranks lower. It does not change the cue's
    /// class: only `maxIntensityScale` crosses the tap threshold. Negative cuts.
    float maxGainCutDb{0.0f};

    /// Level taken off at full reduction, applied after arbitration, so it changes
    /// how loud the landing is and nothing else - not its class, not which contact
    /// wins the hero slot, not the phase. Negative cuts.
    float maxTrimCutDb{0.0f};

    /// A slide is not a clipped landing, but both are mostly-tangential, so the
    /// transfer ratio alone calls a sliding body a fully glancing landing and
    /// turns every contact into a tap. Tangent over closing speed tells them
    /// apart: a clipped foot ~1.4, a knee taking part of a fall 0.35, a squarely
    /// landed foot 0.06, a body sliding on its side 3 to 17. The reduction fades
    /// out between these two.
    float slideRatioStart{2.0f};
    float slideRatioFull{4.0f};

    /// Whether the thigh counts as a landing limb alongside the foot and calf.
    /// Off, only feet and knees are judged.
    bool includeThigh{true};

    /// Below this the limb is barely moving and the ratio is noise.
    float minBodySpeed{60.0f};

    /// Whether the same reduction raises the bar for a crunch, by scaling the
    /// speed the crunch and gore gates compare against. Off: a bone can break at
    /// any angle, and the crunch is already quieter since its level rides on the
    /// intensity the ramp just reduced.
    bool scaleCrunchGate{false};
};

/// Everything RagdollSounds_Algorithm.ini carries.
struct AlgorithmConfig {
    /// Upstream of every stage: what the mod is allowed to hear at all.
    GameIntegrationConfig game;
    IngestConfig ingest;
    /// Stage 2's two axes. `motion` is what the body is doing, `hero` is what
    /// the mix is doing; see Types.h for why they are not one value.
    MotionConfig motion;
    HeroConfig hero;
    ArbitrationConfig arb;
    IntensityConfig intensity;
    EffectiveMassConfig limbs;
    GlancingImpactConfig glancing;
    StrategiesConfig strategies;
    /// Every surface-driven decision, gathered out of four other sections.
    SurfaceConfig surfaces;
    ArmorConfig armor;
    MixConfig mix;
    /// `mix`'s per-layer trims again, split three ways by body part: one sets the
    /// composite's shape, the other says how far a forearm may differ from a spine.
    CompositeBalanceConfig balance;
    CompressConfig compress;
    PlayerConfig player;
    DistanceConfig distance;
    SlotResolutionConfig slots;
    SlotGainConfig slotGains;
    LayerMuteConfig layers;
};


/// The render-time mute for one slot, or nullptr for a slot that has none.
///
/// One mapping rather than two: Stage 5 reads it and the testbench's slot panel
/// writes through it, and a second copy of the switch is how a panel and an
/// engine come to disagree about what is muted.
///
/// The two declared-and-unfilled slots have none, for the same reason they have
/// no trim: nothing resolves to them.
[[nodiscard]] bool* LayerMute(AlgorithmConfig& config, SlotId slot);
[[nodiscard]] const bool* LayerMute(const AlgorithmConfig& config, SlotId slot);

}  // namespace rds
