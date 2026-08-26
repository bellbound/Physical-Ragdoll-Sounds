#pragma once

// Silencing vanilla's own ragdoll body impacts.
//
// Vanilla plays a body impact on every ragdoll contact and resolves nearly every
// surface in the game to the same dirt sample - `PHYBodyMedium` maps 60
// materials onto three sounds, and the other body sets map 57-75 materials onto
// exactly one (08 §5). Layered under ours, everything doubles and half the mix is
// out of our control. So we silence it.
//
// **Method.** `BGSImpactData` holds its two sounds as plain
// `BGSSoundDescriptorForm*` members - `sound1` from SNAM, `sound2` from NAM1.
// Walk the body impact *sets* - PHYBodyMedium, PHYBodyLarge, PHYBodySmall,
// PHYBodyBones, PHYBodyMetalLarge, PHYBodyMetalSmall, plus the armour and meat
// sets - collect every distinct `BGSImpactData` they map to (about eight records,
// since those sets map dozens of materials onto one to three impacts each) and
// null those two pointers once, at data load.
//
// **This is now the fallback, not the plan.** VanillaImpactHook.h suppresses the
// same sounds per *call* instead, which mutates nothing and costs no other mod
// its descriptors - and, being at the moment of the play, is also what observes
// vanilla's decision for the recording. Nulling stays for the runtime where the
// hook cannot be placed, because a load order with vanilla underneath ours is
// worse than either.
//
// **Consequences of the fallback, and they belong in the mod description.** It is
// global: it silences body-material impacts beyond ragdolls too - a dragged
// corpse, a thrown severed head - which is fine, because we take those over
// anyway. And any other mod expecting those descriptors loses them. The hook has
// neither cost, which is the whole reason it is preferred.

#include <cstdint>
#include <string_view>

namespace rds::game {

/// Collect the body impact records without touching them.
///
/// Split out of SuppressVanillaBodyImpacts because the hook needs the same set
/// as a *filter* and must not silence anything to get it. Idempotent; call once
/// at data load, before the hook is installed.
///
/// Everything below reads this index and nothing writes it afterwards, which is
/// what makes it safe to read from whatever thread the impact manager is on.
void BuildBodyImpactIndex();

/// Is this one of the records the body impact sets reach? The hook's filter, so
/// a weapon hit or an explosion that happens to share a record is left alone -
/// which is the thing nulling could only warn about.
[[nodiscard]] bool IsBodyImpact(const RE::BGSImpactData* impact);

/// The editor id we saw on that record at data load, or "?" - looked up here
/// rather than through GetFormEditorID at the moment of a play, because that
/// walks a cache and the hook runs on the impact manager's thread.
[[nodiscard]] std::string_view BodyImpactEditorId(const RE::BGSImpactData* impact);

/// Null the sound pointers on the body impact records. Idempotent; safe to call
/// again after a load. Logs every form it touches at info, because a user
/// wondering where their impact sounds went should find the answer in the log
/// rather than in a forum thread.
void SuppressVanillaBodyImpacts();

/// Put back what SuppressVanillaBodyImpacts took, for a runtime toggle. The
/// originals are kept in a table beside the flag rather than re-read from the
/// file, since another mod may have replaced them between load and now.
void RestoreVanillaBodyImpacts();

}  // namespace rds::game
