#pragma once

// The six stages, behind one object.
//
// Feed in, cues out, driven by a clock the caller owns. The game drives it from
// the frame hook; the testbench drives it from a virtual clock as fast as it
// likes. Same object, same order of operations, same output - which is the only
// reason tuning offline against a recording means anything about the game.
//
//   Stage 0  Ingest       gate, dedupe, reject blow-ups, tag, route self-contacts
//   Stage 1  Crash state  a few dozen floats per tracked actor
//   Stage 2  Motion       Launch <-> Airborne <-> Tumble <-> Slide <-> Resting
//            Moment       Ordinary <-> Hero. Two axes; see Types.h for why
//   Stage 3  Strategies   the pluggable layer. They propose only
//   Stage 4  Arbitration  fixed rules, in order. It disposes
//   Stage 5  Render       cue list -> voices, through ICueSink
//
// Everything except Stage 3 is fixed infrastructure. That is deliberate, and it
// is what keeps the strategy layer from becoming a plugin framework.

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
    Motion motion{Motion::kResting};
    TimeMs motionEnteredMs{};

    /// What the mix is doing. Design owns it; latched and windowed.
    Moment moment{Moment::kOrdinary};
    /// When the hero window opened, or last re-anchored onto a bigger contact.
    TimeMs heroSinceMs{};
    /// The closing speed the open window is anchored on, units/s. A contact
    /// clearly above this re-anchors rather than opening a second moment.
    float heroPeakSpeed{};
    /// Which contact anchored it, so the head accent can tell "I am the hero"
    /// from "I am inside somebody else's hero window".
    std::uint32_t heroSeq{};
    /// Heroes opened this knockdown. A fall is allowed more than one - a body
    /// bouncing down a staircase has more than one real landing in it - but not
    /// an unbounded number if the ini says so.
    std::uint32_t heroCount{};

    float energyAccum{};   ///< running sum of admitted contact intensity
    float peakSpeed{};     ///< strongest closing speed so far, units/s
    std::uint32_t contactCount{};
    std::uint32_t admittedCount{};

    float bodySpeed{};  ///< the COM body's speed

    // ── measured pose, when the take carries it ──────────────────────────────
    //
    // Height above the ground used to live here and does not any more. It was
    // never read, and it was the wrong question: the ground reference is the
    // last floor contact, which goes stale exactly when a body is travelling -
    // a body tumbling down a staircase is measured against a step it left three
    // bounces ago. What the mix actually wants to know is whether the body is
    // *unsupported*, and that can be measured with no ground reference at all.

    Vec3 comPosition{};   ///< mass-weighted body centre, from the pose samples
    Vec3 comVelocity{};
    float verticalSpeed{};  ///< comVelocity.z, units/s. Negative is falling
    /// d(verticalSpeed)/dt, units/s^2. Free fall in game units is about -686
    /// (9.8 m/s^2 x 69.99 u/m); measured at -675 across the real fall in
    /// Vayne_impacts_log_2_cut_4, and at *+229* across the opening scuff that
    /// the old rule read as fully airborne.
    float verticalAccel{};
    /// True while the body is in free flight: nothing is holding it up. This is
    /// the signal the air-time rules should have been reading all along.
    bool airborne{};
    /// When the current unsupported stretch began, and how far the body has
    /// dropped since. The drop is relative and so survives a staircase, which
    /// is what an absolute height could never do.
    TimeMs airborneSinceMs{};
    float fallDropUnits{};

    /// True while something other than gravity and the ground is moving this
    /// body: a leash pulling on a collar, a shout, a blast, a spell that throws
    /// the target. Measured from the acceleration alone - see
    /// `MotionConfig::drivenResidual` - so it costs nothing to support a mod
    /// nobody has written yet, and a mod that does not answer questions is not a
    /// blind spot.
    bool driven{};
    /// How far this tick's acceleration sits from gravity, u/s^2. Kept for the
    /// trace: a flight that is *nearly* driven is the interesting case when this
    /// gate is being tuned, and a bool cannot say it.
    float drivenResidual{};
    /// When the body was last unsupported *and* unpowered - flight start, pushed
    /// forward by every driven tick. This, not `airborneSinceMs`, is what air
    /// time means: a body being hauled through the air has not been falling, and
    /// the rules that pay out for a long fall must not pay for a long pull.
    TimeMs freeFlightSinceMs{};
    /// False on a take recorded before the pose sidecar existed. Every rule that
    /// reads the fields above has to fall back when this is false, because they
    /// are all zero and zero is a lie rather than a neutral value.
    bool haveBodySamples{};
    LimbSite leadingLimb{};  ///< the site carrying the most energy into the fall
    bool headDown{};

    /// How the last slide ended, which is the only part of a slide `motion`
    /// cannot say: the state names where the body is, and a slide leaves it
    /// three ways that sound nothing like each other - see `SlideExit`.
    ///
    /// Held after the fact rather than being a transient. The scrape loop reads
    /// it on the tick it stops, to choose between the ordinary fade and the
    /// launch fade, and the timeline reads it to mark the end of a span it has
    /// already drawn.
    SlideExit slideExit{SlideExit::kNone};

    SurfaceClass surfaceUnder{SurfaceClass::kSoft};
    float distanceToListener{};
    DistanceTier tier{DistanceTier::kFull};

    /// The decaying loudness ceiling temporal masking works against.
    float maskCeilingDb{-120.0f};
};

/// What one run produced, for the log's per-knockdown summary line and for the
/// testbench's counters. The reduction ratio is the number to watch: the target
/// is roughly 10:1, four to six audible moments against 30-60 collisions.
struct EngineStats {
    std::uint32_t eventsIn{};
    std::uint32_t contactsIn{};
    std::uint32_t rejectedBlowup{};
    std::uint32_t rejectedBelowFloor{};
    std::uint32_t droppedMirror{};
    std::uint32_t collapsedManifold{};
    std::uint32_t routedToFoley{};
    std::uint32_t proposedCues{};
    std::uint32_t droppedRateCap{};
    /// Onsets that were inside the rate cap and got through anyway by being
    /// properly louder. Counted rather than inferred: the verifier cannot
    /// reconstruct the decision from the cue list, because the level the
    /// arbitrator judged is the proposed stack's and layers can still be lost to
    /// the voice cap afterwards.
    std::uint32_t rateCapOverrides{};
    std::uint32_t droppedChainMerge{};
    std::uint32_t droppedMasking{};
    std::uint32_t droppedBurstCap{};
    std::uint32_t droppedVoiceCap{};
    /// Hero moments opened, and the times one of them re-anchored onto a bigger
    /// contact rather than opening a second.
    ///
    /// These two replace the `headRefunds` and `airResets` counters, which
    /// counted the two places the arbitrator's budgets used to bend. There is
    /// one place now, and it is a decision of the moment axis rather than a
    /// waiver bought per contact - so what is worth counting is the moment.
    ///
    /// A take with four heroes is a take whose floor is set too low; a take with
    /// none is either a gentle slump, which is legitimate, or a floor set too
    /// high. The reanchor count separates "one moment with peers" from "three
    /// separate events", which the hero count alone cannot say.
    std::uint32_t heroes{};
    std::uint32_t heroReanchors{};
    /// Of those, the ones `[HeadImpact]`'s hero floor relief is responsible for:
    /// anchored on a head that would have failed the ordinary floor, or the
    /// dominance ratio measured against it, without the relief.
    ///
    /// The number that says whether the option is doing anything. Zero with it
    /// switched on is a threshold set too high to reach, which from `heroes`
    /// alone is indistinguishable from a relief that only ever fires on contacts
    /// that would have anchored regardless.
    std::uint32_t heroHeadRelief{};
    /// Slides, and the ones that ended because something stopped the body.
    ///
    /// The pair is the readout for the whole slide rework. `slides` says the
    /// entry test is finding them at all - it used to find none on the one take
    /// that is mostly sliding - and the difference between the two says how
    /// often a slide ends by being interrupted rather than by the body coming to
    /// rest or leaving the ground, which is the case the old machine had no way
    /// to express and simply faded out over.
    std::uint32_t slides{};
    std::uint32_t slideImpacts{};
    /// Flights that were powered by something other than gravity at some point.
    /// A take where this is high and `heroes` is also high is a take whose big
    /// moments are being handed out for someone else's impulse.
    std::uint32_t drivenFlights{};
    /// Closing cues that were emitted while the body was measurably still in
    /// the air. Must be zero - a fall that announces it is over while the body
    /// is still falling reads as broken instantly, and this one has shipped
    /// before: 184 ms into a flight and 121 units above the ground.
    ///
    /// Counted rather than checked from the cue list, because whether the body
    /// was airborne at the time is engine state the cue does not carry.
    std::uint32_t settleInFlight{};
    std::uint32_t emittedCues{};
    /// Cues arbitration admitted and paid for, then a layer mute silenced.
    /// Counted apart from emittedCues so a muted A/B still shows what the
    /// arbitrator decided, rather than looking like it decided less.
    std::uint32_t mutedCues{};
    /// Cues the class compressor held down. Not a drop - every one of these
    /// still played - but a count of zero is how you tell a threshold that is
    /// set too high to do anything from one doing the work you asked for, and a
    /// count equal to `emittedCues` says it is squeezing everything rather than
    /// just the top.
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
    void SetConfig(const AlgorithmConfig& config);

    /// Collect the trace. Off by default: it allocates per decision.
    void SetTracing(bool on);
    [[nodiscard]] const std::vector<TraceRecord>& Trace() const;

    /// Advance to `nowMs`, draining everything the feed has up to it.
    ///
    /// Live, this is called once per frame from the game thread with the real
    /// clock. Offline, it is called at each frame boundary the recording
    /// implies. Windows that scale with frame rate use IFeed::FrameTimeSec, so
    /// the two behave the same at 24 fps and at 144.
    void Tick(IFeed& feed, TimeMs nowMs);

    /// Drop every tracked actor and every running loop. Called on load game,
    /// cell change, and between testbench runs.
    void Reset();

    [[nodiscard]] const EngineStats& Stats() const;
    [[nodiscard]] const CrashState* State(ActorId actor) const;
    [[nodiscard]] std::size_t TrackedActors() const;

    /// The `index`th tracked actor, or null past the end.
    ///
    /// `State()` answers for an actor you can already name, which the game
    /// always can. The offline runner cannot: it is handed a recording and
    /// discovers who is in it by replaying it, so sampling the measured body
    /// per tick needs a way in that does not start from an id.
    [[nodiscard]] const CrashState* ActorAt(std::size_t index) const;

    /// Voices booked right now, across every actor.
    ///
    /// Nothing is capped against this. It is kept because it is the one number
    /// that makes a voice taken and never given back visible, and that bug was
    /// invisible in a log counting only cues. Worth reading when the last actor
    /// lets go, because that is the moment it should be zero.
    [[nodiscard]] std::size_t LiveVoices() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace rds
