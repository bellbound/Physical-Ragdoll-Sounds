#include "HiggsLink.h"

#include <spdlog/spdlog.h>

namespace rds::game::higgs {
namespace {

/// activeragdoll's `IHiggsInterface001`, transcribed whole.
///
/// Only `GetGrabbedObject` is called. Every entry above it is here because a
/// virtual interface is an ABI: the vtable slot is decided by position, so an
/// abridged copy calls whichever method happens to sit at that index in the real
/// one. Kept in the order 3DUI's copy has it, which is the order HIGGS publishes.
struct IHiggsInterface001 {
    virtual unsigned int GetBuildNumber() = 0;

    using PulledCallback = void (*)(bool isLeft, RE::TESObjectREFR* pulledRefr);
    virtual void AddPulledCallback(PulledCallback callback) = 0;

    using GrabbedCallback = void (*)(bool isLeft, RE::TESObjectREFR* grabbedRefr);
    virtual void AddGrabbedCallback(GrabbedCallback callback) = 0;

    using DroppedCallback = void (*)(bool isLeft, RE::TESObjectREFR* droppedRefr);
    virtual void AddDroppedCallback(DroppedCallback callback) = 0;

    using StashedCallback = void (*)(bool isLeft, RE::TESForm* stashedForm);
    virtual void AddStashedCallback(StashedCallback callback) = 0;

    using ConsumedCallback = void (*)(bool isLeft, RE::TESForm* consumedForm);
    virtual void AddConsumedCallback(ConsumedCallback callback) = 0;

    using CollisionCallback = void (*)(bool isLeft, float mass, float separatingVelocity);
    virtual void AddCollisionCallback(CollisionCallback callback) = 0;

    virtual void GrabObject(RE::TESObjectREFR* object, bool isLeft) = 0;
    virtual RE::TESObjectREFR* GetGrabbedObject(bool isLeft) = 0;
    virtual bool IsHandInGrabbableState(bool isLeft) = 0;

    virtual void DisableHand(bool isLeft) = 0;
    virtual void EnableHand(bool isLeft) = 0;
    virtual bool IsDisabled(bool isLeft) = 0;

    virtual void DisableWeaponCollision(bool isLeft) = 0;
    virtual void EnableWeaponCollision(bool isLeft) = 0;
    virtual bool IsWeaponCollisionDisabled(bool isLeft) = 0;

    virtual bool IsTwoHanding() = 0;

    using StartTwoHandingCallback = void (*)();
    virtual void AddStartTwoHandingCallback(StartTwoHandingCallback callback) = 0;

    using StopTwoHandingCallback = void (*)();
    virtual void AddStopTwoHandingCallback(StopTwoHandingCallback callback) = 0;

    virtual bool CanGrabObject(bool isLeft) = 0;

    enum class CollisionFilterComparisonResult : std::uint8_t {
        Continue,
        Collide,
        Ignore,
    };
    using CollisionFilterComparisonCallback = CollisionFilterComparisonResult (*)(
        void* collisionFilter, std::uint32_t filterInfoA, std::uint32_t filterInfoB);
    virtual void AddCollisionFilterComparisonCallback(
        CollisionFilterComparisonCallback callback) = 0;

    using PrePhysicsStepCallback = void (*)(void* world);
    virtual void AddPrePhysicsStepCallback(PrePhysicsStepCallback callback) = 0;

    virtual std::uint64_t GetHiggsLayerBitfield() = 0;
    virtual void SetHiggsLayerBitfield(std::uint64_t bitfield) = 0;

    virtual RE::NiObject* GetHandRigidBody(bool isLeft) = 0;
    virtual RE::NiObject* GetWeaponRigidBody(bool isLeft) = 0;
    virtual RE::NiObject* GetGrabbedRigidBody(bool isLeft) = 0;

    virtual void ForceWeaponCollisionEnabled(bool isLeft) = 0;
    virtual bool IsHoldingObject(bool isLeft) = 0;
    virtual void GetFingerValues(bool isLeft, float values[5]) = 0;

    using NoArgCallback = void (*)();
    virtual void AddPreVrikPreHiggsCallback(NoArgCallback callback) = 0;
    virtual void AddPreVrikPostHiggsCallback(NoArgCallback callback) = 0;
    virtual void AddPostVrikPreHiggsCallback(NoArgCallback callback) = 0;
    virtual void AddPostVrikPostHiggsCallback(NoArgCallback callback) = 0;

    virtual bool Deprecated1(const std::string_view& name, double& out) = 0;
    virtual bool Deprecated2(const std::string& name, double val) = 0;

    virtual RE::NiTransform GetGrabTransform(bool isLeft) = 0;
    virtual void SetGrabTransform(bool isLeft, const RE::NiTransform& transform) = 0;

    virtual bool GetSettingDouble(const char* name, double& out) = 0;
    virtual bool SetSettingDouble(const char* name, double val) = 0;
};

/// HIGGS answers a dispatched message by filling in a function pointer. The
/// message id is HIGGS's own randomly generated one and is not ours to choose.
struct HiggsMessage {
    enum { kQueryInterface = 0xF9279A57 };
    void* (*GetApiFunction)(unsigned int revisionNumber) = nullptr;
};

IHiggsInterface001* g_higgs = nullptr;

}  // namespace

void Acquire(const SKSE::MessagingInterface* messaging) {
    if (g_higgs != nullptr) {
        return;
    }
    if (!REL::Module::IsVR()) {
        // Not a failure and not worth a warning: there is no HIGGS on flat
        // Skyrim and nothing here is meant to work without VR.
        return;
    }
    if (messaging == nullptr) {
        spdlog::warn("higgs: no messaging interface; holds will never be seen");
        return;
    }

    HiggsMessage message;
    messaging->Dispatch(HiggsMessage::kQueryInterface, &message, sizeof(HiggsMessage*), "HIGGS");
    if (message.GetApiFunction == nullptr) {
        spdlog::info("higgs: not installed, or it did not answer. "
                     "DamageAccum:bRequireHeld will never open");
        return;
    }

    g_higgs = static_cast<IHiggsInterface001*>(message.GetApiFunction(1));
    if (g_higgs == nullptr) {
        spdlog::warn("higgs: answered but handed back no interface 001");
        return;
    }
    spdlog::info("higgs: interface 001 acquired (build {})", g_higgs->GetBuildNumber());
}

bool Available() { return g_higgs != nullptr; }

std::uint32_t HeldActorId(bool isLeft) {
    if (g_higgs == nullptr) {
        return 0;
    }
    RE::TESObjectREFR* grabbed = g_higgs->GetGrabbedObject(isLeft);
    if (grabbed == nullptr) {
        return 0;
    }
    // A held cabbage is not a held body. Everything downstream of this asks about
    // an actor's limbs, so anything that is not an actor is the same as nothing.
    return grabbed->As<RE::Actor>() != nullptr ? grabbed->GetFormID() : 0;
}

}  // namespace rds::game::higgs
