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

// ── Stage 2: Phase machine ───────────────────────────────────────────────────

/// What one phase is allowed to spend. The trim is in dB against the actor's
/// mix, so the settle phase being nearly silent is one number rather than a
/// special case in the arbitrator.
struct PhaseBudget {
    float gainTrimDb{0.0f};
    std::int32_t maxCuesPerBurst{4};
};

struct PhaseConfig {
    /// Height above the last known ground contact, in units, and how long off
    /// the ground before Airborne is believed.
    float airborneMinHeight{40.0f};
    float airborneMinTimeMs{120.0f};

    /// The first contact that carries this fraction of the fall's accumulated
    /// energy opens PrimaryImpact. Fire-on-first-contact is safe: 96.2 % of
    /// episodes peak on their first row (07 §9).
    float primaryImpactEnergyFrac{0.35f};

    /// How long PrimaryImpact stays open. The references' hero moments are a
    /// small group of peers inside a couple of hundred ms, not one hit.
    float primaryImpactWindowMs{220.0f};

    /// Sustained tangential contact for this long opens Slide.
    float slideMinTangentSpeed{120.0f};
    float slideMinDurationMs{150.0f};

    /// Energy below this for `settleQuietMs` closes the event with one settle
    /// cue. Falls that trail off feel unfinished (§7).
    float settleEnergyFloor{45.0f};
    float settleQuietMs{300.0f};

    /// Silence held after the ragdoll ends, covering the get-up blend. Unmeasured
    /// - every capture take was paralysed and none of them ever got up (07 §1).
    float getUpBlendMs{400.0f};

    PhaseBudget launch{-6.0f, 2};
    PhaseBudget airborne{-9.0f, 1};
    PhaseBudget primaryImpact{0.0f, 5};
    PhaseBudget tumble{-3.0f, 4};
    PhaseBudget slide{-6.0f, 2};
    PhaseBudget settle{-14.0f, 1};
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

    /// The real budget is voices, not CPU.
    /// A composite is four voices - transient, surface, body, sub - so eight
    /// could not hold two overlapping composites, and a hard crash is precisely
    /// two or three overlapping composites. At eight, log_4's landing lost the
    /// body and sub of its loudest contact and came out as a click.
    std::int32_t voiceCapPerActor{12};
    std::int32_t voiceCapGlobal{24};

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

    /// Layer offsets from the contact frame, in ms. Measured in the references:
    /// transient at 0, body at +8..34, weight at +46..100, sub at +64..74.
    /// Structured, not random - random jitter smears the shape rather than
    /// building it.
    float transientOffsetMs{0.0f};
    float surfaceOffsetMs{8.0f};
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
    float surfaceGainAtMinDb{-12.0f};
    float surfaceGainAtMaxDb{-6.0f};

    /// Pitch is free and continuous in this engine, and it beats doubling the
    /// bank. Random scatter per voice plus a systematic downward bias with
    /// intensity, so a heavier impact starts lower and reads bigger.
    float pitchScatterSemis{2.5f};
    float pitchIntensityBiasSemis{-3.0f};

    /// Stay inside this or the pitch trick starts sounding like a pitch trick.
    float pitchMaxSemis{3.0f};
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
    /// Head-down attitude at the moment of contact makes the gate more willing.
    float headDownBonus{0.25f};
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
    float minTangentSpeed{120.0f};
    float startFadeMs{60.0f};
    float stopFadeMs{140.0f};
    /// How long tangential contact has to persist before the loop starts, so a
    /// single glancing blow does not open one.
    float minDurationMs{120.0f};

    /// ...or this far travelled along the surface, whichever comes first.
    ///
    /// Time alone describes a slow grind and misses a fast skid entirely: log_3
    /// crosses two metres of floor in under the 120 ms the time gate wants, so
    /// the loop never opened on the one take that is mostly sliding. Distance is
    /// integrated from tangent speed rather than from contact positions - the
    /// contact point hops between manifold points and between limbs, and its
    /// displacement is mostly jitter that has nothing to do with sliding.
    float minDistance{45.0f};

    /// How far the body slides before the loop reaches full level. The loop can
    /// open on a skid, but it opens *quietly*, and only a real slide brings it
    /// up - which is what stops every glancing tumble contact sounding like a
    /// body being dragged.
    float fadeInDistance{160.0f};
    float fadeInFloorDb{-15.0f};
    float gainDb{-20.0f};
    /// Loop gain tracks tangent speed continuously between these.
    float speedForMinGain{120.0f};
    float speedForMaxGain{600.0f};
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
    HeadImpactConfig head;
    CrunchGoreConfig crunch;
    ScrapeLoopConfig scrape;
    MotionFoleyConfig foley;
    SettleCloseConfig settle;
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
    float surfaceTrimDb{0.0f};
    float grainTrimDb{0.0f};
    float loopTrimDb{0.0f};

    /// Below this a cue is not worth a voice and is dropped before the cap sees it.
    float voiceFloorDb{-48.0f};
};

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

    // surface skins
    bool surfWood{true};
    bool surfStone{true};
    bool surfSoft{true};

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
/// MixConfig's trims are per *role* - every surface skin moves together, and so
/// does every grain - which is the right grain of control for balancing the
/// composite against the accents. It is the wrong grain for a bank: the three
/// surface skins are three separately recorded files that arrive at three
/// different levels, and pulling `fSurfaceTrimDb` to fix stone quietly takes
/// wood and soft down with it. Then the fix moves into the wav, which is a file
/// edit and a redeploy to undo.
///
/// So one number per slot, summed on top of the role trim rather than replacing
/// it: the role trim says how loud that kind of layer should be, and this says
/// which file needed a decibel. Like the mutes it lands after arbitration, so
/// nothing here can change which cues were chosen - only what they come out at.
struct SlotGainConfig {
    float impTransient{0.0f};
    float impBody{0.0f};
    float impSub{0.0f};

    float surfWood{0.0f};
    float surfStone{0.0f};
    float surfSoft{0.0f};

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
};

/// Everything RagdollSounds_Algorithm.ini carries.
struct AlgorithmConfig {
    IngestConfig ingest;
    PhaseConfig phase;
    ArbitrationConfig arb;
    IntensityConfig intensity;
    EffectiveMassConfig limbs;
    StrategiesConfig strategies;
    MixConfig mix;
    PlayerConfig player;
    DistanceConfig distance;
    SlotResolutionConfig slots;
    SlotGainConfig slotGains;
    LayerMuteConfig layers;
};

}  // namespace rds
