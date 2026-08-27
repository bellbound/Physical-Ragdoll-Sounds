#pragma once

// Silencing vanilla's own ragdoll body impacts.
//
// Vanilla plays a body impact on every ragdoll contact and resolves nearly every
// surface to the same dirt sample - `PHYBodyMedium` maps 60 materials onto three
// sounds, the other body sets 57-75 materials onto exactly one (08 §5). Layered
// under ours, everything doubles.
//
// **Method.** `BGSImpactData` holds its two sounds as plain
// `BGSSoundDescriptorForm*` members (`sound1` from SNAM, `sound2` from NAM1).
// Walk the body impact sets, collect every distinct `BGSImpactData` they map to -
// about eight records - and null those pointers once, at data load.
//
// **This is the fallback, not the plan.** VanillaImpactHook.h suppresses the same
// sounds per *call*, which mutates nothing and also observes vanilla's decision
// for the recording. Nulling stays for the runtime where the hook cannot be
// placed.
//
// **Consequences, and they belong in the mod description.** It is global: it
// silences body-material impacts beyond ragdolls too - a dragged corpse, a thrown
// severed head - which is fine, since we take those over anyway. And any other
// mod expecting those descriptors loses them.

#include <cstdint>
#include <string_view>

namespace rds::game {

/// Collect the body impact records without touching them. Split out of
/// SuppressVanillaBodyImpacts because the hook needs the same set as a *filter*
/// and must not silence anything to get it. Idempotent; call once at data load,
/// before the hook is installed.
///
/// Nothing writes this index afterwards, which is what makes it safe to read
/// from the impact manager's thread.
void BuildBodyImpactIndex();

/// Is this one of the records the body impact sets reach? The hook's filter, so a
/// weapon hit or explosion sharing a record is left alone - the thing nulling
/// could only warn about.
[[nodiscard]] bool IsBodyImpact(const RE::BGSImpactData* impact);

/// The editor id we saw on that record at data load, or "?". Looked up here
/// rather than through GetFormEditorID at the moment of a play, which walks a
/// cache while the hook runs on the impact manager's thread.
[[nodiscard]] std::string_view BodyImpactEditorId(const RE::BGSImpactData* impact);

/// Null the sound pointers on the body impact records. Idempotent. Logs every
/// form it touches at info, so a user wondering where their impact sounds went
/// finds the answer in the log.
void SuppressVanillaBodyImpacts();

/// Put back what SuppressVanillaBodyImpacts took, for a runtime toggle. The
/// originals are kept in a table rather than re-read from the file, since another
/// mod may have replaced them between load and now.
void RestoreVanillaBodyImpacts();

}  // namespace rds::game
