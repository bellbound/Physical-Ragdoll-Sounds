#pragma once

// Reaching an actor's Havok ragdoll - the limb rigid bodies the engine simulates.
//
// The ragdoll exists whenever the animation graph does, animated or not: an actor
// on their feet has these same bodies, keyframed and generating contacts nobody
// wants to hear. That is what lets us attach before a knockdown rather than after,
// and it is also exactly why the phase gate exists (07 section 1).
//
// Ported from skse/QuickModMenuNG/src/debug/Ragdoll.cpp, minus the skeleton
// parent table, which only the bone dump needed.

#include <string>
#include <vector>

namespace rds::game {

/// One ragdoll limb: the body Havok simulates, and the name a sound is chosen by.
struct RagdollBody {
    RE::hkRefPtr<RE::hkpRigidBody> body;

    /// The Havok body's own name. In Skyrim's skeletons this is the bone the body
    /// was authored on - "NPC L Forearm [LLar]" and friends - which is what makes
    /// a per-site sound set possible at all, and why LimbSite is resolved from
    /// this and never from the index (07 section 7).
    std::string name;

    float mass{};
    float radius{};
};

struct RagdollView {
    RE::NiPointer<RE::bhkWorld> world;
    std::vector<RagdollBody> limbs;

    [[nodiscard]] bool Valid() const { return world && !limbs.empty(); }
};

/// Read an actor's ragdoll out of its animation graph. Game thread only.
///
/// Takes the graph's update lock while it reads, because the graph is rebuilt on
/// 3D load and a torn read there is a dangling body pointer rather than a wrong
/// number. Bodies come back held by `hkRefPtr` so they stay alive for as long as
/// the caller keeps the view - but a body whose world has gone still has to be
/// re-checked before use, which is what `hkpWorldObject::world` is for.
[[nodiscard]] RagdollView CaptureRagdoll(RE::Actor& actor);

/// True when every limb still belongs to a physics world.
///
/// False means the ragdoll was rebuilt under us, which happens on cell change, on
/// 3D reload, and six times in three seconds on a standing actor that gets
/// disturbed. The answer is to re-capture, not to give up.
[[nodiscard]] bool BodiesLive(const RagdollView& view);

}  // namespace rds::game
