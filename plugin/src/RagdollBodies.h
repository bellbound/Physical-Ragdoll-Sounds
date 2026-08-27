#pragma once

// Reaching an actor's Havok ragdoll - the limb rigid bodies the engine simulates.
//
// The ragdoll exists whenever the animation graph does: an actor on their feet has
// these same bodies, keyframed and generating contacts nobody wants to hear. That
// is what lets us attach before a knockdown, and why the phase gate exists (07 §1).
//
// Ported from skse/QuickModMenuNG/src/debug/Ragdoll.cpp, minus the skeleton parent
// table, which only the bone dump needed.

#include <string>
#include <vector>

namespace rds::game {

/// One ragdoll limb: the body Havok simulates, and the name a sound is chosen by.
struct RagdollBody {
    RE::hkRefPtr<RE::hkpRigidBody> body;

    /// The Havok body's own name - in Skyrim's skeletons, the bone it was authored
    /// on ("NPC L Forearm [LLar]"). What makes a per-site sound set possible, and
    /// why LimbSite is resolved from this and never from the index (07 §7).
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
/// Takes the graph's update lock: the graph is rebuilt on 3D load, and a torn read
/// there is a dangling body pointer rather than a wrong number. Bodies come back
/// held by `hkRefPtr`, but one whose world has gone still has to be re-checked -
/// see `hkpWorldObject::world`.
[[nodiscard]] RagdollView CaptureRagdoll(RE::Actor& actor);

/// True when every limb still belongs to a physics world. False means the ragdoll
/// was rebuilt under us - cell change, 3D reload, or six times in three seconds on
/// a disturbed standing actor. The answer is to re-capture, not to give up.
[[nodiscard]] bool BodiesLive(const RagdollView& view);

}  // namespace rds::game
