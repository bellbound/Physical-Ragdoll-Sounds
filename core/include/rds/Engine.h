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
//   Stage 2  Phase        Launch -> Airborne -> PrimaryImpact -> Tumble -> Slide -> Settle -> Rest
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
    TimeMs phaseEnteredMs{};
    Phase phase{Phase::kRest};

    float energyAccum{};   ///< running sum of admitted contact intensity
    float peakSpeed{};     ///< strongest closing speed so far, units/s
    std::uint32_t contactCount{};
    std::uint32_t admittedCount{};

    float bodySpeed{};   ///< the COM body's speed
    float height{};      ///< above the last known ground contact, units
    bool airborne{};
    LimbSite leadingLimb{};  ///< the site carrying the most energy into the fall
    bool headDown{};
    float slidingRatio{};  ///< share of recent contacts that were grazes

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
    std::uint32_t emittedCues{};
    /// Cues arbitration admitted and paid for, then a layer mute silenced.
    /// Counted apart from emittedCues so a muted A/B still shows what the
    /// arbitrator decided, rather than looking like it decided less.
    std::uint32_t mutedCues{};
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
    Phase phase{};
    /// "rate cap", "masked -14 dB", "emitted", ... Short and human.
    char outcome[32]{};
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

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace rds
