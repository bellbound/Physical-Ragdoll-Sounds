#pragma once

// The live IFeed: Havok contact callback -> lock-free ring -> drained on the
// game thread.
//
// The shape is skse/QuickModMenuNG/src/debug/ImpactRecorder.cpp's, which solved
// every hard part of this once against the same solver:
//
//   - The contact callback runs on a Havok worker thread inside the solver. It
//     may not touch a TESObjectREFR, may not allocate, and **cannot ask an actor
//     whether it is ragdolling** - only the game thread may. So the phase is
//     published into an atomic and read here with one relaxed load. The gate is
//     then one tick coarse at the boundary, which is the right price (07 §1).
//   - The ring is Vyukov's bounded MPMC queue, sequence-stamped per slot rather
//     than flag-per-slot. See ContactRing.h for why that is the difference
//     between losing one event and losing the session.
//   - `ragdoll_rebuilt` fires on cell change, on 3D reload, and six times in
//     three seconds on a disturbed standing actor, so limbs are resolved on every
//     attach and never cached across one. Re-attach is throttled, but not hard
//     enough that a rebuild storm leaves the mod deaf (07 §7).
//
// The listeners go on before the knockdown because the ragdoll bodies collide the
// whole time an actor is animated - there is nothing to turn on at the moment of
// a fall, and waiting for one would miss its first contacts. The phase gate in
// the callback is what keeps a walking NPC silent.

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "ContactRing.h"
#include "RagdollBodies.h"
#include "rds/Config.h"
#include "rds/Feed.h"

namespace rds::game {

class LimbListener;

/// What the game thread publishes for one tracked actor, and the contact
/// callback reads. Everything in it is either atomic or only written between
/// ticks while the callback is not running.
struct ActorPublication {
    std::atomic<std::uint8_t> phase{};      ///< ActorPhase, one relaxed load in the callback
    std::atomic<std::uint32_t> material{};  ///< terrain MATERIAL_ID under the actor
    std::atomic<bool> tracked{};

    /// Whether the callback's phase gate is open for an actor who is *not*
    /// ragdolling - `GameIntegration:bAnimatedMode`. Published beside the phase
    /// rather than read off the config, which lives behind a mutex.
    std::atomic<bool> hearAnimated{};

    /// Whether this actor is fighting or being fought, for `ActorMode`. Published
    /// beside the phase and read the same way: the contact callback is a Havok
    /// worker thread and cannot ask an actor anything, so both halves of the
    /// question have to be answered on the game thread and left here.
    std::atomic<bool> inCombat{};

    ActorProfile profile;  ///< rebuilt on every ragdoll attach, never cached across one
};

/// The engine clock, shared by every part of the mod. One definition because it
/// has to be one clock: a cue's time is compared against the FeedEvent it came
/// from, and two steady_clock epochs a few ms apart would misplace the sub layer.
[[nodiscard]] TimeMs NowMs();
void ResetClock();

class GameFeed final : public IFeed {
public:
    GameFeed();
    ~GameFeed() override;

    /// Arm the feed. Called once, after data load. The per-limb listeners are
    /// attached per actor as they are tracked, not here.
    void Install();

    /// Per tick, on the game thread: refresh the phase, listener, distance tier
    /// and terrain material for every tracked actor, and pick up anyone newly
    /// knocked down. The publisher half of the atomic the callback reads.
    void PublishTick(float frameTimeSec);

    /// The radii that decide who is worth tracking. Taken once rather than read
    /// per frame, because Algorithm() returns a copy of a large struct.
    void SetCullRadius(float units);

    /// How often the pose is published - 1 every tick, 0 never. Taken alongside
    /// the cull radius and for the same reason.
    void SetBodySampleEveryNTicks(std::int32_t ticks);

    /// The feed's half of `GameIntegration:bAnimatedMode`: who is tracked, whether
    /// the contact callback's gate is open, whether an actor back on their feet is
    /// untracked, and whether the pose is measured for one. The engine has the
    /// other half, and both must be open before a walking actor makes a sound.
    void SetGameIntegration(const GameIntegrationConfig& game);

    /// Detach every listener and drop every actor. Load screen, cell change.
    void Clear();

    /// Limb index to the node a voice can follow, for the player's own ragdoll.
    [[nodiscard]] RE::NiAVObject* BoneNode(ActorId actor, std::int32_t limbIndex) const;

    /// The actor's 3D root, which is what an NPC's voices follow. Every voice
    /// follows a node now: vanilla's voice path and SkyrimNet's both end on
    /// SetObjectToFollow and neither calls SetPosition, and ours - the only one
    /// placing by coordinate - was the one playing from the middle of the cell.
    [[nodiscard]] RE::NiAVObject* RootNode(ActorId actor) const;

    /// Where the game thinks this actor is, in world units. False when it is not
    /// tracked or has no 3D.
    ///
    /// Diagnostic only: a voice five metres from the ears is either a body five
    /// metres away or one at arm's length whose contact points came out wrong, and
    /// those look identical without the body's own position to hold them against.
    [[nodiscard]] bool ActorPosition(ActorId actor, Vec3& out) const;

    /// Put an event on the ring from outside the contact callback - for the one
    /// producer that is not the Havok solver, the vanilla impact hook. The ring is
    /// already multi-producer, so this is the existing guarantee.
    ///
    /// The same ring rather than a second one, so a vanilla row drains in time
    /// order with the contacts beside it and reaches the testbench through the
    /// existing tap.
    void PushEvent(const FeedEvent& event);

    /// Contacts the ring had to throw away. Non-zero means the margin was wrong.
    [[nodiscard]] std::uint64_t Dropped() const;

    // -- IFeed ---------------------------------------------------------------
    bool Drain(TimeMs untilMs, std::vector<FeedEvent>& out) override;
    [[nodiscard]] const ActorProfile* Profile(ActorId actor) const override;
    [[nodiscard]] const ListenerState& Listener() const override;
    [[nodiscard]] float FrameTimeSec() const override;

private:
    /// One tracked actor: what is published to the callback, the bodies the
    /// listeners are attached to, and the listeners themselves. Heap-allocated and
    /// never moved - the listeners hold raw pointers into `pub`, and
    /// ActorPublication holds atomics so it is neither copyable nor movable.
    struct Tracked {
        ActorPublication pub;
        RagdollView ragdoll;
        std::vector<std::unique_ptr<LimbListener>> listeners;
        bool attached{};
        int rebuildCountdown{};
        int idleTicks{};

        /// What we last told the engine, so a transition can be spotted and the
        /// knockdown opened and closed. Starts unknown, so the first tick after a
        /// knockdown publishes ragdoll_start rather than assuming it.
        ActorPhase lastPhase{ActorPhase::kUnknown};
        /// The other half of the mode, edge-published the same way. False rather
        /// than "unknown": an actor we have not seen fight is not fighting.
        bool lastInCombat{};

        /// The same for whether the player has this body in hand, so only the edge
        /// is published. Cleared on `ragdoll_start`, the edge that gives the actor
        /// a fresh runtime in the engine.
        bool held{};

        RE::NiPointer<RE::TESObjectREFR> ref;
    };

    void Attach(Tracked& tracked, ActorId actor);
    void Detach(Tracked& tracked);
    void BuildProfile(Tracked& tracked, RE::Actor& actor);
    void RefreshListener();
    [[nodiscard]] bool ShouldTrack(RE::Actor& actor, float distanceSq) const;

    ContactRing m_ring;
    ListenerState m_listener;
    float m_frameTimeSec{};
    float m_cullRadius{2100.0f};
    std::int32_t m_bodySampleEveryNTicks{1};
    /// `GameIntegration:bAnimatedMode`, and whether anything downstream of it
    /// still wants the pose of an actor who is on their feet.
    bool m_animatedMode{};
    bool m_animatedPose{};
    std::uint64_t m_tick{};

    mutable std::mutex m_mutex;
    std::unordered_map<ActorId, std::unique_ptr<Tracked>> m_actors;
    bool m_installed{};

    /// Scratch for the drain, so the steady path allocates nothing.
    std::vector<FeedEvent> m_scratch;
};

}  // namespace rds::game
