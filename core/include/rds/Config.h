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
/// every surface in the game to the same dirt sample. Layering ours on top
/// doubles everything and puts half the mix out of our control - so we silence
/// it by nulling the two BGSSoundDescriptorForm pointers on the body impact
/// records, in memory only, at data load.
///
/// It is global: it silences a dragged corpse and a thrown severed head too,
/// and any other mod expecting those descriptors loses them. Both are the right
/// trade for owning the mix and both belong in the mod description.
struct SuppressionConfig {
    bool suppressVanillaBodyImpacts{true};
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
    /// The default is Skyrim.esm's dialogue model (0x000B5184, reverb send 26,
    /// attenuates with distance), which is the one verified to behave. An audio
    /// overhaul may have a better-suited record; the log lists what the load
    /// order offers at debug level so it can be found.
    std::int32_t outputModelFormId{0x000B5184};

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
};

// ═════════════════════════════════════════════════════════════════════════════
// RagdollSounds_Algorithm.ini - the sound generation engine
// ═════════════════════════════════════════════════════════════════════════════

// ── Stage 0: Ingest ──────────────────────────────────────────────────────────

struct IngestConfig {
    /// Contacts below this never enter the pipeline. The capture's own floor was
    /// 5 u/s and produced a great deal of nothing; 20 is where the data's
    /// blow-up check first has anything to disagree about.
    float minImpactSpeed{20.0f};

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
    /// below this goes to the foley bed instead of the impact path; above it, a
    /// genuine self-hit gets through.
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
struct BudgetWaiver {
    float burstGapFrac{0.0f};
    bool ignoreRateCap{false};
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
    // So the slide leaves this state three ways, and only three: the body
    // stopped (`spent`, the ordinary Resting edge), the body left the ground
    // (`airborne`, the Airborne edge), or neither - in which case something
    // stopped it, and that is what `SlideExit::kStruck` and the slide-end
    // impact are. Nothing infers an end from the impacts any more.

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

    /// Whether a slide that ended by hitting something places an impact there.
    ///
    /// It is a real collision that the contact stream regularly misses - the
    /// limb that catches on the doorframe reports one glancing row and then the
    /// body is simply stopped - and without it the loudest moment of a slide is
    /// a loop fading out over silence. Off, the slide still ends; it just ends
    /// quietly, which is what the mod did before.
    bool slideEndImpact{true};

    /// Energy below this for `settleQuietMs` enters Resting and closes the event
    /// with one settle cue. Falls that trail off feel unfinished (§7).
    float settleEnergyFloor{45.0f};
    float settleQuietMs{300.0f};

    /// What it takes to leave Resting again, as a multiple of the fall's own
    /// peak closing speed - so it scales with how hard the knockdown was rather
    /// than being a speed that means different things in different falls.
    ///
    /// This is the edge the old machine did not have. `Settle`'s only exit was
    /// `Rest`, so the six loudest contacts of a fall arriving after it had
    /// closed were all judged at the settle budget and every one was dropped.
    /// But the edge cannot be free either: "any contact leaves it" hands the
    /// quiet tail back to masking alone, and the last twenty contacts of a
    /// knockdown being nearly silent is the main lever the design has for
    /// staying unobtrusive (00-Design §4). So a settling flop stays in Resting
    /// and a real contact does not.
    ///
    /// The floor underneath it is `settleEnergyFloor * 2`, for a knockdown so
    /// gentle that 40 % of its peak is still nothing.
    float restingExitPeakFrac{0.40f};

    /// Silence held after the ragdoll formally ends, covering the get-up blend:
    /// for this long after a `ragdoll_end` or `knock_get_up` row, nothing leaves
    /// Resting. Unmeasured - every capture take was paralysed and none of them
    /// ever got up (07 §1) - and it does not block v1, because a death ragdoll
    /// never gets up and simply never leaves Resting.
    float getUpBlendMs{400.0f};

    PhaseBudget launch{-6.0f, 2};
    PhaseBudget airborne{-9.0f, 1};
    PhaseBudget tumble{-3.0f, 4};
    PhaseBudget slide{-6.0f, 2};
    PhaseBudget resting{-14.0f, 1};
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

    /// Slide end: a body that was still travelling this fast when something
    /// stopped it gets a hero moment for it, as a fraction of
    /// `IntensityConfig::speedRefHigh` like every other gate here.
    ///
    /// This clause exists because the slide-end impact is the one contact in the
    /// mod that the dominance clause cannot judge fairly. A slide is a long
    /// stretch of grazes, so `energyRecent` is *low* when it ends - which would
    /// make a gentle stop dominant - and the impact itself is synthesised at the
    /// body's speed rather than a limb's, so it is not on the same scale as the
    /// peak it would be measured against. A speed of its own is the honest test:
    /// how fast was the body actually going when it was stopped.
    ///
    /// At the shipped anchor of 960 the default is 288 u/s, which is a body
    /// still travelling at roughly the speed of an ordinary shove. Zero switches
    /// the clause off and leaves the slide-end impact to the ordinary rules.
    float slideEndFrac{0.30f};

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

    /// Whether they also ignore the global rate cap.
    bool ignoreRateCap{true};

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

    /// Above this the contact is an obliterate, and the gore tier opens, as a
    /// multiple of `speedRefHigh`. Sits above anything a fall can produce.
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

    /// The crunch gate a head at full air time is held to, replacing
    /// `CrunchGoreConfig::crunchGateFrac` for that contact only - which is what
    /// puts a crunch on a slow dive without putting one on every fast sprawl.
    /// Zero leaves the crunch gate alone.
    float headCrunchGateFrac{0.55f};

    /// The chance such a head over that gate actually crunches. CrunchGore's own
    /// ramp is not used here: it runs from the gate up to `crunchCertainFrac`,
    /// and a dive that opens the gate at a third of the certain speed would come
    /// out at the bottom of that ramp and almost never fire - which is the thing
    /// this rule exists to fix. One number instead, so the aliveness is still
    /// tunable. 1.0 is always.
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
    // `HeroConfig::resetsBurst`, `burstGapFrac` and `ignoreRateCap`. Its
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

    // ── Damage: what a head strike is worth in broken bone ──────────────────
    //
    // Two thresholds and two ramps, on top of the accent rather than instead of
    // it. Past the first, a crunch lands just after the skull; past the second, a
    // gore layer lands with it.
    //
    // Distinct from `CrunchGoreConfig`, which is the body's rule: that one is a
    // *probability* gate, softened so an ordinary knockdown cracks sometimes and
    // a real fall always does, and it is deliberately vague because nobody can
    // check whether a given tumble should have broken something. A head strike is
    // not vague - the harder the skull lands the worse it is, every time - so this
    // one is deterministic and ramped on level instead.
    //
    // Where they meet, this wins: a contact this rule fires on is skipped by
    // `CrunchGoreStrategy` rather than being allowed to crunch twice for the same
    // reason. Both still share `maxCrunchesPerEvent`, so a long tumble cannot turn
    // into a bag of breaking sticks by coming in through two doors.

    bool damageEnabled{true};

    /// Threshold one, as a fraction of `IntensityConfig::speedRefHigh` like every
    /// other gate here.
    ///
    /// All three defaults are taken off the corpus rather than guessed, because
    /// the first set was not: 323 head contacts carry a closing speed, they run
    /// to a maximum of **732 u/s**, and their 95th percentile is 469. A gore tier
    /// pitched at the body's obliterate frac was therefore unreachable by a
    /// factor of well over one - it could not fire on any head ever recorded.
    ///
    /// 0.45 is 432 u/s, the top 7% of head contacts: a hard landing, not a knock.
    float crunchAtFrac{0.45f};

    /// Threshold two: from here the crunch is joined by the wet layer. 624 u/s,
    /// the top 4% - reachable, but only by the worst of them.
    ///
    /// Deliberately far under `CrunchGoreConfig::goreGateFrac`. The body's gore
    /// sits at the obliterate tier, above anything a fall can produce, because
    /// for a body it is a special case; for a head it is the second half of the
    /// feature and has to be somewhere a real fall can get to.
    float goreAtFrac{0.65f};

    /// Where the gore's own ramp reaches full. Past it a harder strike is louder
    /// through the composite and nothing here changes.
    ///
    /// 768 u/s, just above the hardest head in the corpus, so the ramp spans the
    /// real range and only something worse than anything recorded tops it out.
    float goreFullFrac{0.80f};

    /// How long after the accent each lands. Not zero by default: the crunch is
    /// what the skull *did*, and it reads as consequence rather than as texture
    /// when it arrives a beat late. Both are measured from the accent's own
    /// offset, so moving the accent moves them with it.
    float crunchDelayMs{25.0f};
    float goreDelayMs{40.0f};

    /// The two ramps, in dB against the contact's onset level.
    ///
    /// Each runs from its own threshold to its own ceiling, and the crunch's
    /// ceiling **is** the gore threshold: past there a harder strike has nothing
    /// more to say through the crunch and everything above belongs to the gore.
    /// Letting both ramp over the top of the range would make the worst impacts
    /// louder twice for one reason, which is how a tier structure turns into a
    /// volume problem.
    ///
    /// Quiet ends deliberately well down. Just over a threshold the layer should
    /// be barely there - the point of a ramp is that crossing it is not an event
    /// in itself - and only well above it should it be the thing you notice.
    float crunchQuietDb{-18.0f};
    float crunchLoudDb{-4.0f};
    float goreQuietDb{-20.0f};
    float goreLoudDb{-6.0f};
};

/// The gnarly gate. Discrete, because you cannot have thirty percent of a bone
/// break and one played quietly sounds like a bug - so the gate is softened with
/// probability, not with volume.
struct CrunchGoreConfig {
    bool crunchEnabled{true};
    bool goreEnabled{true};

    /// Where a crunch becomes possible and where it becomes certain, both as
    /// multiples of `IntensityConfig::speedRefHigh` - see HeadImpactConfig for
    /// why these are fractions and not speeds. At the default anchor of 960 they
    /// are 500 and 700: an ordinary knockdown cracks occasionally - which feels
    /// alive - and a real fall always does.
    float crunchGateFrac{0.52f};
    float crunchCertainFrac{0.73f};

    /// The chance of a crunch at the gate itself.
    ///
    /// Without it the ramp starts at zero, and a gate that never fires at the
    /// speed it opens at is not a gate that opens there - the bottom of the ramp
    /// was dead, and the crunch really began a good fifty units above where the
    /// slider said. That is the wrong shape for a probability gate: the point of
    /// softening one is that the threshold is a *maybe*, not a silence with a
    /// slope after it.
    float crunchGateProbability{0.15f};

    /// Hysteresis so the gate does not flicker during a tumble, on the same
    /// scale as the gate.
    float crunchHysteresisFrac{0.08f};

    /// At most this many crunches per ragdoll, so a long tumble does not turn
    /// into a bag of breaking sticks.
    std::int32_t maxCrunchesPerEvent{3};

    /// Gore sits at the obliterate tier - above anything a fall can produce -
    /// and is a multiple of `IntensityConfig::speedRefHigh` like the rest.
    float goreGateFrac{1.46f};

    float crunchGainDb{-4.0f};
    float goreGainDb{-6.0f};
};

/// Sustained grazing contact drives a looping voice attached to the limb.
/// Low-tilted grinding rumble with grain riding on it, about 20 dB under the
/// impacts - not a hiss.
struct ScrapeLoopConfig {
    bool enabled{true};

    /// The loop is the *voicing* of `Motion::kSlide` and nothing else. When a
    /// slide starts and stops is decided once, on the motion axis, under
    /// `[Motion]`'s slide keys - so the state, the grinding loop, the budget the
    /// impacts riding on it get, and the lane the timeline draws are four views
    /// of one decision rather than four rules that can disagree.
    ///
    /// They did disagree, and it was not subtle. The strategy had its own
    /// duration, distance and speed gates, all of them named differently from
    /// the motion axis' and none of them the same value, so a knockdown could be
    /// in `Slide` with no loop under it or grinding away in `Tumble`.
    float startFadeMs{60.0f};
    float stopFadeMs{140.0f};

    /// The fade used when the slide ended because the body left the ground.
    ///
    /// A slide that ends in friction ends slowly; one that ends because the body
    /// launched ends the instant the surface does, and the ordinary fade drags a
    /// grinding rumble out behind a body that is already in the air. Shorter
    /// than `fStopFadeMs`, and separate rather than shared, because the two are
    /// tuned against different pictures.
    float launchFadeMs{45.0f};

    float gainDb{-20.0f};

    /// Loop level and pitch track the *body's measured speed* continuously
    /// between these. That is the whole of what makes a slide loud: there is no
    /// distance term, no duration term and no ramp - a slide is exactly as loud
    /// as the body is fast, which is the one thing about it a listener can check
    /// against what they see.
    ///
    /// The fades stay, because their job is to hide the transition rather than
    /// to shape the level, and a loop that snaps in at its running level is the
    /// most obvious thing in the mix.
    float speedForMinGain{120.0f};
    float speedForMaxGain{600.0f};

    /// How far under `fGainDb` the loop sits at `fSpeedForMinGain`. The depth of
    /// the speed dependence, in other words: at 0 dB the slide is one level
    /// whatever it is doing, and deep enough that a crawl is inaudible is what
    /// keeps a body settling on its side from sounding like it is being dragged.
    float speedRangeDb{-12.0f};

    float pitchPerThousandUnits{0.15f};
};

/// The continuous bed: cloth, air, and the airborne anticipation rise. The
/// references put it 30-36 dB under the hero hit, and it is what papers over the
/// one-shots underneath.
struct MotionFoleyConfig {
    bool enabled{true};
    float bedGainDb{-33.0f};
    /// Bed level tracks body speed between these.
    float speedForMinGain{30.0f};
    float speedForMaxGain{500.0f};

    /// The airborne anticipation rise. On by default at a low level.
    bool airborneRise{true};
    float airborneRiseGainDb{-24.0f};

    /// The bed falls 8-15 dB in the 50-100 ms before a hero impact in three of
    /// the four reference clips - possibly a deliberate pre-duck, possibly just
    /// what being airborne does. Three out of four is not enough to build on,
    /// so it is a toggle to be tuned by ear, not load-bearing.
    bool preImpactDuck{false};
    float preImpactDuckDb{-10.0f};
    float preImpactDuckMs{70.0f};
};

/// One quiet cue when energy drops below the floor. Falls that trail off feel
/// unfinished.
struct SettleCloseConfig {
    bool enabled{true};
    float gainDb{-16.0f};
    float delayMs{120.0f};
};

struct StrategiesConfig {
    ImpactCompositeConfig impact;
    /// Not a strategy of its own - a measurement three of them read. It lives
    /// here because it is tuned with them and because the head half was part of
    /// `head` until the body half wanted the same timestamp.
    AirTimeConfig airTime;
    HeadImpactConfig head;
    CrunchGoreConfig crunch;
    ScrapeLoopConfig scrape;
    MotionFoleyConfig foley;
    SettleCloseConfig settle;
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
struct SurfaceConfig {
    /// Off, nothing gets a surface skin and the mod plays the same body on
    /// marble as on moss. The fastest A/B for how much of the mix is surface
    /// identity at all; the three mutes below do the same one surface at a time.
    bool enabled{true};

    // -- on the composite -----------------------------------------------------

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

    /// The role trim - all three skins together. Up makes the floor material
    /// obvious; down makes every surface sound the same, which is what vanilla
    /// does. Was `Mix:fSurfaceTrimDb`.
    float trimDb{0.0f};

    /// One trim per file, summed on top of the role trim. The three skins are
    /// three separately recorded sounds arriving at three different levels, and
    /// pulling the role trim to fix stone takes wood and soft down with it.
    /// Were `SlotGain:fSurfWood` and its two neighbours.
    float woodTrimDb{0.0f};
    float stoneTrimDb{0.0f};
    float softTrimDb{0.0f};

    /// Per-skin mutes, applied at render like every other mute - so muting one
    /// leaves every arbitration decision identical and silences only what came
    /// out. Were `Layers:bSurfWood` and its two neighbours.
    bool wood{true};
    bool stone{true};
    bool soft{true};
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

    /// The four-layer impact. Its surface skin has no line of its own: a
    /// composite is one acoustic moment, the cut is taken once for the whole
    /// stack, and giving the skin its own threshold would be a second answer to
    /// a question that has one.
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
    float foleyDb{0.0f};
    float airborneDb{0.0f};
    float settleDb{0.0f};
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
/// proposal time, so turning one off hands its rate-cap and voice budget back to
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

    // grains and texture
    bool limbTap{true};
    bool crunchGran{true};
    bool goreWet{true};

    // loops
    bool scrapeLoop{true};
    bool foleyCloth{true};
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
    float impSub{0.0f};

    float limbTap{0.0f};
    float crunchGran{0.0f};
    float goreWet{0.0f};

    float scrapeLoop{0.0f};
    float foleyCloth{0.0f};
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
    MixConfig mix;
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
