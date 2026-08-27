#pragma once

// The six stages, behind one object.
//
// Feed in, cues out, driven by a clock the caller owns - the frame hook in the
// game, a virtual clock in the testbench. Same object, same order, same output,
// which is the only reason tuning offline means anything about the game.
//
//   Stage 0  Ingest       gate, dedupe, reject blow-ups, tag, route self-contacts
//   Stage 1  Crash state  a few dozen floats per tracked actor
//   Stage 2  Motion       Launch <-> Airborne <-> Tumble <-> Slide
//            Moment       Ordinary <-> Hero. Two axes; see Types.h for why
//   Stage 3  Strategies   the pluggable layer. They propose only
//   Stage 4  Arbitration  fixed rules, in order. It disposes
//   Stage 5  Render       cue list -> voices, through ICueSink
//
// Everything except Stage 3 is fixed infrastructure, which is what keeps the
// strategy layer from becoming a plugin framework.

#include <cstdint>
#include <memory>
#include <vector>

#include "rds/Config.h"
#include "rds/Cue.h"
#include "rds/Feed.h"
#include "rds/SlotManifest.h"
#include "rds/Types.h"

namespace rds {

/// Stage 1: the running per-actor summary. A few dozen floats, and the only
/// state the strategies get to see.
struct CrashState {
    ActorId actorId{};
    bool active{};

    TimeMs ragdollStartMs{};
    TimeMs lastContactMs{};

    // ── Stage 2, two axes ────────────────────────────────────────────────────
    //
    // One enum used to carry both of these and could answer neither properly.
    // See `Motion` and `Moment` in Types.h for what each is and why they split.

    /// What the body is doing. Physics owns it; it transitions freely.
    Motion motion{Motion::kLaunch};
    TimeMs motionEnteredMs{};

    /// What the mix is doing. Design owns it; latched and windowed.
    Moment moment{Moment::kOrdinary};
    /// When the hero window opened, or last re-anchored onto a bigger contact.
    TimeMs heroSinceMs{};
    /// The closing speed the open window is anchored on, units/s. A contact
    /// clearly above this re-anchors rather than opening a second moment.
    float heroPeakSpeed{};
    /// Which contact anchored it, so the head accent can tell "I am the hero" from
    /// "I am inside somebody else's hero window".
    std::uint32_t heroSeq{};
    /// Heroes opened this knockdown. A body bouncing down a staircase has more
    /// than one real landing in it, but the ini may still cap them.
    std::uint32_t heroCount{};

    float energyAccum{};   ///< running sum of admitted contact intensity
    float peakSpeed{};     ///< strongest closing speed so far, units/s
    std::uint32_t contactCount{};
    std::uint32_t admittedCount{};

    float bodySpeed{};  ///< the COM body's speed

    // ── measured pose, when the take carries it ──────────────────────────────
    //
    // Height above the ground used to live here. It was never read and it was the
    // wrong question: the ground reference is the last floor contact, which goes
    // stale exactly when a body is travelling. What the mix wants to know is
    // whether the body is *unsupported*, which needs no ground reference at all.

    Vec3 comPosition{};   ///< mass-weighted body centre, from the pose samples
    Vec3 comVelocity{};
    float verticalSpeed{};  ///< comVelocity.z, units/s. Negative is falling
    /// d(verticalSpeed)/dt, units/s^2. Free fall in game units is about -686
    /// (9.8 m/s^2 x 69.99 u/m); measured at -675 across the real fall in
    /// Vayne_impacts_log_2_cut_4 and at *+229* across the opening scuff the old
    /// rule read as fully airborne.
    float verticalAccel{};
    /// True while the body is in free flight: nothing is holding it up.
    bool airborne{};
    /// When the current unsupported stretch began, and how far the body has
    /// dropped since. Relative, and so survives a staircase.
    TimeMs airborneSinceMs{};
    float fallDropUnits{};

    /// True while something other than gravity and the ground is moving this body:
    /// a leash, a shout, a blast, a spell that throws the target. Measured from the
    /// acceleration alone (see `MotionConfig::drivenResidual`), so a mod that does
    /// not answer questions is not a blind spot.
    bool driven{};
    /// How far this tick's acceleration sits from gravity, u/s^2. Kept for the
    /// trace: a flight that is *nearly* driven is the interesting case when this
    /// gate is being tuned.
    float drivenResidual{};
    /// When the body was last unsupported *and* unpowered - flight start, pushed
    /// forward by every driven tick. This, not `airborneSinceMs`, is what air time
    /// means: a body being hauled through the air has not been falling.
    TimeMs freeFlightSinceMs{};
    /// False on a take recorded before the pose sidecar existed. Every rule above
    /// has to fall back when this is false: the fields are all zero, and zero is a
    /// lie rather than a neutral value.
    bool haveBodySamples{};
    LimbSite leadingLimb{};  ///< the site carrying the most energy into the fall
    bool headDown{};

    // ── the garment ──────────────────────────────────────────────────────────
    //
    // How much the clothes are moving, 0 to 1. Two fields because the envelope is
    // most of the tuning: the timeline draws the smoothed level as a filled curve
    // with the raw drive as a line over it, and the gap between them *is* the
    // attack and the release.
    //
    // Both stay 0 unless `[Rustle] bEnabled` is on, and on a take with no pose
    // sidecar, where the layer switches off rather than guessing.

    /// This tick's measurement, before the envelope. Spiky by nature: it is a
    /// second derivative of a pose stream taken on a body that is being hit.
    float rustleDriveRaw{};
    /// After the attack/release envelope. What the loop's level and pitch read.
    float rustleDrive{};

    /// How violent the last few hundred milliseconds have been, 0 to 1.
    ///
    /// The same measurement the garment is built from, held differently: it
    /// **decays every tick but rises only on ticks with no contact in them**. That
    /// asymmetry is the whole of it (01 §7.4) - a limb striking stone produces an
    /// enormous relative acceleration, so letting contact ticks raise it would read
    /// each collision back to itself and call the result context.
    ///
    /// What survives is the free thrashing *between* collisions, which is what
    /// "this is a bad tumble" means. `DamageViolenceConfig` is its only reader, and
    /// because the rise is suppressed on contact ticks it needs no `…BeforeTick`
    /// twin.
    float motionViolence{};

    /// How the last slide ended - the only part of a slide `motion` cannot say,
    /// since a slide leaves it two ways that sound different. Held after the fact:
    /// the scrape loop reads it on the tick it stops to choose between the ordinary
    /// and launch fades, and the timeline to mark the end of a span it has drawn.
    SlideExit slideExit{SlideExit::kNone};

    SurfaceClass surfaceUnder{SurfaceClass::kSoft};
    float distanceToListener{};
    DistanceTier tier{DistanceTier::kFull};

    /// The decaying loudness ceiling temporal masking works against.
    float maskCeilingDb{-120.0f};
};

/// What one run produced, for the log's per-knockdown summary line and the
/// testbench's counters. The reduction ratio is the number to watch: roughly 10:1,
/// four to six audible moments against 30-60 collisions.
struct EngineStats {
    std::uint32_t eventsIn{};
    std::uint32_t contactsIn{};
    std::uint32_t rejectedBlowup{};
    std::uint32_t rejectedBelowFloor{};
    std::uint32_t droppedMirror{};
    std::uint32_t collapsedManifold{};
    /// Quiet self-contacts dropped at ingest: one limb brushing another
    /// limb of the same body, which makes no impact sound.
    std::uint32_t droppedSelfContact{};
    std::uint32_t proposedCues{};
    std::uint32_t droppedRateCap{};
    /// Onsets inside the nominal rate cap that got through anyway: either properly
    /// louder than the onset holding it, or a hero moment scaled the cap down for
    /// its peers. Counted rather than inferred - the verifier can reconstruct
    /// neither from the cue list.
    std::uint32_t rateCapOverrides{};
    std::uint32_t droppedChainMerge{};
    std::uint32_t droppedMasking{};
    std::uint32_t droppedBurstCap{};
    /// Hero moments opened, and the times one re-anchored onto a bigger contact
    /// rather than opening a second. (They replace `headRefunds` and `airResets`:
    /// there is one place the budgets bend now, and it is a moment-axis decision
    /// rather than a waiver bought per contact.)
    ///
    /// Four heroes in a take means the floor is too low; none is either a gentle
    /// slump or a floor too high. The reanchor count separates "one moment with
    /// peers" from "three separate events".
    std::uint32_t heroes{};
    std::uint32_t heroReanchors{};
    /// Of those, the ones `[HeadImpact]`'s hero floor relief is responsible for -
    /// anchored on a head that would have failed the ordinary floor or the
    /// dominance ratio without it. Zero with the relief on is a threshold set too
    /// high to reach, which `heroes` alone cannot distinguish from a relief firing
    /// only on contacts that would have anchored regardless.
    std::uint32_t heroHeadRelief{};
    /// Slides the entry test found at all - it used to find none on the one take
    /// that is mostly sliding. (`slideImpacts` went with the slide-end lift: it
    /// read 0 on all eight takes, which is the measurement that said the lift was
    /// standing in for an exit test rather than describing anything.)
    std::uint32_t slides{};
    /// The garment's two raw measurements at their peak, **before** either ramp:
    /// fabric-weighted mean relative limb acceleration in u/s^2, and fabric-weighted
    /// mean limb surface speed from rotation in u/s.
    ///
    /// Pre-ramp on purpose. `fThrashFloor` and `fThrashFull` are guesses because
    /// nothing had measured a tumble - free fall is 686 u/s^2 and a limb landing
    /// measured 5110, with the ordinary thrashing of a fall somewhere between.
    /// Running the corpus with `Rustle:bEnabled=1` and reading these columns is the
    /// measurement that replaces them; a peak against the *normalised* drive could
    /// not, because the normalisation is what is being set.
    float rustleThrashPeak{};
    float rustleTumblePeak{};
    /// The same two averaged over every measured tick, which is the number the
    /// ramps want. The peak alone is an impact frame by construction - one limb
    /// reversing against stone in a single solver step - and a ramp stretched to
    /// reach it flattens an ordinary tumble to nothing. Peak and mean bracket it:
    /// the floor belongs under the mean and the top between the two.
    float rustleThrashMean{};
    float rustleTumbleMean{};
    std::uint32_t rustleTicks{};
    /// The highest `CrashState::motionViolence` this run reached, 0 to 1 - whether
    /// the damage rule's violence term can do anything on this corpus at all. A
    /// take whose peak is 0.2 will never see the full gate drop however the
    /// fractions are set.
    float violencePeak{};
    /// The damage rule's own raw thrash - mass-weighted, not fabric-weighted - at
    /// its peak and averaged. A separate pair from `rustleThrash…` because the two
    /// measurements weight the same limbs differently.
    float violenceThrashPeak{};
    float violenceThrashMean{};
    std::uint32_t violenceTicks{};

    /// Accumulated damage: the fullest any one limb's pool got, and how many breaks
    /// the ladder produced. The peak says whether the ladder is reachable on a
    /// given take - a corpus whose pools top out at 0.4 will never fire a rung
    /// pitched at 0.6, the dead-gate trap the tiers fell into once already (01 §4).
    float accumPoolPeak{};
    std::uint32_t accumBreaks{};

    void AccumPeak(float pool) { accumPoolPeak = accumPoolPeak < pool ? pool : accumPoolPeak; }
    /// Flights that were powered by something other than gravity at some point.
    /// A take where this is high and `heroes` is also high is a take whose big
    /// moments are being handed out for someone else's impulse.
    std::uint32_t drivenFlights{};
    std::uint32_t emittedCues{};
    /// Cues arbitration admitted and paid for, then a layer mute silenced. Counted
    /// apart from emittedCues so a muted A/B still shows what the arbitrator
    /// decided rather than looking like it decided less.
    std::uint32_t mutedCues{};
    /// Cues the class compressor held down. Not a drop - all of them still played -
    /// but zero means a threshold too high to do anything, and a count equal to
    /// `emittedCues` means it is squeezing everything rather than just the top.
    std::uint32_t compressedCues{};
    std::uint32_t bursts{};
    float peakSpeed{};
    TimeMs firstCueMs{};
    TimeMs lastCueMs{};

    /// contactsIn / bursts. The design's 10:1.
    [[nodiscard]] float ReductionRatio() const;
};

/// One line per decision, for the testbench's timeline. Only filled when
/// tracing is on, which the game never turns on.
struct TraceRecord {
    TimeMs timeMs{};
    ActorId actorId{};
    std::uint16_t limbIndex{};
    std::uint32_t sourceSeq{};
    float impactSpeed{};
    float intensity{};
    Motion motion{};
    Moment moment{};
    /// "rate cap", "masked -14 dB", "emitted", ... Short and human.
    char outcome[56]{};
};

class Engine {
public:
    Engine();
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    /// The bank stays owned by the caller and must outlive the engine.
    void SetSoundBank(SoundBank* bank);

    /// Cues go here. Null means the engine runs and produces nothing, which is
    /// what `Enabled=0` looks like.
    void SetSink(ICueSink* sink);

    /// Config is copied per Tick, so pushing a new one from the testbench takes
    /// effect on the next tick with no restart and no torn read.
    /// All three tuning columns at once. There is no single-config setter: an
    /// engine holding one column and two stale ones is a bug that only shows up
    /// on an actor who happened to be upright.
    void SetConfig(const ConfigSet& config);

    /// Collect the trace. Off by default: it allocates per decision.
    void SetTracing(bool on);
    [[nodiscard]] const std::vector<TraceRecord>& Trace() const;

    /// Advance to `nowMs`, draining everything the feed has up to it. Once per
    /// frame from the game thread live; at each frame boundary the recording
    /// implies offline. Frame-scaled windows use IFeed::FrameTimeSec, so the two
    /// behave the same at 24 fps and at 144.
    void Tick(IFeed& feed, TimeMs nowMs);

    /// Drop every tracked actor and every running loop. Called on load game,
    /// cell change, and between testbench runs.
    void Reset();

    [[nodiscard]] const EngineStats& Stats() const;
    [[nodiscard]] const CrashState* State(ActorId actor) const;
    [[nodiscard]] std::size_t TrackedActors() const;

    /// The `index`th tracked actor, or null past the end. `State()` answers for an
    /// actor you can already name, which the game always can; the offline runner
    /// discovers who is in a recording by replaying it.
    [[nodiscard]] const CrashState* ActorAt(std::size_t index) const;

    /// Voices booked right now, across every actor. Nothing is capped against it;
    /// it is the one number that makes a voice taken and never given back visible.
    /// Worth reading when the last actor lets go, when it should be zero.
    [[nodiscard]] std::size_t LiveVoices() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace rds
