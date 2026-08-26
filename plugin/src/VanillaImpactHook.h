#pragma once

// Vanilla's body impacts, silenced one call at a time - and written down on the
// way past.
//
// VanillaSuppression.h nulls `sound1`/`sound2` on the eight-odd `BGSImpactData`
// records the body sets reach. That works, but it is a *global form edit* with
// two costs its own header apologises for: a set we do not own that happens to
// share a record goes quiet too (all that file can do is warn), and any other mod
// reading those pointers finds them null. It also throws away the thing we most
// want, because a record with no sounds never reaches the play path and there is
// then nothing to observe.
//
// This hooks the play instead. One call site, one filter, nothing mutated.
//
// The filter is two questions, and they are different in kind. `IsBodyImpact`
// asks whether the record is one of ours to have an opinion about at all;
// VanillaGate.h asks whether *this* play is one we are replacing - an actor of
// ours ragdolling or getting up where it happened. Only both together drop a
// sound, so vanilla keeps everything we are not standing on.
//
// ── where the hook goes ──────────────────────────────────────────────────────
//
// `BGSImpactManager::PlayImpactDataSounds` is reached from nine places - footstep
// and combat-impact event handlers, `PlayImpactEffect` twice, explosions, weapon
// hits. Exactly **one** of them is the ragdoll/collision path: the per-pair
// helper that `ProcessEvent(BGSCollisionSoundEvent*)` calls once for "A hit B"
// and once for "B hit A". Hooking that one call rather than the function entry
// means every other consumer of the impact system is untouched by construction,
// and it needs no detour library - it is the same `write_call<5>` over an
// existing `E8` that FrameHook.h already uses.
//
// The site is verified twice before anything is written: the opcode must be
// `E8`, and its target must resolve to `PlayImpactDataSounds` itself. The second
// check is what makes an offset from an anchor safe - a patch that moves things
// cannot leave us pointed at some other function, only at nothing.
//
// The runtime offsets are from the address libraries and the binaries:
//
//     VR 1.4.15   PlayImpactDataSounds 0x5A9FF0, call site 0x5ABF54  (+0x1F64)
//     SE / AE     not resolved - see kCallSiteFromAnchor
//
// Where an offset is not known, a signature scan looks for the enclosing
// function's prologue instead; that prologue is unique in VR's `.text`, and if it
// is unique on another runtime too then that runtime is covered without anybody
// having to find an address. When neither works the hook does not install, says
// so in the log, and VanillaSuppression's nulling is left in charge.
//
// ── what it reads ────────────────────────────────────────────────────────────
//
// `ImpactSoundData` carries the record, the position, and vanilla's own
// light/heavy decision as `playSound1`/`playSound2` - which the caller computed
// as `sound2 != null && magnitude >= threshold`, so the flag *is* the decision.
// That plus the firing descriptor's playback characteristics is everything a
// take needs to say what vanilla would have sounded like, short of the wav draw
// and the dB roll that happen later inside BSAudioManager (see
// `rds::VanillaSoundInfo` for why those two are not here).
//
// ── the return value matters ─────────────────────────────────────────────────
//
// `ProcessEvent` records the ref pair in a cooldown map only when one of its two
// calls returns true. Returning false while suppressing would leave the throttle
// unarmed, so the same pair would re-resolve sooner than it does in vanilla and
// the recording would be denser than the thing it is a recording of. So a
// suppressed call still answers what vanilla would have answered.

#include <cstdint>

#include "rds/Feed.h"

namespace rds::game {

/// Where an observed play goes. Called on whatever thread the impact manager is
/// on, so the sink must be as safe as the Havok contact callback's is - in
/// practice, a push into the same lock-free ring.
using VanillaSoundSink = void (*)(const FeedEvent&);

/// Write the hook. Call after BuildBodyImpactIndex and before anything can
/// collide, with the trampoline already allocated (plugin.cpp does it once for
/// both hooks). False if no verified call site was found, in which case nothing
/// was patched and the caller should fall back to nulling and say so.
[[nodiscard]] bool InstallVanillaImpactHook(VanillaSoundSink sink);

[[nodiscard]] bool VanillaImpactHookInstalled();

/// May vanilla's body impacts be dropped at all - the ini's switch and the
/// testbench's A/B. It is a permission, not the decision: which plays are
/// actually dropped is VanillaGate.h's, and one that no actor of ours is
/// standing on goes through with this on.
///
/// Observation happens either way - which is the point: the A/B switch changes
/// what is *heard*, not what is recorded, so a take made in vanilla mode and a
/// take made in ours carry the same vanilla track.
void SetVanillaImpactsSuppressed(bool suppressed);

[[nodiscard]] bool VanillaImpactsSuppressed();

/// How many body impacts have been seen and how many were dropped, for the
/// status line. Cheap relaxed loads; exact counts are not the point.
void VanillaImpactCounters(std::uint64_t& seen, std::uint64_t& suppressed);

}  // namespace rds::game
