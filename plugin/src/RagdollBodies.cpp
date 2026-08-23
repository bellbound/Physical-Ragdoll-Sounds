#include "RagdollBodies.h"

#include <cstddef>
#include <format>

namespace rds::game {
namespace {

[[nodiscard]] std::string TextOf(const RE::hkStringPtr& text) {
    const auto* data = text.c_str();
    return data != nullptr ? std::string(data) : std::string{};
}

/// Fills `view` from one animation graph, or leaves it alone and says no.
///
/// An actor can carry several graphs - the third-person one, a furniture one, and
/// in VR the first-person player rig. Only one of them owns a ragdoll at a time,
/// so the caller tries the active graph first and then the rest.
[[nodiscard]] bool CaptureGraph(RE::BSAnimationGraphManager& manager, std::size_t index,
                                RagdollView& view) {
    if (index >= manager.graphs.size()) {
        return false;
    }
    const auto& graph = manager.graphs[index];
    if (!graph || !graph->physicsWorld) {
        return false;
    }

    const auto driver = graph->characterInstance.ragdollDriver;
    const auto* ragdoll = driver ? driver->ragdoll : nullptr;
    if (ragdoll == nullptr || ragdoll->rigidBodies.empty()) {
        return false;
    }

    const float scale = RE::bhkWorld::GetWorldScaleInverse();

    std::vector<RagdollBody> limbs;
    limbs.reserve(static_cast<std::size_t>(ragdoll->rigidBodies.size()));
    for (std::int32_t index2 = 0; index2 < ragdoll->rigidBodies.size(); ++index2) {
        auto* body = ragdoll->rigidBodies[index2];
        if (body == nullptr) {
            continue;
        }
        RagdollBody limb;
        limb.body = RE::hkRefPtr<RE::hkpRigidBody>{body};
        limb.name = TextOf(body->name);
        if (limb.name.empty()) {
            // An unnamed body would resolve to LimbSite::kUnknown, which still
            // sounds - sized off the radius - rather than going silent.
            limb.name = std::format("body{}", index2);
        }
        limb.mass = body->motion.GetMass();
        limb.radius = body->motion.motionState.objectRadius * scale;
        limbs.push_back(std::move(limb));
    }

    if (limbs.empty()) {
        return false;
    }

    view.world = RE::NiPointer<RE::bhkWorld>{graph->physicsWorld};
    view.limbs = std::move(limbs);
    return true;
}

}  // namespace

RagdollView CaptureRagdoll(RE::Actor& actor) {
    RagdollView view;

    RE::BSAnimationGraphManagerPtr manager;
    if (!actor.GetAnimationGraphManager(manager) || !manager) {
        return view;
    }

    RE::BSSpinLockGuard graphLock{manager->GetRuntimeData().updateLock};

    const auto active = static_cast<std::size_t>(manager->GetRuntimeData().activeGraph);
    if (CaptureGraph(*manager, active, view)) {
        return view;
    }
    for (std::size_t index = 0; index < manager->graphs.size(); ++index) {
        if (index != active && CaptureGraph(*manager, index, view)) {
            break;
        }
    }
    return view;
}

bool BodiesLive(const RagdollView& view) {
    if (view.limbs.empty()) {
        return false;
    }
    for (const RagdollBody& limb : view.limbs) {
        if (limb.body && limb.body->world != nullptr) {
            return true;
        }
    }
    return false;
}

}  // namespace rds::game
