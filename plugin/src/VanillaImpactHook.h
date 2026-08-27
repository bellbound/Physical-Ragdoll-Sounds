#pragma once

// Vanilla's body impacts, silenced one call at a time - and written down on the
// way past.
//
// VanillaSuppression.h nulls `sound1`/`sound2` on the records the body sets
// reach. That is a *global form edit*: a set we do not own that shares a record
// goes quiet too, any other mod reading those pointers finds them null, and a
// record with no sounds never reaches the play path so there is nothing to
// observe. This hooks the play instead - one call site, one filter, nothing
// mutated.
//
// The filter is two questions of different kinds. `IsBodyImpact` asks whether the
// record is one of ours to have an opinion about; VanillaGate.h asks whether
// *this* play is one we are replacing. Only both together drop a sound.
//
// ── where the hook goes ──────────────────────────────────────────────────────
//
// `BGSImpactManager::PlayImpactDataSounds` is reached from nine places, and
// exactly **one** is the ragdoll/collision path: the per-pair helper
// `ProcessEvent(BGSCollisionSoundEvent*)` calls once for "A hit B" and once for
// "B hit A". Hooking that call rather than the function entry leaves every other
// consumer untouched by construction, and needs no detour library - the same
// `write_call<5>` over an existing `E8` that FrameHook.h uses.
//
// The site is verified twice before anything is written: the opcode must be `E8`
// and its target must resolve to `PlayImpactDataSounds`. That second check is
// what makes an offset from an anchor safe - a patch that moves things can leave
// us pointed at nothing, never at some other function.
//
//     VR 1.4.15   PlayImpactDataSounds 0x5A9FF0, call site 0x5ABF54  (+0x1F64)
//     SE / AE     not resolved - see kCallSiteFromAnchor
//
// Where an offset is unknown, a signature scan looks for the enclosing function's
// prologue, which is unique in VR's `.text` and may be on other runtimes too.
// When neither works the hook does not install, says so, and nulling takes over.
//
// ── what it reads ────────────────────────────────────────────────────────────
//
// `ImpactSoundData` carries the record, the position and vanilla's own
// light/heavy decision as `playSound1`/`playSound2` - computed by the caller as
// `sound2 != null && magnitude >= threshold`, so the flag *is* the decision. That
// plus the descriptor's playback characteristics is everything a take needs,
// short of the wav draw and dB roll inside BSAudioManager (see
// `rds::VanillaSoundInfo`).
//
// ── the return value matters ─────────────────────────────────────────────────
//
// `ProcessEvent` records the ref pair in a cooldown map only when one of its two
// calls returns true, so returning false while suppressing would leave the
// throttle unarmed and the recording denser than the thing it records. A
// suppressed call still answers what vanilla would have answered.

#include <cstdint>

#include "rds/Feed.h"

namespace rds::game {

/// Where an observed play goes. Called on whatever thread the impact manager is
/// on, so the sink must be as safe as the Havok contact callback's.
using VanillaSoundSink = void (*)(const FeedEvent&);

/// Write the hook. Call after BuildBodyImpactIndex and before anything can
/// collide, with the trampoline already allocated. False if no verified call site
/// was found, in which case nothing was patched and the caller should fall back
/// to nulling and say so.
[[nodiscard]] bool InstallVanillaImpactHook(VanillaSoundSink sink);

[[nodiscard]] bool VanillaImpactHookInstalled();

/// May vanilla's body impacts be dropped at all - the ini's switch and the
/// testbench's A/B. A permission, not the decision: which plays are dropped is
/// VanillaGate.h's. Observation happens either way, so a take made in vanilla
/// mode and one made in ours carry the same vanilla track.
void SetVanillaImpactsSuppressed(bool suppressed);

[[nodiscard]] bool VanillaImpactsSuppressed();

/// How many body impacts have been seen and how many were dropped, for the
/// status line. Cheap relaxed loads; exact counts are not the point.
void VanillaImpactCounters(std::uint64_t& seen, std::uint64_t& suppressed);

}  // namespace rds::game
