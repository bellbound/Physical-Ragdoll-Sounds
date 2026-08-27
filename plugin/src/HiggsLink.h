#pragma once

// Whether the player is holding a body, and nothing else.
//
// The mod asks HIGGS one question - "is this actor in the player's hands" - so
// this is the smallest interface that can answer it. The vtable is not: a virtual
// interface is an ABI and the entries must be present and in order up to the one
// being called, so it is transcribed whole from
// skse/3DUI/src/higgsinterface001.h and only `GetGrabbedObject` is used.
//
// **VR only.** HIGGS does not exist on flat Skyrim, so `Acquire` is a no-op there
// and `Available()` stays false - which makes `AccumDamage:bRequireHeld` honest on
// both runtimes: a build that cannot see a hold reports no hold.

#include <cstdint>

namespace rds::game::higgs {

/// Ask HIGGS for its interface. Call once, on `kPostPostLoad` and no earlier -
/// that is the message HIGGS is ready to answer on. Does nothing outside VR.
void Acquire(const SKSE::MessagingInterface* messaging);

/// Whether there is a HIGGS to ask. False on flat Skyrim, and false in VR when
/// HIGGS is not installed.
[[nodiscard]] bool Available();

/// The form id of whatever actor the player has in that hand, or 0 - for an empty
/// hand, a grabbed non-actor, and every call when `Available()` is false. Game
/// thread only: it dereferences a TESObjectREFR.
[[nodiscard]] std::uint32_t HeldActorId(bool isLeft);

}  // namespace rds::game::higgs
