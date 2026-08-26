#pragma once

// The live IFeed: Havok contact callback -> lock-free ring -> drained on the
// game thread.
//
// The shape is skse/QuickModMenuNG/src/debug/ImpactRecorder.cpp's, which already
// solved every hard part of this once against the same solver:
//
//   - The contact callback runs on a Havok worker thread inside the solver. It
//     may not touch a TESObjectREFR, may not allocate, and above all **cannot ask
//     an actor whether it is ragdolling** - only the game thread may. So the
//     phase is published from the game thread into an atomic and read here with
//     one relaxed load. The cost is that the gate is one tick coarse at the
//     boundary, which is the right price: two of the eleven takes with a ragdoll
//     window have their first contact 9 ms before their own ragdoll_start row,
//     and the contacts are still stamped correctly (07 section 1).
//   - The ring is Vyukov's bounded MPMC queue, sequence-stamped per slot rather
//     than flag-per-slot. See ContactRing.h for why that distinction is the
//     difference between losing one event and losing the session.
//   - `ragdoll_rebuilt` fires on cell change, on 3D reload, and six times in
//     three seconds on a standing actor that gets disturbed, so limbs are
//     resolved on every attach and never cached across one. The re-attach is
//     throttled, but deliberately not hard enough that a rebuild storm leaves the
//     mod deaf (07 section 7).
//
// Why the listeners go on before the knockdown: the ragdoll bodies exist and
// collide the whole time an actor is animated, so there is nothing to "turn on"
// at the moment of a fall - and waiting for one would miss its first contacts.
// Tracking starts when an actor is knocked or ragdolling, which includes the
// lead-in states, and the phase gate in the callback is what keeps a walking NPC
// silent.

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
    /// rather than read off the config in the callback for the same reason the
    /// phase is: the config lives behind a mutex and this is a Havok worker
    /// thread inside the solver.
    std::atomic<bool> hearAnimated{};

    ActorProfile profile;  ///< rebuilt on every ragdoll attach, never cached across one
};

/// The engine clock, shared by every part of the mod.
///
/// One definition because it has to be one clock: a cue's time and the FeedEvent
/// it came from are compared against each other, and two steady_clock epochs a
/// few milliseconds apart would put the sub layer in the wrong place.
[[nodiscard]] TimeMs NowMs();
void ResetClock();

class GameFeed final : public IFeed {
public:
    GameFeed();
    ~GameFeed() override;

    /// Arm the feed. Called once, after data load. The per-limb listeners are
    /// attached per actor as they are tracked, not here.
    void Install();

    /// Per tick, on the game thread: refresh the phase, the listener, the
    /// distance tier and the terrain material for every tracked actor, and pick
    /// up anyone newly knocked down. This is the publisher half of the atomic the
    /// callback reads.
    void PublishTick(float frameTimeSec);

    /// The radii that decide who is worth tracking. Taken from the algorithm
    /// config once rather than read per frame, because Algorithm() returns a copy
    /// of a large struct.
    void SetCullRadius(float units);

    /// How often the pose is published - 1 every tick, 0 never. Taken from the
    /// algorithm config alongside the cull radius, and for the same reason:
    /// Algorithm() returns a copy of a large struct and this is read per frame.
    void SetBodySampleEveryNTicks(std::int32_t ticks);

    /// Whether the ragdoll checks are bypassed, and what the pose is still owed
    /// while they are. Taken from the algorithm config alongside the two above
    /// and for the same reason.
    ///
    /// It is the feed's half of `GameIntegration:bAnimatedMode`: who is tracked,
    /// whether the contact callback's gate is open, whether an actor back on
    /// their feet is untracked, and whether the pose is measured for one. The
    /// engine has the other half, and both have to be open before a walking
    /// actor makes a sound.
    void SetGameIntegration(const GameIntegrationConfig& game);

    /// Detach every listener and drop every actor. Load screen, cell change.
    void Clear();

    /// Limb index to the node a voice can follow, for the player's own ragdoll.
    [[nodiscard]] RE::NiAVObject* BoneNode(ActorId actor, std::int32_t limbIndex) const;

    /// The actor's 3D root, which is what an NPC's voices follow.
    ///
    /// Every voice follows a node now, because a world coordinate does not
    /// survive: vanilla's own voice path and SkyrimNet's both end on
    /// SetObjectToFollow and neither calls SetPosition once between them, and
    /// ours - the only one placing by coordinate - was the one playing from the
    /// middle of the cell.
    [[nodiscard]] RE::NiAVObject* RootNode(ActorId actor) const;

    /// Where the game thinks this actor is, in world units. False when it is not
    /// tracked or has no 3D.
    ///
    /// Diagnostic only. It exists to answer the one question a placement log
    /// cannot answer on its own: a voice five metres from the ears is either a
    /// body five metres away or a body at arm's length whose contact points came
    /// out wrong, and those two look identical until you have the body's own
    /// position to hold them against.
    [[nodiscard]] bool ActorPosition(ActorId actor, Vec3& out) const;

    /// Put an event on the ring from outside the contact callback.
    ///
    /// For the one producer that is not the Havok solver: the vanilla impact
    /// hook, which runs on whatever thread the impact manager is on and wants
    /// exactly what the callback wants - somewhere to put an event that is not a
    /// lock and not an allocation. The ring is already multi-producer, so this is
    /// the existing guarantee rather than a new one.
    ///
    /// It goes on the same ring rather than a second one so a vanilla row is
    /// drained in time order with the contacts it belongs beside, and reaches the
    /// testbench through the tap that is already there.
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
    /// listeners are attached to, and the listeners themselves.
    ///
    /// Heap-allocated and never moved. The listeners hold raw pointers into
    /// `pub`, and ActorPublication is neither copyable nor movable anyway - it
    /// holds three atomics - so the map stores pointers rather than values.
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

        /// The same, for whether the player has this body in their hands: what
        /// the engine was last told, so only the edge is published. Cleared on
        /// `ragdoll_start`, which is the edge that gives the actor a fresh
        /// runtime in the engine with its own copy of this cleared.
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
