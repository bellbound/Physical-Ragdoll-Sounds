#pragma once

// The two config objects, as plain structs.
//
// Object-based rather than key-value: the engine reads `cfg.arb.rateCapMs`, not
// `GetFloat("Arbitration:fRateCapMs")`. The ini file and the testbench's sliders
// are both driven off one description table in ConfigSchema.h that maps keys to
// members, so adding a parameter is one field plus one line of schema and both
// the file format and the UI follow.
//
// Everything here must stay standard-layout and trivially copyable: the schema
// addresses members by offset, and the testbench swaps whole configs between
// audio callbacks by copying the struct.
//
// Defaults are the design's numbers. Where a number came from a measurement the
// comment says which one, because a default with no provenance is a number
// somebody will change without knowing what it cost to find.

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
/// every surface in the game to the same dirt sample. Where we are playing the
/// collision ourselves that is a double with half the mix out of our control, so
/// there we drop it.
///
/// Only there. The drop is claimed per actor per tick by the actor's own phase -
/// ragdolling, or in the get-up blend - and a body impact anywhere else keeps
/// vanilla's sound. VanillaGate.h holds the rule; VanillaSuppression.h holds the
/// global fallback for a runtime where the per-call hook cannot be placed, which
/// cannot make that distinction at all.
struct SuppressionConfig {
    bool suppressVanillaBodyImpacts{true};

    /// How far from a claiming actor a body impact still counts as theirs, in
    /// world units.
    ///
    /// The play carries a world point and no reference - VanillaGate.h says why -
    /// so this is what turns "an actor is down here" into "that collision was one
    /// of theirs". It has to cover a sprawled body from wherever the game says
    /// the actor is: 150 units is a little over two metres, which reaches a flung
    /// arm with room to spare and still leaves the NPC standing over the body
    /// their own footsteps.
    float suppressionRadius{150.0f};
};

/// The two engine-side numbers that cannot be guessed from here.
///
/// Both exist because the right answer is a property of the load order rather
/// than of the design, which is the same reason SkyrimNet made its own output
/// model a setting rather than a constant.
struct AudioConfig {
    /// The BGSSoundOutput every voice of ours is opened with, as a form id.
    ///
    /// LOAD-BEARING. Without a model a sound is a flat 2D voice that follows the
    /// listener round the room - no distance falloff, no reverb send, no VR
    /// spatialisation - and a null model is worse than a dry one, because with
    /// nothing to consult the engine loses the mono channel handling a model
    /// declares.
    ///
    /// We run no reverb of our own, so the send on this record is the whole of
    /// how wet we sound.
    ///
    /// The default is Skyrim.esm's `SOMMono02000` (0x0007F80C): 3D/HRTF, 150 to
    /// 2000 units, 30% send. That is the dry half of the pair vanilla's own body
    /// impacts use, and it is chosen over the wet half deliberately - through
    /// `SOMMono02000_verb` at 85% a heavy impact came back off the room long
    /// after the body had stopped, which is a tail vanilla gets away with on one
    /// sample and we do not on a composite. Skyrim.esm has nothing between the
    /// two at this distance range, so drier is the only way down.
    ///
    /// An audio overhaul may have a better-suited record; the log lists what the
    /// load order offers at debug level so it can be found.
    std::int32_t outputModelFormId{0x0007F80C};

    /// The model taps are opened with instead, or 0 to give them the same one as
    /// everything else.
    ///
    /// It exists for one reason: reverb send is a property of the record and not
    /// a knob on the voice, so "this layer drier than that one" can only be said
    /// by opening the voice under a different model. Vanilla says it the same
    /// way - its light body impacts play through SOMMono01400/01800 at 30% send
    /// and its heavy ones through the _verb pair at 85%.
    ///
    /// 0 while the impacts are on the dry model themselves, which is where the
    /// mix wanted them: with both ends of the pair at 30% there is nothing left
    /// for this to say. It stays because the moment the model above moves the
    /// question comes back, and there is no other way to ask it.
    ///
    /// Per voice and therefore per composite, never per layer: a tap's surface
    /// and armour colour are mixed into the tap's own buffer before anything is
    /// opened, so they go wherever the tap goes. See GameRenderer::Update for
    /// what a group has to be for this to apply to it.
    std::int32_t tapOutputModelFormId{0};

    /// DirectInput scancode that fires one canned impact composite at the player.
    /// 0 is off, which is the shipping value.
    ///
    /// It exists because the renderer has to be provable on its own: without it
    /// the first thing that can make a sound is the whole contact pipeline, and a
    /// silent mod would have half a dozen candidate causes instead of one.
    std::int32_t testCueKey{0};
};

/// The testbench link.
///
/// Off in a shipping install and deliberately so: it opens a loopback socket,
/// streams every contact out of the process, and lets another program replace
/// the algorithm config and the sfx table while the game is running. That is a
/// development tool rather than a feature, so the whole subsystem sits behind
/// one flag and nothing else turns it on - with `enabled` false the plugin
/// makes no socket call at all.
///
/// Failure is always silent. The testbench not being up is the ordinary case,
/// and a mod that complains about it every launch is a mod that gets its
/// logging turned off.
struct DevbenchConfig {
    bool enabled{false};

    /// Loopback only. Nothing here is reachable from another machine.
    std::int32_t port{27860};

    /// Where OBS Studio lives, so the testbench can start it when it is not
    /// already running.
    ///
    /// The game never launches anything - this key is read by the *testbench*,
    /// which loads the same file. It lives here because there is nowhere else
    /// for it to be true: the algorithm ini is one of many you A/B between, and
    /// the path to OBS is a property of the machine.
    char obsPath[260]{};
};

/// What the mod costs the frame it runs on, measured in the game rather than
/// asserted from the testbench.
///
/// The testbench's own benchmark already reports a per-tick figure and it is a
/// good one, but it times `RunOffline` and nothing else - so it cannot see the
/// two halves that only exist in the game: walking the ragdoll's limbs to
/// publish a tick, and handing finished cues to Skyrim's sound manager. Those
/// are the halves most likely to cost something, and until now no number
/// anywhere covered them.
///
/// It also answers a question a throughput figure cannot. A realtime factor is
/// an average over a whole take; what a VR frame budget cares about is the
/// *worst* frame, and the two come apart exactly where it matters - the frame
/// with a fat manifold on it and a burst going out.
///
/// Off in a shipping install. With it off the frame path takes one bool test
/// per span and reads no clock at all.
struct BenchmarkConfig {
    bool enabled{false};

    /// How many knockdowns to measure before reporting and switching off.
    ///
    /// It reports once per session and then stops for good, because the figure
    /// wanted is what an ordinary knockdown costs and a log line repeated every
    /// time somebody falls over is a log line nobody reads.
    std::int32_t knockdowns{2};

    /// A ceiling on frames sampled, so a ragdoll that never lets go cannot hold
    /// the report for ever. Reached, it reports what it has and says it was cut
    /// short rather than pretending the run finished.
    ///
    /// 20000 is about three and a half minutes of continuous ragdoll at 90 Hz,
    /// which no knockdown in the capture set comes close to.
    std::int32_t maxFrames{20000};

    /// Frames costing more than this are counted separately, and the count is
    /// the line worth reading: a mean says what the mod costs and this says
    /// whether it ever spiked.
    ///
    /// 250 us is a fortieth of a 90 Hz frame - far above anything measured
    /// (the testbench's whole-engine figure is 0.3-2.8 us per tick) and far
    /// below anything a player could feel. A non-zero count here is the only
    /// result that makes optimising worth considering.
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

/// What the mod is allowed to hear, and when. The only section that is a
/// question about the *game* rather than about sound.
///
/// The mod was built around one assumption: an actor makes these noises while it
/// is ragdolling and at no other time. It is a good assumption and it is why a
/// village of walking NPCs costs nothing - the ragdoll bodies exist and collide
/// the whole time an actor is animated, so without the phase gate every footfall
/// and every brush past a doorframe would come through as an impact.
///
/// This section is the switch that takes the assumption away, so the thing on
/// the other side of it can be heard. Everything here is off by default and no
/// path below is reachable with `animatedMode` off, which is what keeps the
/// recorded corpus replaying byte-identically.
struct GameIntegrationConfig {
    /// Bypass every ragdoll check the mod has, in the feed and in the engine
    /// both: track any actor inside the cull radius rather than only a knocked
    /// one, let the contact callback through whatever the phase is, and admit
    /// contacts stamped `kAnimated` at ingest.
    ///
    /// This is an experiment, not a shipping mode. Ordinary movement is a great
    /// deal of collision - a walking NPC is in continuous contact with the floor
    /// and intermittent contact with itself - and every rule downstream was
    /// tuned against falls.
    bool animatedMode{false};

    /// Whether the sliding grind may run while the actor is animated.
    ///
    /// A foot dragging on the floor is the most likely thing to open a slide
    /// during ordinary walking, and a grind is a *loop*: it does not stop until
    /// the motion axis leaves `kSlide`, so a false one is a sound that stays.
    bool animatedSlide{true};

    /// Whether the cloth rustle may run while the actor is animated. This is the
    /// one layer that arguably belongs in normal gameplay - a body walking is a
    /// body whose clothes are moving - and it is the reason the pose is still
    /// published for an animated actor when any of these three is on.
    bool animatedRustle{true};

    /// Whether air time may be measured while the actor is animated: a jump, a
    /// fall off a ledge that never knocks the actor down.
    ///
    /// Off, `FlightFor` returns nothing for an animated actor and every rule
    /// that ramps on air time - the head accent's lift, the hero test's arrival
    /// clause - sees a body that has been on the ground the whole time.
    bool animatedAirTime{true};

    /// How long an animated actor may go without an admitted contact before the
    /// engine lets go of it.
    ///
    /// A knockdown has an end: `ragdoll_end` arrives, the actor is released, the
    /// summary is written and the per-knockdown budgets - the burst, the hero
    /// count - start over. Animated mode has no such edge, so without this an
    /// actor acquired on their first footstep is held until they walk out of the
    /// cull radius, with a hero budget that ran out minutes ago.
    ///
    /// Only ever consulted for an actor that is animated *now*, so a long quiet
    /// stretch in the middle of a fall cannot end a knockdown early.
    float animatedIdleReleaseMs{3000.0f};
};

// ── Stage 0: Ingest ──────────────────────────────────────────────────────────

struct IngestConfig {
    /// Contacts below this never enter the pipeline. The capture's own floor was
    /// 5 u/s and produced a great deal of nothing; 20 is where the data's
    /// blow-up check first has anything to disagree about.
    float minImpactSpeed{20.0f};

    /// How far the floor above comes down for a contact that is sliding rather
    /// than arriving, and the tangent speed at which it is all the way down.
    ///
    /// The floor gates on *closing* speed, and a clean flat slide has almost
    /// none by definition - so the better the slide, the more certainly it is
    /// thrown away. Measured on Nazeem_devbench_1: 686 of 2332 ragdoll contacts
    /// sit under the floor, 89 % of them are grazes by ratio and 43 % carry more
    /// than 200 u/s of tangent. One slide there ends at 95.3 s with the body
    /// still doing 354 u/s, because every contact after 95131 ms closes at 6-14
    /// u/s and never reaches `IsGraze` to hold it open. The grind goes quiet
    /// with it: `UpdateContactFraction` only counts limbs that stamped
    /// `grazeMs`, so the body loop reads a body that has left the floor.
    ///
    /// This is the Admit-stage rule 01 §5 lists as vacant. The one that used to
    /// live there scaled the floor on whether a slide was *running*, which ingest
    /// cannot honestly know - it runs before Stage 2, so the state it would read
    /// is last tick's (01 §7.4). This asks the contact instead: tangent speed is
    /// on the event, it is measured, and it needs nothing inferred.
    ///
    /// It only ever *admits*. Nothing here can silence a contact that was going
    /// to be heard, and a contact let in this way is still judged by everything
    /// downstream - intensity, the tap branch, masking, the burst budget.
    ///
    /// **1.0 disables it outright** and is the default: at 1.0 the floor is
    /// unchanged at every tangent speed, so the ramp is never evaluated.
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

    /// Publish every ragdoll limb's position and velocity once every N ticks.
    ///
    /// This is the only measurement of where the body actually *is*. Without it
    /// air time can only be inferred from the gaps between contacts, which reads
    /// a body that has merely stopped touching anything as airborne - maximal at
    /// ragdoll_start, when the actor was standing on the floor a frame earlier.
    ///
    /// 1 is every tick, which is what the engine wants and what a capture should
    /// carry. Higher decimates; 0 turns it off entirely and puts the engine back
    /// on the inference. A knob rather than a constant because it is the one
    /// setting that trades capture size against how well a fall can be measured,
    /// and that trade is worth being able to make without a rebuild.
    std::int32_t bodySampleEveryNTicks{1};

    /// Contacts on one (limb, other body) pair inside a frame collapse to their
    /// max. Do not use manifoldFirst/manifoldLast to bracket this - the flags
    /// do not pair up.
    bool collapseManifolds{true};

    /// tangentSpeed / impactSpeed above which a contact is a graze rather than a
    /// thud, and feeds the scrape path instead of the impact path. Grade B as an
    /// interpretation: nothing in the capture set is actually a slide.
    float grazeRatio{1.5f};

    /// Closing speed above which nothing is a graze, whatever the ratio says. A
    /// body arriving this fast is being hit by the world, and the sideways
    /// component is something it does *as well*, not instead. Without the
    /// ceiling a fast skid classifies as a scrape and the impact path never sees
    /// the loudest contacts in the take.
    float grazeMaxImpactSpeed{220.0f};
};

// ── The modifier contract ────────────────────────────────────────────────
//
// Three rules mutate a contact between ingest and arbitration - the glancing
// landing, the head's air time and the body's air time. They used to hand-roll
// the same dance each time, and the comments said so: "built exactly like the
// two above and for the same reasons". They re-derived the onset gain from the
// intensity delta by hand, capped the lift by hand or not at all, and each
// accumulated its own trim into its own field.
//
// So the pipeline is declared instead, in four stages with a contract each:
//
//   Admit   veto, speed floor            nothing today
//   Shape   intensity, onsetGainDb       glancing, head air, body air
//   Budget  grains, gaps, caps, masking  the hero moment
//   Trim    loudness only                post-intensity, role and file trims,
//                                        the motion/hero trim, the compressor
//
// Two invariants, written here once instead of repeated in four comment blocks.
//
//  - **Anything that changes rank is bounded, and never touches `rawIntensity`.**
//    A level added before arbitration is also a *rank*, so an unbounded one does
//    not merely make a contact loud - it makes it outrank everything in the
//    frame. And Stage 2 must read the untouched figure, or a lifting rule can
//    walk the actor into a different motion state and quieten everything after
//    it.
//  - **Anything at Trim cannot change what was chosen.** By the time it runs the
//    slot, the layer balance and the pitch are all decided; loudness is the only
//    thing left, which is what makes it safe to turn while listening.

/// What one Shape-stage rule may move.
///
/// `maxLevelDb` bounds where the contact *lands*, not how much is added -
/// `gainDb` already bounds the addition on its own, so a second cap on it would
/// be a no-op. Zero is the engine's own natural ceiling, since `onsetGainDb`
/// tops out at exactly 0 dB for the hardest contact the engine can hear. Only a
/// lift is capped; a cut is left alone.
struct ShapeLift {
    float intensity{0.0f};   ///< class and layer balance, and therefore rank
    float gainDb{0.0f};      ///< level and rank, before arbitration
    float maxLevelDb{0.0f};  ///< where the lift may land
    float trimDb{0.0f};      ///< loudness only, after arbitration
};

/// What one Budget-stage rule may waive. Never a level, for the reason above.
///
/// Both fractions are *how much is waived*, so 0 asks for nothing and 1 waives
/// the rule entirely. They have to point the same way: `Grant` combines two asks
/// by taking the more generous, and a field that meant the opposite would make
/// that comparison silently pick the wrong one.
struct BudgetWaiver {
    float burstGapFrac{0.0f};
    /// How much of the global rate cap is waived. A *scale* rather than a switch
    /// on purpose: the interesting question at a landing is not "cap or no cap"
    /// but how close two onsets may get, and the two things that turn up in one
    /// frame are not alike. Contacts on successive frames land 18 ms apart and
    /// read as a cluster; contacts in the *same* frame land 0.1 ms apart and do
    /// not read at all - they sum, +6 dB, and clip. Nothing in the corpus lands
    /// between 1 and 17 ms, so a scaled cap anywhere in that gap keeps every
    /// cluster and rejects every stack. A boolean cannot express that, because
    /// it never looks at the gap.
    float rateCapFrac{0.0f};
    bool resetsBurst{false};
};

// ── Stage 2: the motion axis ─────────────────────────────────────────────────

/// What one state is allowed to spend. The trim is in dB against the actor's
/// mix, so the resting phase being nearly silent is one number rather than a
/// special case in the arbitrator.
///
/// Used by both axes: a motion state has one of these and so does the hero
/// latch, and `BudgetFor` picks between them. That is the whole mechanism by
/// which "design owns how loud the mix is" overrides "physics owns what the
/// body is doing" without either axis having to know about the other.
struct PhaseBudget {
    float gainTrimDb{0.0f};
    std::int32_t maxCuesPerBurst{4};
};

struct MotionConfig {
    /// How long off the ground before Airborne is believed.
    ///
    /// `airborneMinHeight` used to sit beside this and is gone. It was never
    /// read by anything, and height is the wrong question: its ground reference
    /// is the last floor contact, which is stale the moment a body starts
    /// travelling - down a staircase it is a step the body left three bounces
    /// ago. What replaced it is `freeFallFrac` below, which needs no ground.
    float airborneMinTimeMs{120.0f};

    /// How much of free-fall acceleration counts as unsupported.
    ///
    /// A body nothing is holding up accelerates downward at gravity: 9.8 m/s^2
    /// against 69.99 units per metre is about -686 units/s^2. Measured across
    /// the real fall in Vayne_impacts_log_2_cut_4 at -675, and across the
    /// opening scuff of that same take at *+229* - the contact the old air-time
    /// rule scored as fully airborne, and which is in fact the body being pushed
    /// up rather than falling.
    ///
    /// A fraction rather than the number itself, so a mod that changes gravity
    /// or an actor with unusual damping moves one value. Steeper than free fall
    /// still counts: a body driven downward has nothing holding it up either.
    float freeFallFrac{0.55f};
    float gravityUnitsPerSec2{686.0f};

    /// How long the acceleration has to look like free fall before the flag is
    /// believed, and how long it survives a tick that disagrees. One noisy
    /// sample either way is a solver artefact, not a landing.
    float freeFallMinMs{40.0f};
    float freeFallHoldMs{60.0f};

    /// How long nothing may have touched the body before Tumble will hand it
    /// back to Airborne.
    ///
    /// Hysteresis *between the two motion edges*, and a correction rather than a
    /// preference. The machine had none: `Airborne` drops to `Tumble` the moment
    /// anything touches, and `Tumble` returns to `Airborne` on `airborne` alone -
    /// which is a latch on the acceleration, and a body bouncing along a floor
    /// is genuinely ballistic between bounces, so the latch stays on for the
    /// whole bounce train. The two edges then took turns, one per tick, with
    /// nothing in the machine remembering that the body had just been hit.
    ///
    /// Measured on Eldawyn_devbench_1 at 20.78-21.13 s: the state alternated
    /// Tumble/Airborne every tick for 300 ms. The motion budget is what levels
    /// the bed (-9 dB airborne against -3 dB tumbling), so both loops came out
    /// with a 6 dB square wave on them at the tick rate - a visible sawtooth in
    /// the take's own waveform, and audible as pumping.
    ///
    /// **It gates the motion label only.** `airborne`, `AirTimeMs()`,
    /// `fallDropUnits` and `driven` are all untouched, so every rule that reads
    /// the flag itself - the hero test's arrival clause, the drop measurement -
    /// sees exactly what it saw before. What waits is the *name* the mix levels
    /// by, and it waits until the floor has stopped arguing about it. 0 restores
    /// the old edge.
    float landedHoldMs{140.0f};

    /// The time constant on the motion trim as the bed hears it.
    ///
    /// The motion budget is a step: -3 dB tumbling, -6 sliding, -9 in the air.
    /// On a one-shot that is right - an impact is an event, and it takes the
    /// trim of the state it happened in. On a loop it is not, because a loop is
    /// not an event: `cloth_rustle` and the two scrapes are one voice held open
    /// *across* the state change, so a 3 or 6 dB step lands on a sustaining
    /// texture as a click, and a train of them is a square wave.
    ///
    /// `fLandedHoldMs` above is the fix for the chatter that made this audible;
    /// this is the guard that keeps the next one inaudible, and it is worth
    /// having on its own terms - Tumble to Slide is a 3 dB step and Slide to
    /// Airborne another 3, and neither of those is chatter. Ramped, the trim
    /// arrives as the body changing what it is doing, which is what it describes.
    ///
    /// Bypass proposals only - the two loops, the armour skins and the settle
    /// cue. Everything contact-derived still takes the discrete value, and
    /// `maxCuesPerBurst` is never smoothed: a budget is a count, and half a
    /// voice is not a thing. 0 restores the step, exactly.
    float bedTrimGlideMs{90.0f};

    /// Whether a flight that something is pushing is treated differently from
    /// one that is only falling.
    ///
    /// Off, the residual below is still *measured* - it costs a subtraction and
    /// a square root per tick, and it is the number you need in order to choose
    /// a gate - but nothing acts on it, and air time means what it meant before:
    /// time since the body left support, however it came to be moving.
    bool drivenEnabled{true};

    /// How far the measured acceleration may sit from gravity before the body
    /// counts as *driven* rather than falling.
    ///
    /// A body nothing is touching accelerates at exactly gravity - straight
    /// down, nothing sideways. Anything else means something is pushing: a leash
    /// hauling on a collar, an Unrelenting Force shout, a blast, a spell that
    /// throws the target across the room. The engine does not need to know which
    /// of those it was, and must not need to: asking the mod that did it only
    /// works for the mods that answer, and there is always one more.
    ///
    /// Measured on a leash yank in Proventus_Avenicci_devbench_3: the genuine
    /// fall at 40.7 s sits at 197-284 u/s^2 of residual, well inside the gate,
    /// and a body being hauled reads 900-1600. A body resting on the floor reads
    /// ~686 - the ground holding it up is a force too - which is why this is only
    /// consulted while the free-fall test already says nothing is.
    ///
    /// A gate of zero would call every flight driven, so it is treated as off -
    /// but `drivenEnabled` is the switch, and this is only the threshold.
    float drivenResidual{450.0f};
    /// Kept true this long past the last driven tick, so one quiet frame in the
    /// middle of a sustained pull does not read as the flight going ballistic.
    float drivenHoldMs{80.0f};

    /// How long after a flight ends a contact still counts as its landing.
    ///
    /// A landing rule asks about a flight that is *over* - "this contact ended a
    /// fall of N ms and M units" - and the flag has usually dropped by the time
    /// the arrival is judged. Without this the air-time rules read the live
    /// clock, which answers a different question and answers it wrong at both
    /// ends: it pays out on mid-air clips, and it reads exactly zero on the one
    /// contact the rules exist for.
    ///
    /// Two to six frames. Long enough that the flag's own hysteresis cannot
    /// close the window before the body arrives, short enough that a corpse
    /// lying still is not still being paid for the fall that put it there.
    float landingWindowMs{120.0f};

    // ── the slide ────────────────────────────────────────────────────────
    //
    // Entry is measured from the contacts and exit is measured from the body,
    // and that split is the whole shape of the rule. A graze is a good signal
    // that a slide has *started* - sideways motion instead of a hit is exactly
    // what sliding looks like to a collision - and a bad signal that one is
    // still going, because collisions are dense when a fall is busy and absent
    // when it is not, so a slide inferred from them ends every time the solver
    // takes a breath.
    //
    // So the slide leaves this state two ways: the body left the ground
    // (`airborne`, the Airborne edge), or the graze stream dried up while it
    // was still on the ground (`SlideExit::kEnded`). The second says nothing
    // about *why* it dried up, and nothing downstream may assume it does.
    //
    // `slideHoldSpeed` is the one thing that may put that second exit off, and
    // it is measured off the body like the first one: the grazes stopping while
    // the body is still travelling is the solver going quiet, not the slide
    // ending. It changes when `kEnded` fires and never what it means.

    /// Sustained tangential contact for this long opens Slide.
    float slideMinTangentSpeed{120.0f};
    float slideMinDurationMs{150.0f};

    /// ...or this far travelled along the surface, whichever comes first.
    ///
    /// Time alone describes a slow grind and misses a fast skid entirely:
    /// devbench_3 crosses two metres of floor in under the 150 ms the time gate
    /// wants, so the slide never opened on the one take that is mostly sliding.
    ///
    /// Measured off the centre of mass where a take carries pose - the honest
    /// answer to "how far did the body go" - and integrated from tangent speed
    /// where it does not. The contact *point* is not usable for this: it hops
    /// between manifold points and between limbs, and its displacement is mostly
    /// jitter that has nothing to do with sliding.
    float slideMinDistance{45.0f};

    /// How long the graze stream may dry up before the slide is over.
    ///
    /// Not a way of ending the slide on the impacts by the back door: what ends
    /// on this timer is the *slide*, and which of the three exits it took is
    /// decided by the body, not by the silence. It is here because a slide with
    /// no grazes at all in it is not a slide any more whatever else is true, and
    /// because a couple of quiet frames in the middle of a long grind is a
    /// solver artefact rather than the body having stopped.
    float slideGraceMs{140.0f};

    /// The exit speed, and the counterpart to `slideMinTangentSpeed` above.
    ///
    /// `slideMinTangentSpeed` is an **entry** gate and always has been: it is
    /// asked of the contacts, once, to open the state. Nothing asks it again
    /// afterwards, and the comment on `slideHolds` in Engine.cpp says why -
    /// re-applying an entry test built out of a decaying contact hold ends
    /// slides on the decay curve rather than on anything the body did.
    ///
    /// So until now a slide had no speed exit at all. It ended when the grazes
    /// stopped arriving for `slideGraceMs`, which is a statement about the
    /// collision stream and not about the body: a corpse still travelling at
    /// 10-20 u/s goes quiet the moment the solver stops reporting contacts for
    /// it, which on a long low grind is well before it comes to rest.
    ///
    /// This is that exit, asked of the **body** instead. Over it, the slide
    /// outlives its own grazes; under it, the grace timer has the last word as
    /// before. Set it under `slideMinTangentSpeed` and the two are hysteresis:
    /// it takes 120 u/s of rubbing to start a grind and rather less to keep one.
    ///
    /// Measured, not inferred - `bodySpeed` off the pose sidecar - and it does
    /// nothing on a take that has none, where the only speed available is the
    /// decaying contact hold and holding on that is the bug above. Zero is off,
    /// which is exactly the behaviour that shipped before it existed.
    float slideHoldSpeed{0.0f};

    /// ...and the longest a slide may be held that way with no grazes at all.
    ///
    /// The speed hold is bounded by the body slowing down, which is what stops
    /// it in every honest case. This is the other kind of ending: a pose stream
    /// that says the body is drifting when nothing is touching it. Counted from
    /// the last graze, so it is the total silence a grind may carry, and it
    /// never shortens an ordinary slide - one with grazes in it never reaches
    /// this timer.
    float slideHoldMaxMs{2000.0f};

    // The slide-end lift lived here, with its own hero clause beside it in
    // `[Hero]`, and both are gone. They existed because the exit was inferred
    // from the contact stream drying up: a slide ended while the body was still
    // travelling, so the collision that "stopped" it had to be found and made
    // bigger by hand to sound like the stop it was. `slideHoldSpeed` measures
    // the body instead, so a grind now outlives its own grazes and ends when the
    // body does - and the contact that ends one is then an ordinary contact,
    // judged on its own closing speed like every other. A rule that stands in
    // for a measurement is not a feature to keep beside it once the measurement
    // arrives (01-Architecture §7.4).

    PhaseBudget launch{-6.0f, 2};
    PhaseBudget airborne{-9.0f, 1};
    PhaseBudget tumble{-3.0f, 4};
    PhaseBudget slide{-6.0f, 2};
};

// ── Stage 2: the moment axis ─────────────────────────────────────────────────

/// When the mix is allowed to have a hero moment, and what one is worth.
///
/// The test this replaces was `frameEnergy >= 0.35 * energyAccum` against a
/// running total that only ever grew, on an intensity that clamps at 1.0. Two
/// things followed. Once the total passed 1/0.35 the test could never be
/// satisfied again, so the hero moment was structurally unavailable after the
/// opening of a fall; and at the very start the running total *is* the first
/// contact, so nearly any first touch qualified - in Vayne_impacts_log_2_cut_4
/// the hero moment was spent on a 44.7 u/s thigh scuff at 696 ms.
///
/// Everything here is measured on raw `impactSpeed` rather than on intensity,
/// which is also what fixes the saturation: intensity is clamped to 1.0, so
/// above `speedRefHigh` every contact reads the same and the test meant to find
/// the biggest moment of a fall was blind at the top of its own range. Closing
/// speed is not clamped and 608 u/s is plainly bigger than 294.
struct HeroConfig {
    /// Off, the moment axis stays Ordinary for ever and every budget comes from
    /// the motion state - which is the old machine minus its hero phase.
    bool enabled{true};

    /// The absolute floor, as a fraction of `IntensityConfig::speedRefHigh`.
    ///
    /// A fraction rather than a speed because every gate in the mod is really a
    /// statement about where it sits in the range the mod hears (00-Design §6),
    /// and written as a raw number that statement quietly stops being true the
    /// moment the anchor moves. At the shipped anchor of 960 this is 288 u/s:
    /// above the 44.7 scuff that used to steal the moment, below the 294 head
    /// that should have had it.
    float floorFrac{0.30f};

    /// Dominance: how much louder than the decaying recent peak a contact has to
    /// be to be the event rather than part of one.
    ///
    /// The envelope it is measured against is the same peak-hold over closing
    /// speed the settle rule reads, taken from before this tick's contacts were
    /// folded in - a contact cannot be 1.3x itself.
    float dominanceRatio{1.30f};

    /// Arrival: a contact that lands out of a genuine measured flight this long
    /// is a landing, whatever else is going on. This is the clause the pose work
    /// exists to make possible, and it is deliberately only available on a take
    /// that carries pose: the old inference reads the first contact of a take as
    /// maximally airborne, because nothing had ever touched, which is exactly
    /// the failure that spent the moment on the opening scuff.
    ///
    /// 250 ms sits between the two sprawls in Vayne log_2 (16 and 35 ms) and its
    /// dive (597 ms) with room on both sides.
    float arrivalMinAirMs{250.0f};
    /// ...and had come down at least this far, in units. Zero asks nothing of
    /// the drop and judges on flight time alone.
    float arrivalMinDropUnits{0.0f};

    // The slide-end clause stood here and went with the lift it rode. It was a
    // way in for the contact the dominance test could not judge fairly, because
    // a slide is a long stretch of grazes and `energyRecent` is low when one
    // ends. That is still true of a slide that runs out of grazes - and a slide
    // no longer ends that way: `Motion:fSlideHoldSpeed` keeps it open on the
    // body's own speed, so a grind ends when the body stops rather than when the
    // solver takes a breath, and whatever hits at that point is an ordinary
    // contact arriving into an ordinary quiet stretch. The dominance clause is
    // exactly right for that.

    /// How long the latch holds. The references' hero moments are a small group
    /// of peers inside a couple of hundred ms, not one hit - a faceplant
    /// genuinely has a knee, a chest and a head.
    float windowMs{220.0f};

    /// A contact this many times the open window's own anchor speed re-anchors
    /// it: the window restarts, the burst budget resets again and the spatial
    /// collapse point moves onto the new contact.
    ///
    /// Re-anchoring rather than opening a second moment is what makes a landing
    /// read as one event with peers instead of three events. It is also why the
    /// moment tracks its own peak rather than its first grain: log_2's 608.7 u/s
    /// slam arrives 132 ms after the 294 u/s head that opened the window, and
    /// the collapse point belongs on the slam.
    float reanchorRatio{1.15f};

    /// What a hero moment is worth. Overrides the motion state's budget outright
    /// while the latch is open - `BudgetFor` is the one place that is decided.
    PhaseBudget budget{0.0f, 5};

    /// Opening or re-anchoring closes whatever burst was open and lets the
    /// moment start its own with the grain count back at zero.
    ///
    /// This is what absorbs the air-time budget reset rather than bolting it on.
    /// The thing a long fall most often lost to was not a gate but a burst that
    /// some scuff had opened and filled three hundred milliseconds earlier,
    /// while the body was still in the air.
    bool resetsBurst{true};

    /// Fraction of `ArbitrationConfig::burstMinGapMs` waived for the contacts of
    /// a hero moment, so the landing is not dropped for arriving too soon after
    /// the burst it just closed. 1.0 waives it entirely.
    float burstGapFrac{1.0f};

    /// The burst window a moment's own burst runs on, in ms. 0 uses
    /// `ArbitrationConfig::burstWindowMs`, which is what shipped before this
    /// existed and is still the default.
    ///
    /// The moment axis already owns how many grains a burst may hold and how
    /// much of the silence after it is waived; this is the third number of the
    /// same shape and it was the one left behind. It is worth its own value
    /// because a hero moment and an ordinary stretch of tumbling want opposite
    /// things from the window.
    ///
    /// An ordinary burst wants a *wide* window: contacts arrive when the solver
    /// happens to report them, and a window shorter than the spread of a fall's
    /// own contacts shuts before the loud one lands - which is how two taps at
    /// 0.03 and 0.07 intensity came to spend a burst and lock out the 465 u/s
    /// hit that arrived seventy milliseconds after it closed.
    ///
    /// A hero burst wants a *tight* one, and for the reason `rateCapFrac`
    /// describes: the reference rhythm is three or four grains inside 20-40 ms
    /// and then several hundred milliseconds of nothing (00-Design §7). The
    /// grains of a landing are already there - they arrive together because the
    /// limbs arrive together - so the window's job on a moment is not to wait
    /// for them but to stop the burst before whatever comes next joins it and
    /// smears the crash into a roll.
    ///
    /// So the two numbers pull apart, and holding them to one value means
    /// choosing which of the two failures to have.
    float burstWindowMs{0.0f};

    /// How much of `ArbitrationConfig::rateCapMs` is waived inside the window,
    /// so the peers of a hero moment may cluster rather than being spaced like
    /// ordinary contacts.
    ///
    /// This is the shape of a landing. Outside a moment the cap is the design's
    /// 46 ms - about where hearing stops resolving two impacts as separate - and
    /// onsets come out evenly spaced, which reads as a metronome rather than as
    /// a crash. Inside one, three or four grains inside 20-40 ms then several
    /// hundred milliseconds of nothing is the reference rhythm (00-Design §7).
    ///
    /// 0.75 of a 46 ms cap is about 11 ms, which sits in the empty part of the
    /// measured distribution: successive-frame contacts at 18 ms and up survive,
    /// same-frame contacts at 0.1 ms do not. Raising it towards 1.0 walks back
    /// into stacking - simultaneous onsets sum instead of clustering - and the
    /// symptom is a peak that doubles without the mix sounding any denser.
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

    /// ...unless the newcomer is this much louder than the onset holding the
    /// cap, in which case it opens its own. The cap's premise is that onsets
    /// this close add mud rather than detail, and that only holds between
    /// comparable levels: a body slamming down 42 ms after a foot lands and 7 dB
    /// above it is a second event, and capping it is how a multi-limb crash came
    /// out as three sounds.
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
    // `config.md`'s third word. A `Weight` is added to `Proposal::priorityDb`
    // and to nothing else: it moves a contact up the arbitrator's sort, it moves
    // the dB comparison the rate cap's override runs, and it is invisible to the
    // mix. Nobody hears a weight. They hear which contact survived.
    //
    // This is what the level could not say. `levelDb` answers "how big was this"
    // and the arbitrator was using it to answer "which of these matters", and
    // those come apart exactly where a torso lands beside a hand: measured over
    // the thirteen devbench takes, 398 torso proposals were dropped with a limb
    // holding the window, and in 63 of them the limb was *quieter* on closing
    // speed - the torso lost on the mass term, the glance cut, or on arriving a
    // tick later. Another 67 lost by under 3 dB.
    //
    // Which is also why the defaults are 0. Every one of these is a decision
    // about the mix that nobody has made yet, and a weight shipped at a guess
    // would be a reordering nobody could hear the arrival of. At 0 the priority
    // *is* the level, arbitration behaves exactly as it did, and the numbers
    // above say where to start looking: 3 dB of torso weight keeps the ~130
    // cases where the arm was barely ahead and leaves the ~270 where it really
    // was the event.
    //
    // Bounded on purpose, and small. A weight is not a veto: at 3 dB a limb
    // genuinely 6 dB louder still wins, which is what "sometimes the arm *is*
    // the sound" looks like as arithmetic rather than as a second switch.
    //
    // Binned through `DamageSiteFor`, the same three-way bin `BodySlot` and the
    // damage tiers use, so a site belongs to one part everywhere in the engine.
    float torsoWeightDb{0.0f};  ///< spine, COM and the neck - the column
    float headWeightDb{0.0f};   ///< the skull. `HeadImpact:bHeroFloorRelief` is already a partial one of these
    float limbWeightDb{0.0f};   ///< arms, legs, and anything off a skeleton we could not read
};

// ── Intensity: speed to loudness ─────────────────────────────────────────────

/// The same shaping again, applied after the layers have been chosen.
///
/// Intensity does two jobs at once and they pull apart under tuning. It decides
/// *what a contact is made of* - the transient/body/sub balance, the pitch bias,
/// which of the two curves a light tap sits on - and it decides *how loud that
/// comes out*. Reaching for the dynamic range because ordinary knockdowns are
/// too quiet therefore also re-balances every composite in the mod, and the two
/// changes arrive together with no way to hear which one did what.
///
/// So these are the same three numbers, re-applied at Stage 5 against the
/// intensity the cue was already built with. Nothing here can move a slot, a
/// layer balance or a pitch: by the time it runs, all of those are decided.
/// Loudness is the only thing left to change, which is exactly what makes it
/// safe to turn while listening.
///
/// Every default is neutral - the shaping cancels to 0 dB - so the mod behaves
/// identically until one of them is moved. Contact-derived cues only: the loops
/// and the settle cue carry no intensity of their own, and shaping them by a
/// number they never had would just make the bed jump.
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
    /// Sits above anything a fall can produce.
    ///
    /// It used to be a second gate under the body's gore, which made the most
    /// extreme contacts the mod can see *harder* to hear. It relaxes the damage
    /// limits now instead - see `DamageConfig::obliterateBudgetBonus`. The point
    /// itself is unchanged; only what happens past it is.
    float obliterateFrac{1.46f};

    PostIntensityConfig post;
};

/// How much mass is behind a contact.
///
/// The problem this fixes: `NominalMass` says how heavy a limb is *on its own* -
/// a foot about 1, a thigh about 7 - and we were using that to set loudness. But
/// a body landing feet-first delivers the whole body's momentum through the
/// foot. The foot is the doorway, not the thing coming through it, and pricing
/// the doorway made log_11's foot landing 23 dB quieter than its hip landing at
/// the same speed.
///
/// The mirror case matters just as much: a foot clipping a wall while the body
/// sails past really is only the foot. Same limb, same table entry, a fraction
/// of the mass behind it. So limb identity cannot answer the question at all.
///
/// What can, from data we already have per contact and without looking ahead:
/// **how much of the limb's motion is translation rather than rotation.** A limb
/// travelling with the body barely rotates; a limb whipping round a joint
/// rotates hard. Measured on log_11 - the foot landing is 536 u/s at 1.8 rad/s
/// and the hip landing 563 u/s at 5.2 rad/s, both the body arriving, while the
/// calf whipping past between them is 451 u/s at 25.0 rad/s and the loose foot
/// 168 u/s at 42.6 rad/s.
struct EffectiveMassConfig {
    /// Off by default, so this can be A/B'd against the old behaviour rather
    /// than replacing it silently. On, loudness follows how much mass is
    /// actually arriving; off, it follows which limb happens to be touching.
    bool massDeterminesLoudness{false};

    /// The mass a fully coupled contact is worth, on NominalMass's scale (where
    /// the reference is 2.5 and the trunk 7.5). This is the single number that
    /// says how loud "the whole body lands" is, and every coupled contact gets
    /// it regardless of limb - which is the entire point.
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
    /// live here, and this is the line they fall under - the only place in the
    /// engine where a level decision changes a cue's *class*, so it is worth a
    /// slider rather than a constant.
    float tapBelowIntensity{0.15f};

    /// Layer offsets from the contact frame, in ms. Measured in the references:
    /// transient at 0, body at +8..34, weight at +46..100, sub at +64..74.
    /// Structured, not random - random jitter smears the shape rather than
    /// building it.
    float transientOffsetMs{0.0f};
    float bodyOffsetMs{20.0f};
    float subOffsetMs{65.0f};

    /// A few ms of scatter each time so the composite envelope is never
    /// identical. Voices start on frame boundaries anyway, which quantises the
    /// offsets to 7-20 ms and doubles as free scatter.
    float offsetScatterMs{4.0f};

    /// Loudness comes from layer balance, not from tiers. A light contact is
    /// mostly transient with almost no sub; a heavy one is sub-dominant with the
    /// transient riding on top. Two endpoints per layer, interpolated by
    /// intensity, which gives a smooth continuum with no boundaries to hide.
    float transientGainAtMinDb{-5.0f};
    float transientGainAtMaxDb{-18.0f};
    float bodyGainAtMinDb{-8.0f};
    float bodyGainAtMaxDb{-2.0f};
    float subGainAtMinDb{-30.0f};
    float subGainAtMaxDb{0.0f};

    /// Pitch is free and continuous in this engine, and it beats doubling the
    /// bank. Random scatter per voice plus a systematic downward bias with
    /// intensity, so a heavier impact starts lower and reads bigger.
    float pitchScatterSemis{2.5f};
    float pitchIntensityBiasSemis{-3.0f};

    /// Stay inside this or the pitch trick starts sounding like a pitch trick.
    float pitchMaxSemis{3.0f};
};

/// Air time: how long the body had been clear of the world when a contact
/// arrived, and what that is allowed to do to the sound.
///
/// It began as the head's own lead rule, and the measurement is the reason it is
/// no longer only the head's. A dive and a sprawl produce head contacts the
/// closing speed cannot tell apart - Vayne log_2 has a 402 u/s head that is the
/// tip of a spine whip and a 294 u/s head that is a genuine faceplant. What
/// separates them is how long the body had been clear of the world when the head
/// arrived: 16 and 35 ms for the two sprawls, 597 ms for the dive. A
/// seventeenfold gap, and one timestamp per actor to carry it - and that one
/// timestamp answers the same question for every other limb too.
///
/// There are two halves to spend it on, both reading one measurement, and both
/// are **Shape**-stage rules - they decide how big a contact is *built*:
///
///  - the **head** half, the old lead rule, which moves the head accent's own
///    gate, level and crunch chance;
///  - the **body** half, which is the same idea for everything that is not the
///    head - the difference between a limb that came down at the end of a fall
///    and the same limb clipping the floor mid-tumble is air time, and nothing
///    else in the pipeline can see it.
///
/// There used to be a third, the **budget reset**, and it is gone: deciding
/// whether a landing is *heard* is a budget question and belongs to the moment
/// axis, not here. Air time is still the evidence - it is `HeroConfig`'s
/// arrival clause - but it now buys a hero moment rather than a private waiver.
/// Keeping both would have meant two rules reading one measurement to bend the
/// same budgets with two per-knockdown counters.
///
/// Every measurement is taken from the value the actor held *before* this tick's
/// contacts were folded in, so an arm coming down in the same frame as the head
/// is read as part of the same strike rather than as evidence the body was
/// already down.
struct AirTimeConfig {
    // ── The head half: the head arriving first, with the body still clear ────

    bool headEnabled{false};

    /// How long the body must have been clear of the world for the head's air
    /// time to count fully. The ramp is linear from zero to here.
    float headClearMs{250.0f};

    /// A hand is what you put out in front of a dive, so a hand touching down
    /// with the head is part of the same strike rather than evidence the body
    /// was already down. Vayne log_2's faceplant lands its left hand in the
    /// same millisecond as its skull, and counting that hand collapses the air
    /// time to zero on the one contact the rule exists to catch.
    ///
    /// Hands still count as company, and they always count for the body half and
    /// for the reset - this is the head's own measurement only.
    bool headExcludeHands{true};

    /// ...but only for this long. This is the window of forgiveness, so a
    /// *larger* value forgives more: a hand inside it belongs to this strike, a
    /// hand older than it is an ordinary peer again. That is what keeps a body
    /// that broke its fall on one arm, rolled, and clipped its head a second
    /// later from reading as a dive. Zero forgives nothing and makes
    /// `headExcludeHands` do nothing on its own - contacts inside the frame
    /// bucket are still discounted, but a hand a frame or two earlier is not.
    float headHandGraceMs{200.0f};

    /// How much more willing the head gate is at full air time, on the same
    /// scale as `HeadImpactConfig::headDownBonus`.
    float headGateBonus{0.45f};

    /// Level added to the head accent at full air time.
    float headGainDb{9.0f};

    /// The ceiling that boost is allowed to lift the accent to. This bounds
    /// where the accent *lands*, not how much is added - `headGainDb` already
    /// bounds the addition on its own, so a second cap on it would be a no-op.
    ///
    /// It exists because the boost is applied before arbitration, where a level
    /// is also a rank: a large `headGainDb` on top of an already-loud contact
    /// does not merely make the skull loud, it makes it outrank every other
    /// proposal in the frame. Zero is the engine's own natural ceiling -
    /// `onsetGainDb` tops out at exactly 0 dB - so it costs an ordinary head
    /// accent nothing and only bites once the boost would push past what the
    /// hardest possible contact could reach unaided.
    ///
    /// Only a positive boost is capped; a negative `headGainDb` is a cut and is
    /// left alone. Company damping is applied after this and can still take the
    /// accent back below the ceiling.
    float headMaxLevelDb{0.0f};

    /// The post-arbitration half of the head boost, on the same ramp. Nothing
    /// caps it - a trim is not a rank, so it cannot take the frame over - which
    /// makes it the one to reach for when the accent is simply too quiet and the
    /// sort is already coming out right.
    float headTrimDb{0.0f};

    // The head halo lived here: four numbers that lifted the contacts *around*
    // a led head, so a dive read as a body going down rather than as an accent
    // floating over nothing. It is gone, and the hero window is why.
    //
    // What the halo was, structurally, is a peer group and a spatial collapse
    // re-implemented per contact - and the moment axis has both already. A hero
    // window is 220 ms wide and every contact inside it is a peer by
    // construction; `Arbitration:bSpatialCollapseOnHero` puts them at one point.
    // All four numbers defaulted to 0, so nothing that shipped is losing a
    // setting it had.
    //
    // One capability did go with it and is worth naming rather than pretending
    // otherwise: the halo added *intensity*, which moves the layer balance and
    // the composite/tap class, while the hero window only moves budget and trim.
    // If a dive comes out thin, that is the thing to put back - as a lift on the
    // hero window rather than on the head, so it is one rule and not two.

    /// The crunch gate a head at full air time is held to, replacing the head
    /// part's own `DamageTierConfig::atFrac` for that contact only - which is what
    /// puts a crunch on a slow dive without putting one on every fast sprawl.
    /// Zero leaves the crunch gate alone, and so does any value above it: this
    /// only ever lowers the bar.
    ///
    /// It moves *whether*, never *how loud*. The level ramp is still measured
    /// from the tier's configured threshold, so a dive that only crunches because
    /// of this rule comes out at the quiet end of the ramp rather than jumping to
    /// the middle of it.
    float headCrunchGateFrac{0.55f};

    /// The chance such a head over that gate actually crunches. The tier's own
    /// two-ended ramp is not used here: it runs from the tier threshold up to the
    /// cap, and a dive that opened the gate well below the threshold would come
    /// out under the bottom of that ramp and almost never fire - which is the
    /// thing this rule exists to fix. One flat number instead, so the aliveness is
    /// still tunable. 1.0 is always.
    float headCrunchProbability{1.0f};

    // `headClaimsOnset` used to live here, fired by the top of the head's own
    // air-time ramp. The question it asks is real and it has moved to
    // `HeadImpactConfig::claimsOnsetOnHero`, fired by the moment axis instead -
    // see there for why the trigger was the part that was wrong.

    // ── The body half: everything that is not the head ───────────────────────
    //
    // The head half asks "did the skull arrive first". This one asks the plainer
    // question the rest of the body has never been able to answer: was this limb
    // falling, or was it already down? A thigh that touches the floor 20 ms
    // after a shoulder is the second grain of a landing; the same thigh at the
    // same speed after 600 ms of air is the landing. Closing speed does not
    // separate those two either - a tumble reaches the same speeds it fell at -
    // and everything downstream of intensity is therefore working blind.
    //
    // Hands count here, both as the peer that ends the air time and as a limb
    // that can be lifted by it: an arm slapping down after a long drop is a
    // landing, not the arm you threw out in front of a dive.
    //
    // The head is excluded outright. It has its own half above, and letting both
    // fire would stack two boosts on the one contact the whole rule was built
    // around.

    bool bodyEnabled{false};

    /// How long the body must have been clear of the world for a limb's air time
    /// to count fully. The ramp is linear from zero to here, so a contact half
    /// way up it gets half of everything below.
    float bodyClearMs{250.0f};

    /// Intensity added at full air time. Intensity is the loud knob - it moves
    /// the composite's layer balance, the pitch bias, and whether the contact is
    /// a composite at all rather than a tap - so a small number here does more
    /// than a large one in dB, and it is the one that can turn a landing that
    /// was filler into an event. As with the halo, `rawIntensity` is untouched,
    /// so lifting a landing cannot walk the actor into a different phase and
    /// make everything after it louder.
    ShapeLift bodyLift{};

    /// Level added before arbitration, where a level is also a rank - so this is
    /// what stops the landing being crowded out by the scuffs around it.

    /// The ceiling that lift is allowed to reach, exactly as `headMaxLevelDb`:
    /// it bounds where the contact *lands*, not how much is added, and zero is
    /// the engine's own natural ceiling. Only a positive lift is capped.

    /// ...and after arbitration, where it is loudness alone and cannot take the
    /// frame over.

    // ── Budget reset: gone, absorbed by the hero window ──────────────────────
    //
    // Seven numbers used to live here. They handed a landing that had been in
    // the air long enough a fresh arbitration budget - close the open burst,
    // refill the grains, waive the gap and the rate cap - whichever limb it
    // landed on, once per knockdown.
    //
    // That is a second hero moment, implemented in the arbitrator, and its own
    // help text said so: "A fall is one landing; a body bouncing down a
    // staircase is not entitled to a fresh budget for every bounce" is a
    // sentence about hero moments, written where hero moments could not be
    // expressed.
    //
    // Both halves of it now sit on the moment axis, where the question belongs.
    // Its evidence - air time past a threshold - is `HeroConfig::arrivalMinAirMs`
    // and is now measured rather than inferred. Its effect is
    // `HeroConfig::resetsBurst`, `burstGapFrac` and `rateCapFrac`. Its
    // per-knockdown cap is `HeroConfig::maxPerEvent`, which defaults to
    // unlimited because the staircase really does have more than one landing
    // in it.
};

struct HeadImpactConfig {
    bool enabled{true};

    /// Head contacts get their own layer and their own gate, as a multiple of
    /// `IntensityConfig::speedRefHigh` rather than as a speed.
    ///
    /// Every gate in the mod is really a statement about where it sits in the
    /// loudness range - "a head accent belongs on the top third of what this
    /// mod ever hears" - and written as a raw speed that statement quietly stops
    /// being true the moment the loud anchor moves. Re-anchoring the range then
    /// meant re-deriving four thresholds by hand, and forgetting one left a gate
    /// that had silently become either free or unreachable.
    float gateFrac{0.31f};

    float gainDb{-2.0f};
    /// The post-arbitration half of the same voicing. `gainDb` is added before
    /// arbitration, so it is a rank as well as a level - a hot `head_impact`
    /// wav pulled down there also loses the accent its place in the sort, which
    /// is the trap `config.md` was written around. Put voicing here and the
    /// balance changes without the arbitrator noticing.
    float trimDb{0.0f};
    /// Head-down attitude at the moment of contact makes the gate more willing.
    float headDownBonus{0.25f};

    // ── Company: the head arriving inside a pile ─────────────────────────────
    //
    // The other half of the same question. A head that lands with four other
    // limbs in the same frame, and is only 1.13x faster than the fastest of
    // them, is the end of a lever - the loudness belongs to the body. A head
    // that lands with one hand and is 1.43x faster than it is the strike.

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

    /// ...and by this much after it, where it is loudness alone. Damping a
    /// sprawl purely here keeps the head's place in the sort while taking it
    /// down in the mix - useful when the accent is the right cue to spend the
    /// budget on and merely the wrong size.
    float companyTrimDb{0.0f};

    // ── Budget refund: gone, absorbed by the hero window ─────────────────────
    //
    // Six numbers used to live here, buying a hard head strike out of the
    // arbitrator's budgets: log_1 dropped a 537.8 u/s head - the loudest contact
    // in the take - on `burst gap`, because a thigh tap opened a burst 155 ms
    // earlier and the minimum gap between bursts is 300 ms.
    //
    // It is the same budget as the air-time reset, bought with head evidence
    // instead of air-time evidence, and `ApplyHeadRefund` wrote the *same four*
    // proposal fields `ApplyAirReset` did. Under the hero test that 537.8 u/s
    // head clears the absolute floor and the dominance ratio comfortably, so it
    // *is* a hero moment and gets the budget without a second rule, a second
    // gate expressed as a multiple of another gate, or a second per-knockdown
    // counter to keep in step with the first.

    // ── Hero floor relief: the head as the start of a moment ─────────────────
    //
    // The moment axis's floor is limb-blind, and a faceplant is the one contact
    // where that is wrong: the strike a fall is *about* can sit under the floor,
    // or clear it and then fail the dominance ratio against a busy envelope, and
    // the fall comes out with no hero moment in it at all.
    //
    // So a head over a threshold of its own gets the floor lowered, for that
    // contact and no other. The rule itself lives in `AdvanceMoment` - whether
    // the mix treats something as a moment is a moment-axis question and not a
    // strategy's (01-Architecture §7.1) - and it is configured here because this
    // is where a head is tuned.
    //
    // It composes with `claimsOnsetOnHero` below, and that is the point of it:
    // that switch turns a head which *anchored* a moment from an accessory into
    // its own onset, and this is what puts a head strike in a position to anchor
    // one at all.

    bool heroFloorRelief{false};

    /// How hard the head has to hit to earn it, as a fraction of
    /// `IntensityConfig::speedRefHigh` - the same scale as `gateFrac` above and
    /// as `HeroConfig::floorFrac`, because every gate in the mod is a statement
    /// about where it sits in the range the mod hears (00-Design §6), and written
    /// as a raw speed that statement quietly stops being true the moment the
    /// anchor moves.
    ///
    /// Defaulted to the accent's own gate: a head hard enough to be worth an
    /// accent is the natural candidate for being worth a moment.
    float heroFloorReliefAtFrac{0.31f};

    /// ...and how much comes off the hero floor when it does, on that same
    /// scale, so the two simply subtract and the result reads straight off: a
    /// 0.30 floor with 0.10 of relief is a head floor of 0.20. Clamped at zero,
    /// and zero relief is indistinguishable from the feature being off.
    ///
    /// Note what this moves besides the floor. The dominance clause measures
    /// against `max(floor, energyRecentBeforeTick)`, so lowering the floor also
    /// lowers the reference a decayed envelope is clamped to - and that is what
    /// actually lets a head *anchor* a moment rather than merely survive the
    /// first of the two tests. Both halves are deliberate; see `AdvanceMoment`.
    float heroFloorReliefFrac{0.10f};

    /// A head impact that is the anchor of a hero moment stops being an
    /// accessory and becomes its own onset.
    ///
    /// The question is real - a head landing is still a body landing, so the
    /// accent normally rides along with the composite and dies with it - and
    /// only the trigger has changed. It used to be `strike.airFull`, the top of
    /// the head's own air-time ramp, which is a private measurement that says
    /// nothing about whether the *mix* considered this a moment. Now it is the
    /// moment axis: this contact anchored the hero.
    ///
    /// Note what this does not fix. A flag that moves a proposal between
    /// ride-along and onset still moves `fGainDb` onto the arbitrator's sort,
    /// which is the trap config.md was written around. The real answer is the
    /// `Proposal::priorityDb` split that file says it is waiting on; until then
    /// the `fGainDb`/`fTrimDb` pair above is the mitigation - put voicing in the
    /// trim and the sort stops noticing it.
    bool claimsOnsetOnHero{true};

    // Damage - what a head strike is worth in broken bone - used to live here,
    // beside a second and differently-shaped rule for the rest of the body in
    // `CrunchGoreConfig`. Both are gone and there is one rule now: `DamageConfig`,
    // below, tuned three times over.
    //
    // The merge is not tidying. Two rules meant two gates, two level ramps and
    // one shared budget, and the budget was the part that bit: whichever rule
    // reached a contact first spent from it, so switching the body's crunch on
    // could silence the head's - the loud, checkable one - without a single
    // counter moving to say so. Splitting the budget per part and per tier is
    // what actually fixes that; folding the two rules into one is what stops it
    // growing back.
};

// -- Damage -------------------------------------------------------------------

/// One tier of damage - a crunch, or the gore above it - for one part of the
/// body.
///
/// Discrete, because you cannot have thirty percent of a bone break and one
/// played quietly sounds like a bug. A tier that should not be certain is
/// therefore softened with *probability*, never with volume - and the level ramp
/// underneath says how bad it was, which is a different question from whether it
/// happened at all.
struct DamageTierConfig {
    bool enabled{true};

    /// Where the tier opens and where its ramps top out, both as fractions of
    /// `IntensityConfig::speedRefHigh` - so the whole tier structure moves with
    /// the range instead of having to be re-derived every time the anchor does.
    ///
    /// Above the cap a harder contact has nothing more to say through this tier:
    /// it is already at `loudDb` and at `probAtCap`, and everything further
    /// belongs to the tier above it or to the composite. A cap at or under the
    /// threshold is not an error - it is how you ask for a step instead of a ramp
    /// - and collapses to "full from the threshold up" rather than dividing by a
    /// span of one unit, which is the trap that turned the body's old ramp into a
    /// step in the wrong place.
    float atFrac{};
    float capFrac{};

    /// The chance the tier actually fires, at its threshold and at its cap.
    ///
    /// Both default to 1: a threshold means what it says unless you say
    /// otherwise. Pull `probAtGate` down and the bottom of the range becomes a
    /// *maybe* that firms up as the contact gets worse - the shape the body's old
    /// rule had, and the one to reach for when a tier fires too reliably to feel
    /// alive. Pull `probAtCap` down as well and the whole tier is a coin flip.
    ///
    /// A tier left at 1/1 draws no random number at all. That is deliberate: it
    /// keeps switching one part's damage on from re-rolling every variant and
    /// every scatter after it, which is exactly what used to make two exports
    /// differ everywhere instead of where the edit bit.
    float probAtGate{1.0f};
    float probAtCap{1.0f};

    /// The level ramp, in dB against the contact's own onset level, over the same
    /// span the probability ramps over.
    ///
    /// The quiet end sits deliberately well down. Just over a threshold the layer
    /// should be barely there - the point of a ramp is that crossing it is not an
    /// event in itself - and only well above it should it be the thing you
    /// notice.
    float quietDb{};
    float loudDb{};

    /// How long after the impact this lands, measured from the composite's own
    /// body offset so moving the stack moves it too.
    ///
    /// Not zero: a crunch is what the bone *did*, and it reads as consequence
    /// rather than as texture when it arrives a beat late.
    float delayMs{};

    /// How many of these one knockdown may produce, and how close together they
    /// are allowed to come.
    ///
    /// One budget per part per tier, never a shared one. A shared counter lets
    /// the order contacts happen to arrive in decide which layer you hear: a limb
    /// crunch that was dropped on arbitration and never made a sound could spend
    /// the skull's slot on the way past, and no counter anywhere moved to say so.
    ///
    /// The budget resets when the body is launched again - `Motion::kLaunch`,
    /// the only event boundary the engine has. `spacingMs` is the finer control
    /// and the one to reach for first: 0, the default, is off, and any value at
    /// all stops one sprawl arriving as a burst of breaking sticks without
    /// capping what a long tumble is allowed to add up to.
    std::int32_t budget{};
    float spacingMs{0.0f};
};

/// One part of the body, and what a contact on it is worth in broken bone. See
/// `DamageSite` for why the body is three parts and not nine.
struct DamagePartConfig {
    bool enabled{true};

    /// What speed the tiers here are judged on: this contact alone, or how hard
    /// the whole body is being dealt with.
    ///
    /// 0 is the contact's own closing speed and nothing else. 1 is the actor's
    /// recent-energy envelope - the same decaying peak the hero rule measures
    /// dominance against, carried in the same u/s the gates are written in, and
    /// the engine's existing answer to "how violent is what is happening to this
    /// body right now". Anything between is a straight blend.
    ///
    /// It exists because the honest answer differs per part. A skull is judged on
    /// its own arrival: that is the whole checkable claim of the head tier, and it
    /// is why this is 0 there. A femur is not - a leg can snap in a slam it barely
    /// touched, and holding it to its own contact speed alone leaves the bone that
    /// obviously broke silent while the shoulder that landed squarely cracks.
    ///
    /// The envelope has already taken the maximum with every contact of this frame
    /// by the time a strategy reads it, so the blend can only ever raise the speed
    /// a tier is judged on. Turning it up loosens a gate; it can never tighten one.
    float bodyForceShare{0.0f};

    DamageTierConfig crunch;
    DamageTierConfig gore;
};

/// Crunch and gore, for every part of the body, in one rule.
///
/// It replaces two. The head's was deterministic and ramped on level - the harder
/// a skull lands the worse it is, every time, and that is checkable. The body's
/// was a probability gate, softened because nobody can say whether a given tumble
/// should have broken something. Keeping both shapes costs nothing here: a tier
/// whose two probabilities are 1 *is* the deterministic rule, and one whose
/// `probAtGate` is 0.15 *is* the old body ramp. What the merge removed was the
/// second gate, the second level ramp, and the one budget the two of them fought
/// over.
///
/// Three parts, two tiers, and the tiers are independent. Gore is not nested
/// inside crunch: it has its own threshold, its own budget and its own spacing,
/// so a contact bad enough to be wet still sounds wet on a frame where the crunch
/// budget is gone. It used to die with the crunch, which made the rarest and most
/// expensive layer in the mod the easiest one to suppress by accident.
/// How much the violence of the fall itself moves the damage rule.
///
/// **The question this answers is not "how hard was this contact".** That one is
/// already answered, twice over - by `impactSpeed` through the tier ramps and by
/// `bodyForceShare` through the body envelope. This is the other one: *what kind
/// of fall is this contact happening inside*. A medium knock on a body that has
/// been cartwheeling down a staircase for half a second should break something
/// more readily than the same knock on a body that was lying still, and nothing
/// in the rule could previously say so.
///
/// **Windowed, and never point-in-time, and that is not a preference.** The
/// obvious implementation - read the thrash on the tick the contact arrives -
/// is the exact failure 01 §7.4 records for the `driven` gate: *a detector whose
/// evidence the thing it is detecting also produces*. A limb striking stone has
/// its velocity reversed inside one solver step, so the thrash on a contact tick
/// **is** the contact, restated in different units. Reading it would double-count
/// impact speed and call it a second opinion, and every contact would find itself
/// maximally violent by virtue of being a contact.
///
/// So `ActorRuntime` holds a decaying peak-hold that **decays on every tick and
/// rises only on ticks carrying no contact at all**. What it measures is the free
/// thrashing of the body between collisions, which is what "this is a bad tumble"
/// actually means. It also means no `…BeforeTick` snapshot is needed: on the tick
/// damage reads it, the value provably cannot contain this tick's collision.
///
/// Off by default, with the amounts below already set to a usable voicing - so
/// `bEnabled` is the whole of turning it on, and off is byte-identical.
struct DamageViolenceConfig {
    bool enabled{false};

    // -- its own measurement, sharing nothing with the garment ---------------
    //
    // These duplicate the shape of `[Rustle]`'s ramps and deliberately not their
    // values, because the two answer different questions off the same pose
    // stream and the first build of this wrongly had damage reading the
    // garment's figure:
    //
    //  - The garment weights each limb by **how much cloth** hangs on it, then
    //    multiplies by body speed and adds an airborne term. All three are right
    //    for fabric and all three are wrong here. A naked body breaks exactly as
    //    well as a clothed one; a body drifting fast and limply is not breaking
    //    anything; and a long fall is not itself an injury - the landing is, and
    //    the landing is a contact the tiers already judge.
    //  - Violence weights each limb by **`NominalMass`** - how much *body* is
    //    being thrown about, which is the question a breaking bone answers to.
    //
    // Sharing them also had a practical cost worth naming: tuning the rustle
    // moved how often bones broke, and neither slider said so.
    //
    // Always measured relative to the body, with no switch, because there is no
    // reading in which the absolute figure is right here - everything on a
    // falling body accelerates at g together, and free fall is not violence.

    /// The two raw signals, each through its own ramp and then blended.
    /// `thrash` is relative limb acceleration in u/s^2; `tumble` is limb surface
    /// speed from rotation in u/s. See `[Rustle]` for why they cannot be added
    /// before they are normalised.
    /// **Measured on the thirteen takes**, mass-weighted, per tick and only on
    /// the contact-free ticks this term is built from:
    ///
    /// | | mean | peak |
    /// |---|---|---|
    /// | long takes (600-5700 ticks, much of it settled) | 469-726 | 3157-9928 |
    /// | short cuts (55-190 ticks, mostly action) | 630-1596 | 3893-8210 |
    ///
    /// 600 to 4500 puts a settled body under the floor, an ordinary tumble low
    /// in the range and a real slamming near the top, and it leaves headroom
    /// above the worst take: the corpus tops out around 10000 and the worst fall
    /// there is has not been recorded yet. Across the corpus that yields a
    /// violence of **0.08 to 0.46** - a spread that discriminates, which is the
    /// property this term lives or dies by. A first pass borrowed the garment's
    /// ramps instead and read 1.00 on twelve takes of thirteen.
    ///
    /// `rds-verify` prints the mean and peak per take whenever the term is on,
    /// so this is re-measurable in one command.
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

    /// How fast the memory of a violent stretch fades. The averaging window, so
    /// a body that has stopped thrashing stops being treated as though it is.
    ///
    /// Long enough to bridge the gaps between the bounces of a staircase and
    /// short enough that a fall which has settled is judged as settled.
    float holdMs{450.0f};

    /// How much of the answer comes from *this limb* rather than from the body.
    ///
    /// 0 is body-wide only: how bad is this fall. 1 is the contact's own limb
    /// only: how hard is this particular arm being whipped about. Both are real
    /// and they come apart - a body sliding to a stop with one leg still
    /// cartwheeling is quiet on the first and loud on the second - so the blend
    /// is a slider rather than a choice.
    ///
    /// The limb half is deliberately **not** fabric-weighted, unlike the
    /// garment's: a bare arm breaks exactly as well as a sleeved one.
    float limbShare{0.5f};

    /// How far the tier's threshold drops at full violence, as a fraction of the
    /// span between the tier's own threshold and its cap.
    ///
    /// **It moves the gate and never the ramp**, which is the pattern the
    /// air-time head gate already established: a contact admitted only because
    /// the body was thrashing arrives at the *quiet end* of the level ramp
    /// instead of jumping into the middle of it. So lowering the bar does not
    /// also make what comes through it loud - those are two different asks, and
    /// they are two different keys.
    ///
    /// Expressed as a fraction of the tier's own span rather than as an absolute,
    /// so one number is right for all six tiers however far apart their
    /// thresholds sit, and so it can never invert the gate or drive it negative.
    float gateDropFrac{0.35f};

    /// Added to the tier's firing chance at full violence.
    ///
    /// The other half of "occurrence", and the one to reach for first. The gate
    /// drop changes *which* contacts are eligible - it admits weaker ones, which
    /// is the more aggressive edit and the one that can put a crunch on a settle.
    /// This changes how often the already-eligible ones actually fire, which is
    /// the more conservative reading of "more crunching in a bad fall" and leaves
    /// the character of what breaks alone.
    ///
    /// **Dead at the shipped defaults, and deliberately kept anyway.** All six
    /// tiers ship at `probAtGate = probAtCap = 1`, so an eligible contact already
    /// fires with certainty and there is no room above it - this adds to a
    /// probability that is clamped at 1.0 and changes nothing. It becomes live
    /// the moment any tier's `probAtGate` is pulled down, which is the documented
    /// way to make a tier "fire too reliably to feel alive" less reliable. Named
    /// here rather than left to be discovered by a sweep that moves nothing.
    float chanceBonus{0.25f};

    /// Added to the layer's level at full violence.
    ///
    /// The "intensity" half. A tier is discrete - a bone either broke or it did
    /// not - so violence must not be allowed to soften one into existence; it
    /// only makes a break that was already happening sit further forward.
    float levelBonusDb{2.5f};

    /// Extra budget slots at full violence, and how far the spacing shrinks.
    ///
    /// **These are the levers that actually move occurrence, and the corpus is
    /// what says so.** The first pass of this feature had only the gate drop and
    /// the chance bonus, on the reasoning that a budget is what stops a fall
    /// machine-gunning and a violent fall is exactly when that matters most.
    /// Measured, that reasoning was wrong about which constraint was binding:
    ///
    /// | change | crunch/gore proposals over the corpus |
    /// |---|---|
    /// | baseline | 9235 |
    /// | gate drop at its **maximum** | 9243 (+8) |
    /// | crunch budgets +8 each | 9300 (+65) |
    ///
    /// The tiers are budget-limited, not threshold-limited, by roughly eight to
    /// one. Admitting more candidates into a ledger that is already spent
    /// changes nothing you can hear, so a violence term with no budget term in
    /// it is decoration.
    ///
    /// A relaxation and never a waiver, exactly as `iObliterateBudgetBonus` is:
    /// a raised budget is still a budget, so the worst tumble the solver can
    /// produce still cannot turn a fall into a bag of breaking sticks. Rounded
    /// rather than truncated, or a term that peaks near 0.5 on real falls would
    /// grant nothing at all.
    /// Sized against the only denominator that means anything here - crunch and
    /// gore events, of which the corpus produces 156 at the shipped defaults.
    /// `proposedCues` is the wrong one to read: it counts every layer of every
    /// composite, so a real change to the damage layer looks like rounding error
    /// against it.
    ///
    /// | with violence on | events | |
    /// |---|---|---|
    /// | gate drop alone | 160 | +3 % |
    /// | budget bonus 2 alone | 164 | +5 % |
    /// | everything, bonus 3 | 184 | +18 % |
    ///
    /// The two are superadditive, which is the shape you would want: a lowered
    /// gate finds the candidates and a raised budget is what can afford them. 3
    /// is the default because it is plainly audible without turning a bad fall
    /// into a bag of breaking sticks.
    int budgetBonus{3};

    /// **Also dead at the shipped defaults**, for a different reason than the
    /// chance bonus: every tier ships with `spacingMs = 0`, and scaling zero is
    /// zero. Set any tier's spacing and this starts tightening it. Kept because
    /// "a bad fall should crunch in quicker succession" is a real and separate
    /// ask from "a bad fall should crunch more times", and the two want different
    /// keys even while one of them is waiting on a spacing to exist.
    float spacingScale{0.6f};
};

/// One rung of the accumulated-damage ladder.
///
/// Discrete like the tiers, and for the same reason: a bone either broke or it
/// did not, so a rung that should be less certain is made rarer by sitting
/// further up the ladder, never quieter.
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
/// **The gap this fills is measurable.** In `Proventus_Avenicci_devbench_7` a
/// head is bashed against a wall twenty-four times over sixteen seconds. The
/// contacts peak at 371 u/s against a head crunch gate of 432, so *not one of
/// them can ever crunch* - the tier system is asking "was this hit hard enough"
/// and the answer is honestly no, every time. A skull worked on for sixteen
/// seconds should nonetheless come apart, and nothing in the mod could say so.
///
/// So this counts hits instead of judging them. Every admitted contact adds to a
/// pool on **its own limb**, the pool heals over seconds, and crossing each rung
/// of a ladder plays a break. Repetition is the whole signal, which is why it
/// cannot be folded into the tiers: they are a function of one contact and this
/// is a function of a history.
///
/// **It shares nothing with the tiers or with the violence terms** - its own
/// pool, its own ladder, its own budget and its own section. The one thing it
/// borrows is which *slot* a site's crunch plays, because that is a fact about
/// the sound bank rather than a tuning decision.
struct AccumDamageConfig {
    bool enabled{false};

    /// Only the head accumulates, and only the head breaks.
    ///
    /// **The two scopes this feature can have are not the same feature.** Left
    /// open it is "a body that is worked on comes apart", and every limb of a
    /// long tumble is being worked on - so a fall that brushes eighteen limbs
    /// walks all eighteen pools up their ladders and the knockdown ends in a
    /// bag of breaking sticks. Closed to the head it is "a skull bashed against
    /// something gives way", which is the case the measurement in the struct
    /// comment above is actually about.
    ///
    /// Binned through `DamageSiteFor`, so this is the head *and the neck* - the
    /// same three-way bin the tiers and `BodySlot` use, and a site belongs to
    /// one part everywhere in the engine.
    ///
    /// Applied before the pool is touched rather than at the break: a limb that
    /// may never break has no business banking damage it can never spend, and
    /// gating at the top is what makes "off for everything but the head" mean
    /// the limb pools do not move at all.
    bool headOnly{false};

    /// Only while the player has this body in their hands.
    ///
    /// **VR, and HIGGS.** The case this whole ladder was written for is a player
    /// holding a ragdoll by one hand and driving its head into a wall, and that
    /// is a thing the player *does* rather than a thing that happens to a body.
    /// Everything else a knockdown contains - the tumble down the stairs, the
    /// settle, the corpse dragged by a passing draugr - is the fall's own
    /// business and should sound like a fall, not like a beating.
    ///
    /// The hold is published from the game thread as `held_start` / `held_stop`
    /// state rows, exactly like `ragdoll_start`, so it keeps its place in time
    /// against the contacts around it and replays from a recording the same way
    /// it ran live.
    ///
    /// **On flat Skyrim, and in VR without HIGGS, nothing is ever held** - no
    /// `held_start` row is ever published - so turning this on there switches the
    /// accumulated ladder off. That is the honest answer rather than a silent
    /// fallback to "always held": the switch says *require*, and a build that
    /// cannot tell whether the player is holding anything cannot honour it.
    /// Older recordings carry no hold rows either, for the same reason, which is
    /// why this is off by default and the corpus replays as it always did.
    bool requireHeld{false};

    /// How long the pool takes to drain, as a time constant. This is an injury
    /// healing rather than an envelope releasing, so it is measured in seconds:
    /// long enough to bridge the ~1.2 s between one bash and the next, short
    /// enough that two separate falls are two separate histories.
    float healMs{7000.0f};

    /// Contacts under this intensity add nothing. Without it the settling
    /// scrabble at the end of every knockdown slowly fills the pool, and a body
    /// that has come to rest eventually breaks a bone for no reason anyone can
    /// see.
    float ignoreBelowIntensity{0.10f};

    /// What each qualifying contact adds, as a multiple of how far its intensity
    /// clears the floor. Turn it up to make a limb wear out faster without
    /// moving the ladder.
    float perHitScale{1.0f};

    /// The pool's ceiling. A limb being worked on indefinitely stops climbing
    /// rather than banking damage it will spend later, which is what stops a
    /// long beating from firing the whole ladder at once the moment a rung is
    /// reached.
    float maxPool{4.0f};

    /// How hard the blow that actually breaks the limb has to be, as intensity.
    ///
    /// **Reaching a rung arms a limb; this is what fires it.** The pool says the
    /// limb is ready to go and then it waits there, taking more damage and
    /// making no sound, until a contact with real weight behind it arrives.
    ///
    /// It exists because without it the break lands on whichever contact
    /// happened to tip the arithmetic over, and in a beating that is very often
    /// a nothing - the twenty-fourth gentle scuff, identical to the twenty-third
    /// that produced no sound at all. A bone going is the loudest thing this mod
    /// does and it has to land on a hit the player can see land.
    ///
    /// Measured in intensity rather than closing speed so it reads against the
    /// same scale the pool is filled from, and so one number means the same
    /// thing on a skull and on a wrist. Well above
    /// `ImpactComposite:fTapBelowIntensity` (0.15) on purpose: a contact that
    /// would have come out a single tap has no business breaking anything.
    float breakIntensity{0.35f};

    /// How far the pool has to fall below a rung before that rung can fire
    /// again, as a fraction of its threshold.
    ///
    /// **Hysteresis, and it is load-bearing rather than a refinement.** Without
    /// it a pool sitting near a threshold steps down and straight back up on
    /// alternate contacts, firing the same rung over and over: the corpus
    /// produced 51 breaks on one take and 25 on another that way, all of them
    /// the same bone cracking on a loop. A rung is a thing that happened, so
    /// re-arming it has to mean the limb genuinely recovered - not that the
    /// arithmetic wobbled.
    ///
    /// 1 would be no hysteresis at all and is the behaviour to avoid; 0 means a
    /// rung never re-arms inside a knockdown.
    float rearmFrac{0.55f};

    /// The floor under how often one limb may produce a break. The rungs space
    /// themselves, so this guards the case they cannot: a limb sitting exactly
    /// on a threshold while contacts keep arriving.
    float minGapMs{220.0f};

    /// How many breaks one limb may produce in a knockdown, and how many the
    /// whole body may.
    ///
    /// **The body cap is not a formality.** With a per-limb cap alone, eighteen
    /// limbs each allowed six breaks is a hundred and eight, and the corpus went
    /// straight there: the first pass produced 132 breaks on one long tumble and
    /// 90 on another, which is a bag of breaking sticks rather than a body. The
    /// ladder is meant to be the rare, awful thing that happens when something
    /// is *worked on*, and a long fall touching every limb is not that.
    ///
    /// Three per limb because a limb has three interesting states - cracked,
    /// broken, ruined - and a fourth says nothing new.
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

    /// The head. All three parts' thresholds are taken off measured data rather
    /// than judgement, and this is the one that set the practice: a tier pitched
    /// anywhere near the old obliterate frac could not fire on any head ever
    /// recorded, which is a dead feature and not a rare one.
    ///
    /// 409 head contacts in the corpus carry a closing speed. 0.45 is the top
    /// 8.3% of them: a hard landing, not a knock. 0.65 is the top 4.4%. 0.80 was
    /// set just above the hardest head in the corpus as it then stood (732 u/s,
    /// 0.76); the corpus has since grown a 1099 u/s outlier, so the ramp now tops
    /// out slightly before the worst of them rather than exactly at it, which
    /// costs nothing - past the cap the composite is what gets bigger.
    ///
    /// The crunch cap and the gore threshold are the same number on purpose: past
    /// there a harder strike has nothing more to say through the crunch, and
    /// everything above belongs to the gore. Letting both ramp over the top of the
    /// range would make the worst impacts louder twice for one reason, which is
    /// how a tier structure turns into a volume problem.
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
    /// Measured the same way, and it had to be: the corpus holds 2019 spine
    /// contacts and they top out at **837 u/s**, 0.87 of the loud anchor - so a
    /// tier pitched by eye lands above the part's own ceiling and never fires at
    /// all. 0.45 is the top 1.0% of them, against the head's 8.3%, and that ratio
    /// is the point: a body landing on its back is the single most common contact
    /// a knockdown produces, and it must not crack every time. 0.72 is the top
    /// 0.30%. 0.88 sits just above the hardest spine there is, so the gore ramp
    /// spans the real range.
    ///
    /// A third of the blend comes off the body envelope - a spine contact is most
    /// of the body by definition, so what happened to the body is close to what
    /// happened to it.
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
    /// The highest thresholds, and the measurement says why: 6853 limb contacts
    /// in the corpus against 409 heads. They outnumber everything else by better
    /// than sixteen to one, so a gate that suits a skull turns an ordinary tumble
    /// into a bag of breaking sticks here. 0.68 is the top 0.83% of them - 57
    /// crunches across the whole corpus - and 0.95 is the top 0.12%. They reach
    /// 1099 u/s, 1.14 of the anchor, which is what makes a gore tier up here
    /// reachable at all; the cap sits just above it.
    ///
    /// A limb is also the part whose own closing speed says least about whether it
    /// broke, which is what the half share off the body envelope is for.
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
    // `IntensityConfig::obliterateFrac` used to be a second gate underneath the
    // body's gore, ANDed with the gore gate. That was the wrong job for it. It
    // made the most extreme contacts the mod can see *harder* to hear, and it is
    // the reason a gore tier a real fall could actually reach had to be written as
    // a special case for the head alone.
    //
    // It relaxes the limits instead now. A contact past the obliterate point is
    // the one thing nobody will accept a budget refusing, so its budget is raised
    // and its spacing shortened - for that contact only, and without touching what
    // the tier is worth in dB. It is a relaxation and not a waiver on purpose: a
    // ridiculous impulse from another mod still cannot machine-gun the layer,
    // because a raised budget is still a budget.

    /// Extra slots granted to a tier's budget when the contact is past the
    /// obliterate point. 0 holds an obliterate to the ordinary budget.
    std::int32_t obliterateBudgetBonus{2};

    /// What a tier's `spacingMs` is multiplied by for such a contact. 1 leaves the
    /// spacing alone; 0 removes it entirely for an obliterate.
    float obliterateSpacingScale{0.25f};
};

/// Sustained grazing contact drives looping voices attached to the limbs that
/// are doing the grinding.
///
/// Two kinds of loop, not one, and they run *together* rather than one of them
/// being chosen. A body that starts flat and rolls onto a shoulder would audibly
/// snap between two files if the mod picked, and a boundary is the thing a
/// listener hears; the rest of the mod avoids exactly this by blending layers
/// instead of switching between tiers. So:
///
///  - **The body loop** is the full-weight grind, and how loud it is comes off
///    how much of the body is actually on the surface - the contact fraction
///    below. Under `fBodyFracStart` it is silent, which is what stops a corpse
///    dragged by one ankle from sounding like a corpse lying flat.
///  - **The limb loops** are light and dry, one per limb chain, each on the bone
///    inside that chain that is doing the most rubbing. Several run at once,
///    because several limbs can be scraping at once.
///
/// The two are independent, and the only thing that couples them is the optional
/// duck: with the body grind up, the limb loops can be pulled down under it so a
/// full slide is one sound rather than five.
struct ScrapeLoopConfig {
    bool enabled{true};

    // -- ownership -----------------------------------------------------------

    /// Whether a running slide gives its harder contacts back to the impact
    /// path instead of spending them as scrape.
    ///
    /// **A graze outside a slide is never claimed, and that is no longer a
    /// switch.** Claiming one was what the mod used to do unconditionally, and
    /// it was a bug rather than a taste: about half of all worthwhile contacts
    /// in an ordinary tumble classify as grazes, so half of them were taken off
    /// the impact path, no slide opened because a single glancing knock is not
    /// one, and nothing was played in their place. Half the contacts of a
    /// knockdown deleted from the mix and replaced with silence. Nothing wants
    /// that back, so the old `claimWhileSlidingOnly` is gone rather than
    /// inverted, and the impact path always gets a graze that no slide is
    /// running under.
    ///
    /// What is left is the question worth asking, and this is it: *inside* a
    /// slide, is a rub that is also a hit still the slide's to spend? Off, it
    /// is - every graze of a grind is scrape, which is what keeps a skid from
    /// thudding on every frame. On, `claimBelowIntensity` draws the line, and
    /// anything over it is built as an impact as well.
    bool slidesDontClaim{false};

    /// Where that line sits: below this intensity a slide keeps the contact,
    /// at or above it the impact path gets it.
    ///
    /// **Only consulted while `slidesDontClaim` is on.** Off, a slide claims
    /// every graze under it and this number means nothing - which is the
    /// default, and is what the mod shipped with.
    ///
    /// The boolean answers "may a grind give anything back"; this answers "is
    /// *this* contact a hit as well as a rub", which is the question a phase
    /// cannot answer and the reason the pair is a pair. A body grinding down a
    /// staircase produces spine and thigh contacts at 107-164 u/s whose
    /// *intensity* is 0.28-0.49 - mass, radius and coupling are what lift a slow
    /// closing speed that far - and every one of them was spent as a scrape
    /// grain because the ratio said rub. `grazeMaxImpactSpeed` cannot reach
    /// them: it is a ceiling on closing speed and theirs is low. This is the
    /// same idea on the axis that says how big the contact actually is.
    ///
    /// Falling through means the impact path voices it, so no grain is proposed
    /// for it - one collision is one onset, and a grain on top would spend the
    /// burst budget twice for a single hit.
    ///
    /// The default is the tap threshold rather than 1.0. Intensity clamps at
    /// 1.0, so a 1.0 here would hand back nothing at all and the switch above
    /// would read as free and never fire - the dead-gate trap 01 §4 documents
    /// for the head's gore. At `ImpactComposite::tapBelowIntensity` the line
    /// falls exactly where the composite already stops bothering: under it a
    /// contact would have come out a single tap, which a scrape grain stands in
    /// for perfectly well; over it there was a stack to build and the slide was
    /// eating it.
    float claimBelowIntensity{0.15f};

    /// The loops are the *voicing* of `Motion::kSlide` and nothing else. When a
    /// slide starts and stops is decided once, on the motion axis, under the
    /// slide keys in this same section - so the state, the grinding loops, the
    /// budget the impacts riding on it get, and the lane the timeline draws are
    /// four views of one decision rather than four rules that can disagree.
    ///
    /// They did disagree, and it was not subtle. The strategy had its own
    /// duration, distance and speed gates, all of them named differently from
    /// the motion axis' and none of them the same value, so a knockdown could be
    /// in `Slide` with no loop under it or grinding away in `Tumble`.
    /// Long enough to be the grind's arrival and not just a de-click.
    ///
    /// A slide is not declared until 150 ms, or 45 units, into a grind that is
    /// physically already happening - the detector needs that much to tell a rub
    /// from a knock. So the loop always opens late, into a body that has been
    /// scraping for a moment already, and opening it at full level is the
    /// "somebody turned a noise on" that the whole slide rework is about.
    ///
    /// **This does nothing in the game today.** `GameRenderer::StartLoopVoice`
    /// bakes a loop's gain into its PCM buffer and opens it with `Open`; nothing
    /// in the plugin calls `FadeInPlay`, so `fadeMs` on a `kStartLoop` is read by
    /// the offline mixer and by no one else. Tuning it in the testbench tunes
    /// something the game will not reproduce until that is fixed.
    float startFadeMs{140.0f};

    /// What is left to hide once the speed ramp does the fading.
    ///
    /// Short on purpose, and shorter than it looks: with `fSpeedRangeDb` set so
    /// the ramp bottoms out at the voice floor, a grind that ends in friction has
    /// already faded itself to nothing before this is reached, and there is no
    /// step here to hide. This is for the endings that are genuinely abrupt -
    /// the actor released mid-skid, a body out of earshot, the mod switched off.
    float stopFadeMs{110.0f};

    /// The fade used when the slide ended because the body left the ground.
    ///
    /// A slide that ends in friction ends slowly; one that ends because the body
    /// launched ends the instant the surface does, and the ordinary fade drags a
    /// grinding rumble out behind a body that is already in the air. Shorter
    /// than `fStopFadeMs`, and separate rather than shared, because the two are
    /// tuned against different pictures.
    float launchFadeMs{45.0f};

    // -- the body loop -------------------------------------------------------

    bool bodyEnabled{true};

    /// Raised with the ramp below rather than independently of it. The two are
    /// one setting: deepening the speed dependence pulls the whole middle of the
    /// range down with it, so the top has to come up if an ordinary slide is to
    /// stay where it was. At the shipped pair a full-speed skid is 4 dB louder
    /// than it used to be and a 300 u/s one about 5 dB quieter, which is the
    /// speed mattering more, said in numbers.
    float gainDb{-16.0f};

    /// Loop level and pitch track the *body's measured speed* continuously
    /// between these. A slide is exactly as loud as the body is fast, which is
    /// the one thing about it a listener can check against what they see.
    ///
    /// **The bottom is also where the grind stops, so it has to be a speed the
    /// body genuinely reaches.** It used to be 120 u/s, which is a body still
    /// moving at a walking pace: the grind was switched off partway down its own
    /// ramp, 16 dB clear of the voice floor, and `fStopFadeMs` was left to hide
    /// the step. 40 u/s is a body that has all but stopped, so the ramp has
    /// somewhere to go and the ending is the body slowing down rather than a
    /// voice being cut.
    float speedForMinGain{40.0f};
    float speedForMaxGain{600.0f};

    /// How far under `fGainDb` the loop sits at `fSpeedForMinGain`. The depth of
    /// the speed dependence, in other words: at 0 dB the slide is one level
    /// whatever it is doing, and deep enough that a crawl is inaudible is what
    /// keeps a body settling on its side from sounding like it is being dragged.
    ///
    /// **Set so `fGainDb + fSpeedRangeDb` lands on `Mix:fVoiceFloorDb`, and that
    /// correspondence is the whole ending of a slide.** -16 and -32 make -48,
    /// which is the floor. A body slowing to a halt therefore fades out on its
    /// own measured speed and reaches silence exactly as it reaches the bottom
    /// of the ramp, so the stop has nothing left to cut and needs no envelope
    /// laid over it. Move either number and the grind is cut mid-ramp again.
    ///
    /// It was -12 before, which put the bottom at -32 dB against a -48 floor.
    /// That is the 16 dB step `fStopFadeMs` was covering, and it is why a slide
    /// sounded switched off rather than run down.
    float speedRangeDb{-32.0f};

    float pitchPerThousandUnits{0.15f};

    // -- the contact fraction ------------------------------------------------
    //
    // The measurement the mod never had, and the reason the grind used to fire
    // on almost nothing and then play at full body volume. Every body site
    // carries an anatomical mass (07 §6, `NominalMass`): a foot is about 1.5 %
    // of a body, a spine body about 11 %, a head about 7 %. Add up the ones that
    // are demonstrably rubbing and you have how much body is on the floor.
    //
    // One foot dragging is 1.5 %. Both feet and both shins is about 13 %. A body
    // flat on its back and skidding is 60 % and up. That is the quantity the
    // body loop's level should be a function of, and it costs a sum per tick.
    //
    // Two rules this measurement is held to. It shapes **the loop only** and may
    // never silence an impact - no inferred state gets to suppress a real
    // collision, and that rule earned itself the hard way. And where the body's
    // own sites cannot be resolved the fallback is the *quieter* answer: no
    // measurement means limb-only, never a guess at loud.

    /// Off, the body loop ignores how much of the body is touching and is a
    /// function of speed alone - which is what the mod did before this existed,
    /// and the fastest A/B for whether the fraction is what fixed it.
    bool fractionEnabled{true};

    /// Where the body grind begins to come in and where it is at full weight, as
    /// a fraction of total body mass in contact. Below the first it does not play
    /// at all; the limb loops are what carry the bottom of the range.
    float bodyFracStart{0.20f};
    float bodyFracFull{0.55f};

    /// How far under `fGainDb` the body loop sits at `fBodyFracStart`, the same
    /// shape as `fSpeedRangeDb`. Deep, because just over the threshold the grind
    /// should be barely there - crossing it must not be an event in itself.
    float bodyFracRangeDb{-15.0f};

    /// How long a limb goes on counting as "in contact" after its last graze.
    ///
    /// Contacts are dense when a fall is busy and absent when it is not, so a
    /// fraction computed from one tick's contacts alone flickers between 45 % and
    /// nothing at solver frequency. This is the hold that turns a stream of
    /// collisions into a state, and it is what the limb loops are held open on
    /// as well.
    float contactHoldMs{140.0f};

    /// Whether the body loop hangs on the bone nearest the contact rather than on
    /// the actor's root.
    ///
    /// The root is roughly the pelvis, and even for a genuine full-body slide
    /// that is not where the sound is. Nearest-to-contact rather than lowest,
    /// because down is not a reliable direction: a body can grind along a wall,
    /// down a staircase, or across a ceiling, and a rule that assumes the floor
    /// is below puts the sound in the wrong place every time one of those
    /// happens. Still one single point, so nothing smears - the right one.
    bool bodyFollowsContact{true};

    // -- the limb loops ------------------------------------------------------

    bool limbEnabled{true};

    /// How many limb loops may run at once, of the five chains that can have one
    /// - two arms, two legs and the head. Ranked by how hard each chain is
    /// rubbing, so the ones you would actually notice are the ones that get a
    /// voice. Zero is the same as switching the limb half off.
    ///
    /// A mix decision and not a budget one - nothing is capped against the voice
    /// count any more, and Skyrim will take as many as we ask it for. The default
    /// is under five because five limbs grinding at once is more sound than the
    /// picture supports, not because the fourth one costs anything.
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

    /// How hard a chain that already has a loop has to *stay* rubbing to keep
    /// it. Below `fLimbMinTangentSpeed`, so the pair is hysteresis: the budget
    /// is handed out on the entry test and taken away only on this one, which is
    /// what stops two crossing legs swapping a voice back and forth tick to tick.
    float limbHoldTangentSpeed{60.0f};

    /// Whether a limb loop hangs on the limb that is doing the rubbing.
    ///
    /// Off, it collapses onto the body like every loop used to. On is the
    /// design's own answer - a sound must come from where the limb is - and it
    /// matters most for the player, whose ragdoll is at arm's length and for whom
    /// a collapse to one point sounds like the audio is inside your head.
    bool limbFollowsLimb{true};

    /// How long a different bone inside the same chain has to be the one doing
    /// the rubbing before the loop moves onto it.
    ///
    /// The original worry about following the limb was legitimate: a scrape
    /// hopping between bones frame by frame smears instead of tracking. This is
    /// the cure, and it is applied to the hop rather than to the following.
    float limbHoldMs{200.0f};

    // -- the head ------------------------------------------------------------

    /// Whether the head chain's loop is tinted rather than being the plain limb
    /// grind.
    ///
    /// A skull dragging on stone is a small contact patch, so it is the limb file
    /// and not the body one - but it is a *skull*. The head impact earns its
    /// "that was a head" quality from a faint ring rather than from being a
    /// different recording, and the same trick applies here for a fraction of the
    /// asset cost: the same file, pitched down a little, with its own trim. If it
    /// turns out not to be enough, a proper head variant is a file drop and not a
    /// code change.
    bool headTint{true};
    float headPitchScale{0.90f};
    float headGainDb{-2.0f};

    // -- the body loop over the limb loops -----------------------------------

    /// Whether the body grind pulls the limb loops down under it.
    ///
    /// Both play at once by design, but a full-weight slide with four limb loops
    /// still grinding on top of it is more sound than the picture supports. The
    /// duck is scaled by the body loop's own weight, so it arrives with the body
    /// grind rather than switching on: at `fBodyFracStart` nothing is taken off,
    /// at `fBodyFracFull` the whole of `fLimbDuckDb` is.
    bool bodyDucksLimbs{true};

    /// How far down, at full body weight. Deep enough is suppression rather than
    /// damping - a limb loop ducked under the mix's own voice floor is stopped
    /// rather than played at silence - so this one slider covers both of what the
    /// two behaviours would have been.
    float limbDuckDb{-9.0f};

    // -- the entry catch -----------------------------------------------------
    //
    // One `scrape_grain` at the moment a grind *starts*: the scuff of the limb
    // arriving on the surface, under the head of the loop it introduces.
    //
    // **This is what the catch layer became, and the change is worth stating
    // because the slot brief still describes the old one.** It used to fire
    // through a whole slide - any graze harder than the slide's recent average
    // was a catch, rate-limited and rolled for - on the theory that a slide's
    // character is its irregularity and the loop had none. It is: the reference
    // recordings put sixty-five grit peaks a second on the rumble. But sixty-five
    // a second is *texture*, and texture belongs in the file at 65 Hz, not in the
    // cue list at 12 Hz where it arrives as a rattle of separate little impacts
    // over a grind - random bumps during a slide, which is not what a slide does.
    //
    // The moment that genuinely is an event is the *entry*: a slide is not
    // declared until 150 ms or 45 units into a grind, so the loop always opens
    // into a body that has been scraping for a moment already, and it opens with
    // nothing marking the arrival. That is the "somebody turned a noise on" the
    // slide rework is about, and one grain on the front of it is the fix - the
    // same shape as `imp_transient` in front of `imp_sub`.
    //
    // So the gates that only made sense for a stream are gone rather than
    // defaulted off: `fGrainCatchRatio` (harder than the slide's own average),
    // `fGrainMinGapMs` (a floor under the rate) and `fGrainProbability` (the roll
    // per contact) have no meaning for a once-per-grind event, and a slider that
    // cannot change anything is worse than no slider. Their keys are dropped
    // rather than `Renamed`, so an old ini's tuning of a different feature does
    // not carry into this one.
    //
    // Still never an inference. The grain rides the loop's own entry, and a loop
    // only enters on a chain that has really been grazing inside
    // `fContactHoldMs` - so there is a collision behind every one of these, which
    // is the rule the deleted settle system and the deleted synthesised slide
    // impact both broke.

    bool grainEnabled{true};

    /// How loud the entry scuff is against the grind it introduces.
    ///
    /// Against the *loop's* level and not against a contact's onset, which is
    /// the other half of repurposing this: a mid-slide catch was an accessory to
    /// the collision that caused it, and an entry is the front of the sound it
    /// opens. It scales with the grind, so a limb barely dragging scuffs quietly
    /// and a body arriving at speed scuffs hard, with nothing to tune twice.
    float grainGainDb{-6.0f};

    /// How much the pitch of an entry scuff is scattered, either way.
    ///
    /// Kept, and it still earns its place with one grain per grind: a body that
    /// grinds, launches and lands three times in a fall enters three times, and
    /// three entries at one pitch read as one sample repeating.
    float grainPitchScatter{0.18f};

    /// Whether the **body** grind gets an entry scuff as well as the limb grinds.
    ///
    /// Off by default, and that is a statement about the picture rather than
    /// caution. A limb arriving on a surface is a scuff - a foot catching, a hand
    /// slapping down and dragging - and that is a real moment with a real edge to
    /// it. A torso arriving flat is not a scuff, it is a *fall*, and the impact
    /// composite is already voicing it: a knockdown that ends in a skid has just
    /// put a full stack with a sub on it into the same 100 ms. A scuff on top of
    /// that is a fifth layer nobody asked for, on the one moment in the mod that
    /// is already loudest.
    ///
    /// On is for the entry the composite does *not* cover: a body that was
    /// already down, already still, and is then dragged. There is no collision
    /// there for the impact path to have voiced, so the grind opens out of
    /// silence - which is exactly the case the entry catch exists for.
    bool grainOnBody{false};

    // -- the level's own movement --------------------------------------------

    /// How much of the *contact* speed is blended into the level's own speed.
    ///
    /// Body speed is smooth by nature, and a level that follows it alone reads as
    /// a constant however correct it is. Contact tangent speed is genuinely
    /// spiky, because limbs load and unload as a body tumbles, and a little of it
    /// makes the grind breathe.
    ///
    /// A wobble *around* the body speed and never a replacement for it: the level
    /// used to be driven purely by contact speed and that was wrong - one limb's
    /// tangent is the speed of a limb, not of the body. 0 is the pure body speed.
    float contactSpeedBlend{0.25f};

    /// How much the level has to move before a running loop is told about it.
    ///
    /// A loop that re-cues every frame buries the cue list, so there is a
    /// deadband - but the shared one is three-quarters of a decibel, which is
    /// most of what a breathing grind actually does, and the scrape was being
    /// held flat by the thing meant to keep the log readable.
    float levelDeadbandDb{0.25f};

    // -- surfaces ------------------------------------------------------------

    /// Whether the loops pick a surface-coloured file.
    ///
    /// Off, everything plays the default grind - which is what the mod did
    /// before, on flagstone, floorboards, dirt, snow and ice alike. On, a slot
    /// with no recording behind it falls back to the default one anyway, so this
    /// costs nothing until the files exist.
    bool surfaceVariants{true};

    // -- the rumble bed ------------------------------------------------------
    //
    // The mass under the grind, as its own voice: `scrape_loop_rumble` held open
    // for the life of a slide, at the body grind's anchor, with its own level and
    // a pitch that deliberately does not move.
    //
    // **Why a layer and not a better grind file.** Measured against GTA 4's
    // slide events our grinds are 35-45 dB out on the bass-to-hiss balance and
    // in the opposite direction - theirs bass-led with the sub band loudest and
    // a hard rolloff over 8 kHz, ours broadband and flat to 20 kHz with the sub
    // 40 dB down. Grain rates match, so density was never the problem. And no EQ
    // rescues it: there is nothing under the shelf to boost, which is the same
    // lesson `crunch_gran` taught.
    //
    // **Why its own voice and not baked into the six grind files.** Baking it
    // costs the two things that matter. `MixLoop` takes one gain and one pitch
    // per voice and applies pitch as a resample of the whole source, so a
    // composite file forces the bed to pitch with the speed - and pitching bass
    // down at a crawl is flubby, blooms on a sub and vanishes on a laptop
    // speaker. It would also be six files to re-render every time the bed
    // changed, and during the body/limb crossfade two incoherent bass beds at
    // partial gain, which beats audibly down there. One voice at pitch 1.0 under
    // all of them is one asset, one mix, and mass that stays solid while the
    // grit crossfades over it.
    //
    // Two unsynchronised loop lengths is the free half: the bed runs longer than
    // the grinds, so the pair does not repeat on a common period and the looping
    // is harder to hear than either file alone.

    /// Off, the slide is the grinds alone - which is what the mod was before this
    /// existed, and the fastest A/B for whether the bed is what fixed it.
    bool rumbleEnabled{true};

    /// The bed's level at full weight and full speed.
    ///
    /// Over the body grind's own `fGainDb` rather than under it, which looks
    /// wrong and is the whole point: in the references the sub band is the
    /// *loudest* band of a slide and the grit rides on top of it. The same
    /// inversion `imp_sub` has against `imp_transient`, for the same reason -
    /// the layer that carries the mass is not the layer that carries the
    /// character.
    ///
    /// **-13 against the grind's -16 is measured, and it is measured against the
    /// files the bank actually plays.** The measurement below was taken with both
    /// layers at the top of their ramps, so it is a statement about the files and
    /// the balance rather than about the speed response, and it survives the ramp
    /// underneath it changing. Summed with the assigned `scrape_loop`
    /// and its own peak normalised out, the pair comes to tilt +14.1 with the sub
    /// band loudest and a centroid of 5444 Hz - inside GTA 4's +10 to +21 and
    /// inside its 4355-5714 Hz, on both axes, from a grind that measures -36.3
    /// tilt and 9102 Hz on its own. The assigned `scrape_limb` does the same
    /// thing: -44.8 alone, +19.6 and 5282 Hz with the bed under it.
    ///
    /// Which settles an argument that was open before this layer existed. Those
    /// two files measure as hiss and the library holds grinds that measure +17.6
    /// tilt, and the obvious move was to swap them. It is the wrong move: a grind
    /// that already carries its own low shelf and this bed on top of it comes to
    /// **+27** tilt and a 2431 Hz centroid - past the window on both axes, in the
    /// same direction, because the mass is now in the mix twice. To use a bass-led
    /// grind the bed has to come down to -24 or below, and there it contributes
    /// almost nothing and the centroid still lands short at 3544 Hz.
    ///
    /// So the grit files were never the problem. They are grit, they measure like
    /// grit, and they were only ever wrong because nothing was underneath them.
    /// **A bass-led grind wants a much lower bed, and the two decisions are one
    /// decision** - move this without listening to what the grinds are and the
    /// slide goes muddy rather than heavy.
    float rumbleGainDb{-13.0f};

    // The bed has no speed ramp of its own. It rides the limb grinds':
    // `fLimbSpeedForMinGain`, `fLimbSpeedForMaxGain` and `fLimbSpeedRangeDb`,
    // measured on `slideTangent`, which is `loop.tangent` at actor scope.
    //
    // `fRumbleSpeedRangeDb` and `fRumbleSpeedCurve` stood here and are gone. The
    // argument for them was physics and it is a good argument - friction noise
    // rises with transit rate, energy into the floor rises with the square of
    // speed, so a body at a crawl should have grit and no mass. In the mix it
    // produced a layer that was inaudible or fully on with nothing in between:
    // 30 dB of depth with a squared track under it, fed by the *body's* speed,
    // which is smooth by definition. A switch, not a ramp.
    //
    // Sharing the limb ramp is also why there is nothing to keep in sync. Two
    // knobs over one quantity is two things that drift apart by hand, and the
    // slide has been bitten by exactly that: the strategy used to carry its own
    // duration, distance and speed gates beside the motion axis' and the two
    // disagreed, so a knockdown could sit in `Slide` with no loop under it.
    //
    // The cost is real and worth saying: tuning the limb grinds' depth now moves
    // the bed's with it. If those two ever want to be different numbers, this is
    // the decision to reopen.

    /// A fixed pitch for the bed, and **the one ramp in this section that is
    /// deliberately not a ramp.**
    ///
    /// Floor and body resonance do not move with how fast the body is going -
    /// GTA 4's slide events hold a static spectrum and swell in level, which is
    /// the opposite of what a pitched bed does. Leave this at 1 unless a
    /// recording needs transposing to sit right, in which case it is a tuning of
    /// the *file* and not a response to anything.
    float rumblePitch{1.0f};

    /// ...and the ramp it is not, available anyway so the claim above can be
    /// tested by ear rather than believed.
    ///
    /// 0 is the design's answer. Wind it up and the bed slides with the grind;
    /// the flubbiness at the bottom of the speed range is the thing to listen
    /// for, and it arrives well before the movement reads as speed.
    float rumblePitchPerThousandUnits{0.0f};

    /// Whether a limb-only slide gets a bed at all, and how much less of one.
    ///
    /// A single dragging foot is a small contact patch, and a small contact patch
    /// still loads the floor - the difference between a foot and a torso is how
    /// hard, not whether. So the bed runs for both and the difference is a trim,
    /// which is also the cheapest possible answer: one file, one voice, one
    /// number between the two cases.
    ///
    /// Interpolated on the same contact fraction the body grind's own weight
    /// uses, so it arrives with the body rather than switching: the whole of
    /// `fRumbleLimbGainDb` at `fBodyFracStart` and none of it at `fBodyFracFull`.
    ///
    /// **With no body grind running at all the trim is applied whole**, whatever
    /// the contact fraction says, and that is the fix for a real bug rather than
    /// a nicety. There are three ways to have no body grind - `bBodyEnabled` off,
    /// the fraction under `fBodyFracStart`, the body slower than
    /// `fSpeedForMinGain` - and in the first of them a body lying flat and
    /// skidding still measured a fraction near 1. That cancelled this trim
    /// entirely and played the bed at full body level under nothing but limb
    /// grinds: mass with none of the grit, which is the one thing the bed must
    /// never be on its own.
    bool rumbleOnLimbs{true};
    float rumbleLimbGainDb{-9.0f};
};

/// The airborne anticipation rise, and nothing else any more.
///
/// This used to own a continuous cloth bed under every ragdoll as well. It was
/// muted in all thirty-eight saved configs - `bFoleyCloth = 0` in every one - so
/// it was never once heard on purpose, and it went with its slot rather than
/// staying as a layer nobody turns on. The name still fits what is left: an air
/// whoosh driven by how the body is moving is motion foley.
struct MotionFoleyConfig {
    bool enabled{true};

    /// The airborne anticipation rise. On by default at a low level.
    bool airborneRise{true};
    float airborneRiseGainDb{-24.0f};
};

/// The garment: a continuous fabric and armour layer riding a knockdown.
///
/// **Off by default, and off means byte-identical.** Nothing is measured, no
/// proposal is made and no random number is drawn while `bEnabled` is 0, so the
/// whole corpus replays exactly as it did before this section existed. Every
/// slider under it is set to a sensible voicing rather than to a neutral one, so
/// turning the switch on gives something worth listening to rather than a
/// starting point of silence.
///
/// **There was a cloth bed here before and nobody ever switched it on.**
/// `MotionFoley` owned it and `bFoleyCloth` was 0 in all thirty-eight saved
/// configs, so it went with its slot. Three things were wrong with it and all
/// three are structural rather than a matter of taste: its level was a function
/// of *body speed*, which is highest when the body is airborne (where cloth does
/// least) or sliding (where the grind drowns it) and lowest through the tumble,
/// which is the whole of what this is for; it had no armour class, so it was
/// wrong for three quarters of the population by construction; and nobody had
/// framed what it was filling. The mod discards about nine contacts in ten - the
/// design's own 10:1 reduction - and the body goes on visibly thrashing through
/// every one of the gaps. A layer pinned to body speed does not fill that,
/// because body speed cannot tell a limp drift from a cartwheel.
///
/// If this one ends up at 0 in every saved config too, delete it.
struct RustleConfig {
    bool enabled{false};

    // ── what counts as movement ──────────────────────────────────────────────
    //
    // Two independent signals, each with its own ramp in its own units, and a
    // weight to blend them. Two ramps rather than one shared one because the
    // quantities are not commensurable: a thrash is an acceleration in u/s^2 and
    // a tumble is a surface speed in u/s, and adding them before normalising
    // would hide a unit conversion inside a weight where nobody could read it.

    /// **Thrash** - how hard the limbs are accelerating *relative to the body*,
    /// fabric-weighted across the skeleton, u/s^2.
    ///
    /// The signal, and the reason this is not driven off the centre of mass. The
    /// COM of a body bouncing down a staircase traces a fairly smooth arc
    /// between treads; what is violent is the limbs, flung about the body as it
    /// turns over. A COM-driven layer would be nearly flat through the most
    /// chaotic part of a fall.
    float thrashWeight{1.0f};
    /// The ramp, in u/s^2. Below the floor there is no rustle at all, which is
    /// what keeps a body lying still silent - the most important number here.
    ///
    /// **Measured, on the thirteen takes in `Research/NewRecordings`.** The
    /// fabric-weighted mean relative limb acceleration, per tick:
    ///
    /// | take length | mean | peak |
    /// |---|---|---|
    /// | long (700-7000 ticks, mostly settled) | 583-793 | 6300-18300 |
    /// | short cuts (78-170 ticks, mostly action) | 945-2565 | 6000-18300 |
    ///
    /// So an active tumble sits around 1000-2500 and an impact frame reaches
    /// 6000-18000. The first pass of these defaults guessed 140/1100 and was
    /// badly wrong in the direction that matters: at a top of 1100 every tick of
    /// every take saturated, and the layer would have been a constant drone
    /// rather than something that breathes.
    ///
    /// 500 to 4000 puts the settled stretches under the floor, an ordinary
    /// tumble around a third of the way up, and every real impact at the top -
    /// which, with the release below, is what produces the tail after a hit.
    ///
    /// `EngineStats::rustleThrashMean` and `rustleThrashPeak` are printed per
    /// take by `rds-verify` whenever the feature is on, so this is re-measurable
    /// in one command rather than being a number to take on trust.
    float thrashFloor{500.0f};
    float thrashFull{4000.0f};

    /// **Tumble** - how fast the limbs are rotating, as surface speed
    /// (angularSpeed x radius), fabric-weighted, u/s.
    ///
    /// The term a staircase is actually made of, and the one that cannot be
    /// folded into the thrash. A limb rotating at a *constant* rate drags its
    /// sleeve across itself continuously with no acceleration involved at all,
    /// so a steady cartwheel has a large tumble and a small thrash; a body
    /// slamming from one pose to another has the reverse. Both are rustle and
    /// neither implies the other.
    ///
    /// **These three are still guesses, and there is a reason they had to
    /// stay guesses.** `angularSpeed` is published on every live limb sample and
    /// always has been, but the `_pose.bin` sidecar never stored it - so the
    /// term measured exactly 0 on all thirteen takes while being live in the
    /// game, which is the seam rule's own failure case: the testbench cannot
    /// reproduce what the game does. The sidecar is v2 now and carries it, and
    /// v1 files still load with it at zero. **The corpus has to be re-captured
    /// before this can be tuned offline**, and until then the honest reading of
    /// a rustle in the testbench is "the thrash half only".
    float tumbleWeight{0.8f};
    float tumbleFloor{40.0f};
    float tumbleFull{400.0f};

    /// Per-limb clamp on the thrash term, u/s^2, applied *before* the sum.
    ///
    /// Not optional, and the position matters. Pose has no equivalent of the
    /// blow-up rejection ingest does on the arithmetic, so a limb the solver
    /// teleports produces an acceleration of arbitrary size and the layer
    /// screams. Clamping per limb rather than after the sum also fixes the
    /// ordinary case: one limb striking stone gets held to something sane while
    /// the other seventeen contribute their real motion, so an impact *lifts*
    /// the drive instead of saturating it and the ramp above only has to span
    /// the range a tumble actually occupies.
    float thrashCeiling{10000.0f};

    /// Whether the body's own acceleration is subtracted from each limb's.
    ///
    /// On is right: what makes fabric move is motion *relative* to the body it
    /// is on, and everything on a falling body accelerates at g together - so
    /// the subtraction is what correctly quietens a free fall. Off is the naive
    /// version, kept as a one-key A/B because a switch this load-bearing should
    /// be falsifiable.
    bool relativeToBody{true};

    /// How much a fast body multiplies the two terms above. A multiplier and
    /// never an addend: the same thrash at 600 u/s displaces more cloth further,
    /// but a body drifting fast and limply must not rustle at all. 0 is pure
    /// thrash and tumble, which is the baseline every A/B should start from.
    float speedWeight{0.30f};
    float speedForFull{600.0f};

    /// The airborne term, and the only addend. **Off by default.**
    ///
    /// A cloak genuinely does flap in a long drop, and the relative measurement
    /// above correctly reads nothing there. But `air_whoosh` already fires on
    /// exactly this state, and two voices saying "this body is falling" is one
    /// too many. Present so the comparison can be made; if the fabric version
    /// wins, the answer is to retire the whoosh rather than to layer under it.
    float airWeight{0.0f};
    float airSpeedForFull{500.0f};

    /// Below this drive the loop is stopped rather than held at silence.
    ///
    /// There is no separate hold time, because the release below already is one:
    /// an exponential decay never reaches zero, so what ends a rustle is this
    /// threshold and how long `fReleaseMs` takes to fall through it. A second
    /// timer would be the same delay expressed twice.
    float silenceDrive{0.03f};

    // ── how it moves ─────────────────────────────────────────────────────────

    /// Asymmetric on purpose, and the asymmetry is the whole voicing.
    ///
    /// Fast attack because cloth responds immediately and a slow one puts the
    /// rustle behind the impact that caused it. Slow release because that is
    /// what a garment does - and it buys two things for free. It fills the gaps
    /// between bounces, so a tumble reads as continuous rather than as a rattle;
    /// and it puts a decaying fabric tail behind every impact, which is the
    /// sound of clothing settling after a hit, arrived at without inferring
    /// anything. No "the fall is over" state, no synthesised contact - just the
    /// envelope of a measured quantity coming down.
    float attackMs{30.0f};
    float releaseMs{280.0f};

    /// The loop's pitch across the drive range. Deliberately narrow: a garment
    /// does not change pitch much, and a wide range here is the fastest way to
    /// make a sustained layer sound synthetic.
    float pitchAtFloor{0.94f};
    float pitchAtFull{1.07f};

    /// A slow wobble on the level, so a two-second file does not read as a
    /// two-second file.
    ///
    /// A deterministic sine and never a random walk: the engine must stay
    /// reproducible given a seed, and anything drawing a number per tick
    /// re-rolls every variant and scatter downstream of it. Phased off the
    /// actor's own start time, so two bodies falling together do not breathe in
    /// unison.
    float wanderDepthDb{1.5f};
    float wanderHz{0.23f};

    /// How far the level moves before a running loop is re-cued. The scrape's
    /// value, and for the scrape's reason: without a deadband a loop emits an
    /// update every frame and buries the cue list under no-ops.
    float levelDeadbandDb{0.25f};

    /// Longer than the scrape's at both ends. Fabric has no transient, and a
    /// rustle that snaps in is the most obvious thing in the mix.
    float startFadeMs{90.0f};
    float stopFadeMs{240.0f};

    // ── how loud ─────────────────────────────────────────────────────────────

    /// The layer's level at full drive. It is a bed: the old cloth layer
    /// measured 30-36 dB under a hero hit and this belongs at the quiet end of
    /// that, felt more than heard.
    float gainDb{-26.0f};

    /// How far under `fGainDb` the layer sits at the bottom of the drive ramp.
    /// Deep, so crossing the floor is not an event in itself - the same shape as
    /// `ScrapeLoop:fSpeedRangeDb`.
    float driveRangeDb{-20.0f};

    /// Per-class voicing, on top of whichever conditional variant the class
    /// selected. Unlike the armour skins these do not ship at 0: with one slot
    /// carrying all four classes there is no empty slot to make a naked body
    /// silent, so the bare trim is what does that job.
    float bareTrimDb{-9.0f};
    float clothTrimDb{0.0f};
    float lightTrimDb{1.0f};
    float heavyTrimDb{2.0f};

    /// Your own ragdoll is at arm's length and its layers follow bones, so the
    /// garment is closer to the ears than any NPC's will ever be.
    float playerTrimDb{-3.0f};

    /// How far a running body grind pulls the rustle down, at full body weight,
    /// scaled by the grind's own weight so it arrives with the slide rather than
    /// switching on - exactly as `bBodyDucksLimbs` scales `fLimbDuckDb`.
    ///
    /// A slide already has a body loop, up to three limb loops and a grain layer
    /// describing the same motion. A fabric bed under all of that is mud, and it
    /// is precisely the mistake the old cloth bed made. Deep enough to be
    /// suppression rather than damping, so one slider covers both.
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

/// Everything that decides how the floor colours a sound, in one place.
///
/// A surface skin is not a variant of an impact - it is an extra layer stacked
/// on one, and that distinction is why this section can exist at all. Colour is
/// additive, so six surfaces cost six files; a surface *axis* would multiply
/// every layer in the composite by six instead. It is also the more honest
/// model: landing on boards does not replace the sound of a body arriving, it
/// adds a hollow knock to it. A missing skin is then a body landing with no
/// floor named, which is a quieter mod rather than a wrong one.
///
/// These controls used to be spread across four sections - the composite owned
/// the offset and the ramp, `Mix` owned the role trim, `SlotGain` owned the
/// three per-file trims and `Layers` owned the three mutes - so answering "is
/// the floor too loud" meant four panels. Every row moved here carries where it
/// came from, so an ini written before the move still loads.
/// One surface's own settings.
///
/// Thirteen of these live in `SurfaceConfig`, one per `SurfaceClass`, and only
/// the ones you have *opened* are written to the ini. A closed block is not
/// empty and is never read as zeroes - `SurfaceConfig::Resolve` fills it from
/// the nearest opened ancestor, or from the section defaults at the top of the
/// chain. That is what makes opening one free: the values it starts with are
/// already the values it was playing, so a `+` changes nothing until you move a
/// slider.
///
/// Which fields are here and which stayed global is the one design decision in
/// this struct. A field is here if it is a property of the *material* - when
/// glass arrives, how steeply it ramps, whether water is allowed to colour every
/// scuff. A field stayed in `SurfaceConfig` if it is a property of the *system* -
/// the master switch, the role trim, the tap's envelope and its rank clamp.
struct SurfaceSkinConfig {
    /// Muted at render like every other mute, so muting one leaves every
    /// arbitration decision identical and silences only what came out.
    bool enabled{true};

    /// This skin alone, summed on top of `SurfaceConfig::trimDb`. Thirteen
    /// separately recorded sounds arrive at thirteen different levels, and
    /// pulling the role trim to fix stone takes every other floor with it.
    float trimDb{0.0f};

    /// When the colour arrives, relative to the contact frame. Close to the
    /// transient, or it stops reading as the same event - but not identical
    /// across materials: a hollow knock has a resonant delay that glass does
    /// not, and a splash lags both.
    float offsetMs{8.0f};

    /// The ramp, interpolated by intensity like every other layer: a brush of
    /// the floor barely names it, a body dropped on it names it clearly. This is
    /// the widest genuine difference between materials - glass is nearly silent
    /// at a brush and a shatter at speed, where carpet is flat across the whole
    /// range. One global ramp could not say that.
    float gainAtMinDb{-12.0f};
    float gainAtMaxDb{-6.0f};

    /// Colour the burst filler with this material too.
    ///
    /// Per-surface rather than global because water is the counter-example that
    /// forced it: nine of every ten contacts are taps, and a splash on every one
    /// of them is absurd, where wood and gravel want tap colour badly.
    bool onTaps{true};

    /// The tap's ramp for this material. The tap's *offset* and its headroom
    /// clamp stay global - they describe the 40-100 ms grain being coloured
    /// rather than the floor doing the colouring.
    float tapGainAtMinDb{-12.0f};
    float tapGainAtMaxDb{-8.0f};
};

struct SurfaceConfig {
    /// Off, nothing gets a surface skin and the mod plays the same body on
    /// marble as on moss. The fastest A/B for how much of the mix is surface
    /// identity at all; the per-class mutes do the same one surface at a time.
    bool enabled{true};

    // -- the chain root -------------------------------------------------------
    //
    // These six are what a class inherits when neither it nor any ancestor has
    // a block of its own, which on a fresh install is all thirteen of them. They
    // keep the ini keys they have always had, so a file written before the
    // surfaces list existed loads unchanged and every class inherits exactly
    // what it inherited before.

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
    // A tap was a single grain, and therefore the one cue in the mod that could
    // not say what it hit. That is a gap rather than a missing luxury: nine of
    // every ten contacts are taps, so a body sliding down a wooden staircase
    // spent nine tenths of itself sounding like a fall down nothing in
    // particular. Scuffs are where a floor gets identified - the hero hits are
    // where it gets confirmed.

    /// Colour the burst filler too, not only the four-layer composite.
    bool onTaps{true};

    /// The tap's own offset and ramp, both tighter and lower than the
    /// composite's. A tap is 40-100 ms of grain, so a skin arriving 8 ms in and
    /// running 200 ms would outlive what it is colouring and read as a second
    /// cue rather than as part of the first.
    float tapOffsetMs{4.0f};
    ///
    /// The ramp is measured rather than guessed. At -20/-14 the colour landed
    /// under `Mix:fVoiceFloorDb` on nearly every tap that survived to render,
    /// so the layer existed and was never heard: a tap is already the quietest
    /// cue in the mod and the floor sits a few decibels under it, which leaves
    /// no room underneath for something 15 dB down. A tap's colour rides close
    /// or it does not ride at all - and the headroom below is what stops
    /// "close" turning into "level with".
    float tapGainAtMinDb{-12.0f};
    float tapGainAtMaxDb{-8.0f};

    /// How far under its own tap the colour is held, whatever the ramp says.
    ///
    /// This is "colour, not dominate" written down as a number. A proposal's
    /// level is the loudest of its layers, and level before arbitration is also
    /// *rank* - so a skin allowed to come out over the grain it is colouring
    /// would put a scuff on the floorboards ahead of a real impact in the sort.
    /// Negative, always: 0 lets the colour tie with the grain and positive lets
    /// it win. The tap's rank is taken from the tap alone regardless, so this
    /// clamp is about what is heard, not about what is chosen.
    float tapHeadroomDb{-3.0f};

    // -- level, after the cue has been chosen ---------------------------------

    /// The role trim - every skin together. Up makes the floor material
    /// obvious; down makes every surface sound the same, which is what vanilla
    /// does. Was `Mix:fSurfaceTrimDb`.
    float trimDb{0.0f};

    // -- the list -------------------------------------------------------------

    /// Which classes have a block of their own in the surfaces ini.
    ///
    /// This is the whole of the `+` in the panel: opening a surface sets its
    /// bit, and nothing else happens, because `Resolve` has already left the
    /// block holding the values it was inheriting. Closing it clears the bit and
    /// re-resolves, and the ini section disappears on the next save.
    bool opened[static_cast<std::size_t>(SurfaceClass::kCount)]{};

    /// Every class's *effective* settings, inherited or owned.
    ///
    /// Always current: `Resolve` is run after every load and after every edit,
    /// so the engine indexes this directly and never walks a chain on the audio
    /// path. A closed entry is a copy of its nearest opened ancestor rather than
    /// a default, which is why reading one without checking `opened` is correct.
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
    /// `enabled` and `trimDb` at their neutral values rather than at a global,
    /// because those two have no global to take - `SurfaceConfig::enabled` is
    /// the master switch and `trimDb` is the role trim that sums on top.
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

    /// Push the section defaults down through the parent chain into every class
    /// that has no block of its own.
    ///
    /// Run after every load and after every edit. Cheap - thirteen classes and
    /// chains at most three deep - and idempotent, so calling it twice costs
    /// nothing and forgetting where the last call was is not a bug.
    void Resolve() {
        const SurfaceSkinConfig root = RootSkin();
        for (std::size_t i = 0; i < kClasses; ++i) {
            if (opened[i]) {
                continue;
            }
            // The nearest ancestor that owns its settings, or the root. Walking
            // per class rather than in dependency order means this does not care
            // what order the classes are declared in.
            const SurfaceSkinConfig* from = &root;
            for (SurfaceClass p = SurfaceParent(static_cast<SurfaceClass>(i));
                 p != SurfaceClass::kCount; p = SurfaceParent(p)) {
                if (opened[static_cast<std::size_t>(p)]) {
                    from = &skins[static_cast<std::size_t>(p)];
                    break;
                }
            }
            // `enabled` and `trimDb` come along: an opened parent that has been
            // muted or trimmed should take its unopened children with it, which
            // is the whole reason muting `stone` is worth doing at all now that
            // ice and glass hang off it.
            skins[i] = *from;
        }
    }
};

// -- Armour: the second colour axis -------------------------------------------

/// What the body was wearing, as a layer rather than as a timbre shift.
///
/// The same shape as `SurfaceConfig` and for the same reason. Colour is
/// additive, so four armour classes cost four files - where an armour *axis*, a
/// heavy variant of every layer, would cost 23 slots x 4 classes x 2 variants
/// and would not sound better. The thing that changes when you put plate on is
/// not the body's mass; it is that something metallic moved. That is a layer.
///
/// Nothing here does anything until `armor_*` files exist. All four slots ship
/// with `expectedVariants = 0`, so with no assets installed every cue is
/// identical to one from a build without this section - which is the property
/// the whole feature was asked to have, and the one Phase 1's acceptance test
/// checks by diffing cue lists over the corpus.
struct ArmorConfig {
    /// Off, no cue gets an armour skin and the class is not even read. The
    /// fastest A/B for how much of the mix is armour identity at all; the four
    /// mutes at the bottom do the same one class at a time.
    bool enabled{true};

    // -- how the class is decided ---------------------------------------------
    //
    // The mapping from a body to a class is a judgement call, and these are the
    // three places it can be overruled.

    /// Per contact limb rather than per actor. On - the default - heavy boots
    /// and nothing else means feet get the plate rattle and the rest get the
    /// bare skin, with no new mechanism: `contact.coverage` is already the class
    /// of the limb that hit. Off, every contact reads the actor's body-slot
    /// class instead, so a heavy cuirass makes the whole actor clank.
    bool perLimb{true};

    /// A site with nothing in its own biped slot takes the cuirass's class
    /// rather than reporting bare.
    ///
    /// Right for the common case - a cuirass with no separate gauntlets does not
    /// mean bare forearms - and wrong for heavy boots on an otherwise naked
    /// body, where you want the rest to read naked. Off, an empty slot means
    /// bare. This is `CoverageForSite`'s hardcoded behaviour made switchable.
    bool inheritFromBody{true};

    /// A worn ARMO with no name and no weight is bare, not clothing.
    ///
    /// TNG's skin is a real TESObjectARMO occupying five slots, so without this
    /// a naked modded body reads as *clothed on five sites* and `armor_bare`
    /// essentially never fires. The recording loader has always known this - it
    /// is the data dictionary's own rule, in `CoverageFrom` - and the live path
    /// did not until this switch was added.
    bool bareIsNaked{true};

    /// Where the airborne rise gets a class from, having no contact and
    /// therefore no limb. 0 the actor's body slot - a cuirass is what you hear
    /// moving - 1 the last contact's limb, 2 off.
    int actorClassSource{0};

    // -- the head accent ------------------------------------------------------
    //
    // Deliberately the only armour rule anywhere near the head. No armour skin
    // is layered on `head_impact` and no armour condition is threaded through
    // `HeadImpactStrategy`; these two are read once, at the top, before any
    // classification runs.
    //
    // What they take away is the *accent* - the dull skull thud with the ring on
    // it, which is what reads as bare skull and is exactly wrong under a helm.
    // The composite still fires, the skins still fire, and damage still fires
    // through DamageStrategy's own door. A head in plate landing hard is still
    // loud; it just stops sounding like a melon.

    bool noHeadOnLight{false};
    bool noHeadOnHeavy{false};

    // -- on the composite -----------------------------------------------------

    /// When the rattle arrives. Between the transient and the body on purpose:
    /// metal moving because something stopped is a consequence of the contact,
    /// not the contact itself, so it lands after the strike and before the mass.
    /// At 0 it fuses with the transient into one brighter click, which is the
    /// failure mode to listen for.
    float offsetMs{12.0f};

    /// The ramp, interpolated by intensity like every other layer.
    float gainAtMinDb{-14.0f};
    float gainAtMaxDb{-7.0f};

    /// How far under the stack the skin is held, whatever the ramp says.
    ///
    /// Negative, always. The armour skin never enters `levelDb` - it passes
    /// `ranks = false` like the surface skin beside it - so this is about what is
    /// heard rather than about what is chosen. It matters more here than there:
    /// a plate rattle is the layer most likely to be long and loud relative to
    /// what it is colouring, because armour keeps moving after the body has
    /// stopped, and that is exactly the layer you do not want deciding which
    /// contact wins a hero slot.
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
    /// flagstone is one of the few places the feature pays for itself with a
    /// single file.
    bool onSlide{true};
    float slideGainDb{-10.0f};

    // -- the free levers ------------------------------------------------------
    //
    // Both cost no files at all. Pitch is continuous and free in this engine and
    // the design already leans on it - 00 SS7, "it beats doubling the bank".
    //
    // All eight ship at 0 so that an install with no armour assets is *exactly*
    // today's mod, which is the promise the feature was built on. The voicing
    // worth trying first is heavy -1.0 and bare +0.5: it makes an armoured body
    // read heavier and a naked one lighter with no assets installed at all, and
    // it is the cheapest dynamic in the whole feature.

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
    /// With vanilla's body impacts suppressed we are not sitting next to them
    /// any more, so the level calibrates against footsteps and combat instead -
    /// which is why this is a config value and not a mixing decision. There is
    /// no bus control in this engine; loudness relative to combat is achieved
    /// purely by our own gain.
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
/// layers in the same proportions today, and they should not be: the mass layer
/// carries a body, the sub is a body's boom, and out on a stick there is less of
/// both and proportionally more of whatever the floor said.
///
/// Every field is 0, so the block is inert until somebody tunes it. That is not
/// modesty - it is that nothing here is measurable from a recording. Which
/// contact was *loud* is physics and the corpus answers it; how much sub a
/// forearm should have is taste, and the only instrument for it is ears
/// (00-Design §3, and `logs do not define correct behaviour`).
struct LayerBalanceConfig {
    float transientTrimDb{0.0f};
    float bodyTrimDb{0.0f};
    float subTrimDb{0.0f};
    /// The surface skin - what it *hit*, coloured by which part hit it. Out on a
    /// hand the floor is most of what there is to hear, so this is the one of the
    /// four most likely to want to go up rather than down.
    float surfaceTrimDb{0.0f};
};

/// Per-part trims on the composite's four layers, at the Trim stage.
///
/// ── Why this is a Trim and what that buys ────────────────────────────────────
///
/// It lands in `LayerTrimDb`, after arbitration, so it cannot change what was
/// chosen: the sort, the rate cap, the chain merge and the masking drop have all
/// happened and read `Proposal::levelDb`, which this never touches. That is what
/// makes it safe to turn while listening (01 §5, `config.md` rule 4).
///
/// ── Why the range is not symmetric ───────────────────────────────────────────
///
/// A **cut** is safe in the strong sense: the arbitrator decided this cue was
/// audible against a level that turns out to be higher than what renders, so the
/// mix ends up quieter than predicted and nothing was admitted that should not
/// have been.
///
/// A **boost** is safe in the weaker sense of not reordering anything, and it
/// widens a gap `config.md` already names as the thing `Proposal::priorityDb` is
/// waiting on: masking and `maskCeilingDb` read the pre-trim number. Boost a
/// layer 10 dB and the cue that survives sits 10 dB over the ceiling that judged
/// it, while the cue masking dropped for being 12 dB under stays dropped on a
/// level neither of them will render at. The class compressor has the same
/// blind spot - `compressCutDb` is computed from the proposal's level before
/// this is added, so a boosted layer escapes the squeeze that was meant to hold
/// its class down.
///
/// Hence -18 to +6 in the schema. Enough cut to take a layer out of the picture,
/// enough boost to bring a skin up over a body it sits 4 dB under, and not
/// enough of either to make the arbitrator's arithmetic a fiction.
///
/// ── The one way a Trim here can still change what plays ──────────────────────
///
/// `Emit` drops a layer under `Mix:fVoiceFloorDb`, and a proposal whose every
/// layer falls under it emits nothing - which rolls the whole provisional commit
/// back (01 §6): the burst budget is refunded, the masking ceiling is not raised
/// and the moment is not counted. So a deep enough cut does not merely quieten a
/// stack, it can delete the event and hand its budget to whatever comes next.
/// That is the rollback working as designed rather than a bug, and it is the
/// reason the cut is bounded at all rather than left open.
///
/// Measured on `Proventus_Avenicci_devbench_5`, all four limb trims at -18:
///
/// ```
///   default   proposed 348 | rate 115 chain 5 mask 12 burst 54 | emitted 185
///   -18 all   proposed 348 | rate  98 chain 4 mask 15 burst 57 | emitted 135
/// ```
///
/// `proposed` does not move - nothing before Stage 5 saw the trim - but every
/// budget downstream of a rolled-back stack does. A boost does not do this: the
/// same take with the spine's sub at +6 and its body at +3 reports every counter
/// unchanged and the same 185, because a layer already over the floor cannot
/// cross it going up. **Cuts can restructure and boosts cannot; boosts drift the
/// arbitrator's prediction and cuts cannot.** Two different hazards, one at each
/// end of the range, which is why the range is bounded at both.
///
/// ── What is deliberately not here ────────────────────────────────────────────
///
/// The crunches, the gore and the head accent are per-part already, one layer up,
/// where they can stop a cue being *proposed* instead of muting one the
/// arbitrator has already made room for: `Damage:b<Part>Enabled` with a threshold
/// and a budget each, and `HeadImpact`'s own gain and trim. A second per-part
/// control over those would be two ways to say one thing, which is the failure
/// the three crunches' shared mute is written down to avoid.
///
/// The armour skin is not here either. It is a different axis - what the body was
/// *wearing*, not which part it is - it has `Armor:f<Class>TrimDb` for its own
/// level, and `ClampArmorSkin` already holds it under the stack it colours.
struct CompositeBalanceConfig {
    /// One switch over all twelve, so an A/B is one key rather than a dozen.
    /// With every trim at 0 the feature is already inert, so this is for turning
    /// a *tuned* balance off in one gesture and hearing what it was doing.
    bool enabled{true};

    /// The skull. It plays the torso's mass layer - see `BodySlot` - so this is
    /// the only place the composite under a faceplant can be shaped apart from
    /// the one under a back-slam.
    LayerBalanceConfig head{};
    /// The neck and the torso: the column, and the part with a body behind it.
    LayerBalanceConfig spine{};
    /// Arms, legs, and anything off a skeleton we could not name - which lands
    /// here rather than on the skull's tuning, exactly as it does in `Damage`.
    LayerBalanceConfig limb{};
};

// -- Compression: holding the top of one class down ---------------------------

/// How much of its own range each kind of moment is allowed to use, *on the
/// mod's own scale*.
///
/// This is a compressor and deliberately not a ceiling. A hard cap is the
/// obvious thing to reach for - "no tap over -20 dB" - and it has one failure
/// mode that is easy to walk into and hard to un-hear: everything above the cap
/// arrives at exactly the cap, so a threshold set a few dB too low turns a dozen
/// distinct impacts into a dozen identical ones. Measured on log_2 with a hard
/// cap at -20: four separate impacts landed within 1 dB of each other where
/// there had been an 11 dB spread. The tumble stops having a shape.
///
/// So above `threshold` the range is squeezed rather than flattened. A ratio of
/// 4 means four decibels of input become one of output: the loud ones are still
/// ordered, still distinguishable, just closer together. `ratio = 1` is off and
/// a large ratio approaches the hard cap, so the whole span between "leave it
/// alone" and "clamp it" is one dial.
///
/// **What is not here, because it already exists.** Compressing the *whole*
/// range rather than its top is `Intensity:fDynamicRangeDb`,
/// `PostIntensity:fExtraRangeDb` and the two `fSoftClipKnee`s. Those are global
/// and they move every cue, including the quiet ones that were already right.
/// The gap this fills is per-class and top-only: "taps and head cracks get too
/// loud, everything else is fine".
///
/// ── The unit ────────────────────────────────────────────────────────────────
///
/// Thresholds are measured against `Proposal::levelDb` - the number arbitration
/// itself sorted on, before a single trim is applied - and not against the
/// rendered level. A threshold written as a rendered level, or as a 0..1
/// amplitude, would be an *absolute* one, and an absolute threshold fights every
/// volume control downstream of it: turn `Mix:fMasterGainDb` up 6 dB to sit
/// against combat properly and the loudest hits do not move, so the mod gets
/// louder everywhere except at the top and the compression silently tightens by
/// 6 dB. A 0..1 amplitude would also be a fiction - the game applies its own
/// distance falloff and output model after us, so there is no full scale for it
/// to be a fraction of.
///
/// `levelDb`'s scale has a real zero: `onsetGainDb` tops out at exactly 0 dB for
/// the hardest contact the engine can hear, and every layer endpoint sits at or
/// under it. So
///
///     fTapDb = -20  means  "start holding a tap once it comes within 20 dB of
///                           the loudest thing this mod can produce"
///
/// which is a statement about the mix's internal balance and nothing else. Every
/// trim then applies on top of the compressed value - master, player master, the
/// phase trims, the slot and role trims, the post-intensity shaping - so turning
/// the mod up turns its compression up with it and the setting keeps meaning the
/// same thing at every volume.
///
/// Because the input is bounded at 0 dB, a finite ratio still gives an exact
/// worst case, which is the number to reach for when the question is "how loud
/// can this get":
///
///     loudest possible = threshold + (0 - threshold) / ratio
///
/// Two consequences of measuring pre-trim, both deliberate:
///
///  - A slot trim is *not* compressed. `SlotGain:fHeadImpact = -10` for a hot
///    wav still lands on top, so a head accent held to -6 renders at -16. The
///    compressor is about the event; the trims are about the mix.
///  - Arbitration has already run on the uncompressed level, so this can never
///    change which contact wins the rate cap. A held hero hit is still the hero
///    hit of its frame (config.md, rule 2).
struct CompressConfig {
    /// Off by default and behind a flag rather than parked at values nothing
    /// reaches. Every threshold below is 0, which *is* the top of the range and
    /// therefore already a no-op - but leaving it implicit would make this a
    /// section that looks inert and is not.
    bool enabled{false};

    /// Decibels in per decibel out, above a class's threshold. 1 is off; 4 is an
    /// ordinary musical squeeze; 20 is near enough a hard limiter to be one.
    ///
    /// One ratio for every class rather than nine, because it is the *character*
    /// of the holding - gentle or firm - and wanting that to differ between a
    /// tap and a crunch is a much rarer thing than wanting the thresholds to.
    /// The threshold is where a class starts being held; this is how hard.
    ///
    /// Defaults to 4 rather than to 1 so that lowering a threshold does
    /// something the first time. A ratio of 1 with a threshold moved is a
    /// section that is switched on, configured, and silent about why nothing
    /// changed.
    float ratio{4.0f};

    // ── the impact composite, band by band ───────────────────────────────────
    //
    // Four lines rather than one, and the cut is taken **per layer against that
    // layer's own level**. The four are the frequency ranges of a single hit and
    // they are not near each other: measured over one 6.6 s window of
    // `Proventus_Avenicci_devbench_7`, the loudest each reached was
    //
    //     imp_body       -9.2 dB      <- owns the composite's peak
    //     imp_sub       -11.7 dB
    //     imp_body_limb -15.5 dB
    //     imp_transient -17.4 dB
    //     surf_wood     -21.7 dB
    //
    // so one threshold over the stack can only hold the transient to pay for the
    // body's peak. Holding the body and the sub at lines of their own is what
    // frees the headroom `Mix:fMasterGainDb` then has somewhere to spend - which
    // is the only way to get louder without arriving at `MixParams::clipCeiling`.
    //
    // **This does reshape the stack as level rises, and that is deliberate.** The
    // one-cut-per-stack rule this replaced was written to protect the layer
    // balance, and it is the right rule for a compressor whose bands are not
    // bands. These are: transient, body and sub are a split by frequency, so
    // holding them independently is multiband compression and reshaping is what
    // multiband compression is for. A hit that would have gone dull all over now
    // loses the part that was actually too big.
    //
    // Every one defaults to 0 - the top of the range - so a config that has
    // never touched them is byte-identical to one from before the split.

    /// The click. The layer that says *when*, and the one least worth holding:
    /// it is 8 dB under the body already and holding it is how an impact stops
    /// reading as a strike.
    float transientDb{0.0f};

    /// The mass. **The line to reach for first** - it owns the composite's peak
    /// at every intensity, so the headroom is here or it is nowhere.
    float bodyDb{0.0f};

    /// The pitched sub at +65 ms. The longest layer and the lowest, so it holds
    /// the peak longest even where it does not set it, and the layer whose
    /// excursion a soft clip mangles most audibly.
    float bassDb{0.0f};

    /// The same mass out on an arm or a leg. Its own line rather than the body's
    /// because it is drier and 6 dB quieter, so a threshold that holds the torso
    /// correctly never reaches it.
    ///
    /// Read off the slot the *proposal* named, so this works before anybody has
    /// recorded an `imp_body_limb` - exactly as `SlotGain:fImpBodyLimb` trims the
    /// `imp_body` file the limb composite falls back to.
    float bodyLimbDb{0.0f};

    /// Every other layer of a composite: the surface skin and the armour skin.
    /// Both ride the body ~12 dB under it and neither is ever the layer that
    /// clips, so they share the catch-all the section shipped with rather than
    /// each earning a line nobody would move.
    float impactDb{0.0f};

    /// The burst filler. The nine of every ten contacts that sit under
    /// `tapBelowIntensity`, and the line most worth pulling down: taps are what
    /// a tumble is mostly made of, so a tap that can reach the top of the range
    /// is a tumble with no shape to it.
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
/// Free and inline rather than a member of the engine, because three places need
/// the same arithmetic: the engine applies it, the testbench explains it on a cue
/// that already carries it, and the timeline draws the height a held cue would
/// have had. Three copies of a compressor curve is three chances to disagree
/// about what the slider did.
[[nodiscard]] inline float CompressCutDb(const CompressConfig& cfg, float thresholdDb,
                                         float levelDb) {
    if (!cfg.enabled || levelDb <= thresholdDb) {
        return 0.0f;
    }
    // Under 1:1 the compressor would be an expander, which is not a thing this
    // section is for and would be a surprising way to make something louder.
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
    /// Under this: everything. Between: hero composites only - nobody resolves
    /// the detail at that range. Beyond: stop tracking the actor entirely.
    /// Culling is not an optimisation bolted on afterwards; it is what keeps a
    /// battlefield of ragdolls from ever becoming a performance question.
    float fullRadius{700.0f};        ///< ~10 m
    float simplifiedRadius{2100.0f};  ///< ~30 m


    // No rolloff parameter here: Skyrim attenuates a positioned voice itself,
    // through the BGSSoundOutput model attached to the sound, so a second curve of
    // ours only ever double-counted. These radii are budget boundaries - what to
    // spend a voice on - and never gain.
};

/// One mute per sound slot, applied at Stage 5 - after arbitration.
///
/// This is the "what does this layer actually contribute" switch, and it is
/// deliberately not the same thing as a strategy's `enabled` flag. Those gate at
/// proposal time, so turning one off hands its rate-cap and burst budget back to
/// the arbitrator and *other* cues move in to fill it: you hear a different mix,
/// not the same mix minus one element. Muting here leaves every arbitration
/// decision byte-identical and only silences what comes out, which is the only
/// way to A/B a layer honestly.
///
/// Both are wanted. Use `enabled` to answer "should the mod ship this feature",
/// and these to answer "is the late sub really doing the work" - which is the
/// design's central claim and the one thing worth checking by ear first.
struct LayerMuteConfig {
    // the impact composite, layer by layer
    bool impTransient{true};
    bool impBody{true};
    bool impSub{true};  ///< mute this one and the design says the gnarl should leave with it

    // grains and texture. The spine and limb crunch files have no mute of their
    // own: they are the same layer on a different bone, so they answer to the
    // mute of the crunch they are a variant of - exactly as the surface-coloured
    // scrapes answer to their loop. Silencing one part's damage and not the
    // others is what `Damage:b<Part>Enabled` is for, and it is the honest switch
    // for it because it stops the cue being *proposed* rather than muting it
    // after arbitration has already made room for it.
    bool limbTap{true};
    bool crunchGran{true};
    bool goreWet{true};
    bool scrapeGrain{true};

    // loops. The surface-coloured scrape variants have no mute of their own:
    // they are the same layer on a different floor, so they answer to the mute
    // of the loop they are a variant of.
    bool scrapeLoop{true};
    bool scrapeLimb{true};
    /// The bed under both grinds. Its own mute rather than the grind's, because
    /// silencing the mass and silencing the grit are the A/B this layer exists
    /// to make possible.
    bool scrapeLoopRumble{true};
    bool airWhoosh{true};

    // accents
    bool headImpact{true};
    bool settleRest{true};
};

/// One gain trim per sound slot, applied at Stage 5 beside the mutes.
///
/// MixConfig's trims are per *role* - every grain moves together, and so does
/// every loop - which is the right grain of control for balancing the composite
/// against the accents. It is the wrong grain for a bank: two files in one role
/// are two separate recordings arriving at two different levels, and pulling the
/// role trim to fix one takes the other down with it. Then the fix moves into the
/// wav, which is a file edit and a redeploy to undo.
///
/// The three surface skins are the clearest case of that and they are no longer
/// here: their trims moved into SurfaceConfig with the rest of the surface
/// controls, so the floor is tuned in one panel instead of four.
///
/// So one number per slot, summed on top of the role trim rather than replacing
/// it: the role trim says how loud that kind of layer should be, and this says
/// which file needed a decibel. Like the mutes it lands after arbitration, so
/// nothing here can change which cues were chosen - only what they come out at.
struct SlotGainConfig {
    float impTransient{0.0f};
    float impBody{0.0f};
    /// The limb body layer. Its own trim, `imp_body`'s mute - two recordings at
    /// two levels, one decision about whether the layer exists at all. Until
    /// somebody records `imp_body_limb_01.wav` this trims the `imp_body` file
    /// the limb composite falls back to, which is the useful half of it early:
    /// it sits every limb impact back without touching the torso's.
    float impBodyLimb{0.0f};
    float impSub{0.0f};

    float limbTap{0.0f};
    /// The three crunch files are three separate recordings arriving at three
    /// different levels, so each gets its own trim even though they answer to one
    /// mute. That is the whole reason this section exists - see above.
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
/// mod rather than a broken one, which is what lets thirteen files grow to
/// twenty-nine without touching code.
struct SlotResolutionConfig {
    /// Random selection repeats immediately, and immediate repeats are what
    /// people notice. A shuffle bag does not.
    bool shuffleBag{true};

    /// Fixed seed so a testbench A/B compares two configs and not two dice
    /// rolls. 0 means seed from the clock, which is what the game wants.
    std::uint32_t rngSeed{0};

    /// Draw every random choice for a cue from its own contact rather than from
    /// one running stream.
    ///
    /// A single stream makes an A/B useless: the shuffle bag and the scatter
    /// advance once per cue, so a change that adds or removes one cue early in a
    /// take re-rolls the variant and the pitch of everything after it, and two
    /// exports differ everywhere instead of where the change bit. Seeding per
    /// contact from `rngSeed` and the contact's own row means a config edit
    /// changes only the cues it actually affects - and changing `rngSeed` still
    /// re-rolls the whole take, so the variety is not lost.
    bool stableVariants{true};

    // -- conditional variants -------------------------------------------------
    //
    // A file on a slot may ask something of the contact before it is a candidate
    // - "only on stone", "only in plate", or both - and where it applies it
    // beats the plain files rather than merely joining them. The pack carries
    // the conditions; these three decide whether they are honoured.
    //
    // In [Slots] rather than in [Armor] because a condition can be surface-only
    // and has nothing to do with armour. This section already owns the shuffle
    // bag and the stable picker, which is the same kind of decision.

    /// Master. Off, every file on a slot is a candidate everywhere, exactly as
    /// before conditions existed.
    bool conditionalVariants{true};

    /// Honour the surface half, and the armour half.
    ///
    /// Two switches so that "is the stone-specific set actually earning its
    /// files?" can be answered. Turning one off collapses the ladder on that
    /// axis while the other keeps working, which is the only clean way to hear
    /// which half of a condition did the work. A half switched off is a half
    /// with no opinion - which is what `any` already means - so this needs no
    /// separate path through the resolver.
    bool surfaceConditions{true};
    bool armorConditions{true};
};

/// Everything RagdollSounds_Algorithm.ini carries.
/// A foot the body lands squarely on takes the fall. A foot that clips the floor
/// on the way past does not, and it should not sound like it did. The two differ
/// only in the angle they arrive at, so the measure is how much of the limb's
/// motion goes *into* the surface rather than past it: `impactSpeed / bodySpeed`,
/// which is the cosine of the angle of incidence.
///
/// Measured across three captures of the same knockdown: 0.995 for a foot the
/// body drops squarely onto (6 degrees off the normal), 0.737 for a knee that
/// takes some of it (42 degrees), 0.579 for a foot that clips the floor while
/// the body carries on into a whiplash (55 degrees). The three cases sound
/// different and the closing speed alone cannot tell them apart.
///
/// Scoped to the lower body, and that is not optional. A head dive comes in at
/// 0.611 and a hand thrown out in front of a fall is lower still; both are
/// glancing by this measure and both should stay loud. The number only means
/// "little was transferred" for a limb that was supposed to take the landing.
struct GlancingImpactConfig {
    bool enabled{false};

    /// Transfer at or above this is a square landing and is left alone; at or
    /// below `noTransferFrac` the reduction is full. Between them it ramps.
    float fullTransferFrac{0.90f};
    float noTransferFrac{0.55f};

    /// What intensity is multiplied by at full reduction. Intensity carries the
    /// *class* of the event: the composite/tap split is a branch on it, so a bad
    /// enough glance demotes itself to a tap, and it is also what the arbitrator
    /// ranks by. 1.0 leaves intensity alone and uses only the trim below.
    float maxIntensityScale{0.35f};

    /// Level taken off at full reduction, applied *before* arbitration - so the
    /// landing is quieter and also ranks lower, losing the hero slot to whatever
    /// hits next. It does not change the cue's class: only `maxIntensityScale`
    /// crosses the tap threshold.
    ///
    /// Negative is a cut.
    float maxGainCutDb{0.0f};

    /// Level taken off at full reduction, applied after arbitration - so it
    /// changes how loud the landing is and *nothing else*: not its class, not
    /// which contact wins the hero slot, not the phase. This is the knob for
    /// "quieter, but leave the rest of the fall where it was".
    ///
    /// Negative is a cut. Use it alone (with `maxIntensityScale` at 1.0) for
    /// pure loudness, or alongside it.
    float maxTrimCutDb{0.0f};

    /// A slide is not a clipped landing. Both are mostly-tangential motion, so
    /// the transfer ratio alone calls a body sliding along the floor a fully
    /// glancing landing and turns every contact of it into a tap.
    ///
    /// Tangent over closing speed tells them apart: a clipped foot runs about
    /// 1.4, a knee taking part of a fall 0.35, a squarely landed foot 0.06 -
    /// while a body sliding on its side runs 3 to 17. Between these two the
    /// reduction fades out, so nothing switches off at a cliff edge.
    float slideRatioStart{2.0f};
    float slideRatioFull{4.0f};

    /// Whether the thigh counts as a landing limb alongside the foot and calf.
    /// Off, only feet and knees are judged.
    bool includeThigh{true};

    /// Below this the limb is barely moving and the ratio is noise.
    float minBodySpeed{60.0f};

    /// Whether the same reduction also raises the bar for a crunch, by scaling
    /// the speed the crunch and gore gates compare against.
    ///
    /// Off by default. A bone can break at any angle, and the crunch is already
    /// quieter without this - its level rides on the intensity the ramp just
    /// reduced. Turn it on if glancing landings crunch too readily.
    bool scaleCrunchGate{false};
};

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
    /// `mix`'s per-layer trims again, split three ways by body part. Beside it
    /// rather than inside it because it is the same decision at a finer grain
    /// and the two are read together - one sets the composite's shape, the other
    /// says how far a forearm may differ from a spine.
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
/// One mapping rather than two. Stage 5 reads it to decide whether a layer is
/// audible and the testbench's slot panel writes through it, and a second copy
/// of the switch is exactly how a panel and an engine come to disagree about
/// what is muted. It also means a mute moving sections - as the three surface
/// skins just did - is one edit here rather than a hunt through two switches.
///
/// The two declared-and-unfilled slots have none, for the same reason they
/// have no trim: nothing resolves to them, so a control over either would be a
/// control over silence.
[[nodiscard]] bool* LayerMute(AlgorithmConfig& config, SlotId slot);
[[nodiscard]] const bool* LayerMute(const AlgorithmConfig& config, SlotId slot);

}  // namespace rds
