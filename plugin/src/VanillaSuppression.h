#pragma once

// Silencing vanilla's own ragdoll body impacts.
//
// STUBBED.
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
// Why this rather than a hook: no addresses, no thread concerns, decals and
// hazards keep working, and form-data edits like this live in memory only - they
// never reach a save file.
//
// **Consequences to accept, and they belong in the mod description.** It is
// global: it silences body-material impacts beyond ragdolls too - a dragged
// corpse, a thrown severed head - which is fine, because we take those over
// anyway. And any other mod expecting those descriptors loses them. Both are the
// right trade for owning the mix.

#include <cstdint>

namespace rds::game {

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
