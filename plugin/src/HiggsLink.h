#pragma once

// Whether the player is holding a body, and nothing else.
//
// The mod asks HIGGS exactly one question - "is this actor in the player's
// hands right now" - so this is the smallest interface that can answer it
// rather than a copy of the whole API. The vtable below is not: a virtual
// interface is an ABI, and the entries have to be present and in order up to the
// one being called or the call lands somewhere else entirely. So it is
// transcribed whole from skse/3DUI/src/higgsinterface001.h, which is
// activeragdoll's, and only `GetGrabbedObject` is ever used.
//
// **VR only.** HIGGS does not exist on flat Skyrim, so `Acquire` is a no-op
// there and `Available()` stays false - which makes
// `AccumDamage:bRequireHeld` an honest switch on both runtimes: it says
// *require*, and a build that cannot see a hold reports no hold rather than
// falling back to "always held".

#include <cstdint>

namespace rds::game::higgs {

/// Ask HIGGS for its interface. Call once, on `kPostPostLoad` and no earlier -
/// that is the message HIGGS is ready to answer on. Does nothing outside VR.
void Acquire(const SKSE::MessagingInterface* messaging);

/// Whether there is a HIGGS to ask. False on flat Skyrim, and false in VR when
/// HIGGS is not installed.
[[nodiscard]] bool Available();

/// The form id of whatever actor the player has in that hand, or 0.
///
/// 0 for an empty hand, for a grabbed object that is not an actor, and for every
/// call at all when `Available()` is false - one answer for "nothing is held
/// there", because every one of those cases means the same thing to the rule
/// that reads it.
///
/// Game thread only: it dereferences a TESObjectREFR.
[[nodiscard]] std::uint32_t HeldActorId(bool isLeft);

}  // namespace rds::game::higgs
