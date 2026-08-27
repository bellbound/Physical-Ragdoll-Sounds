#pragma once

// Where the mod owns vanilla's body impacts, published per tick and asked per
// play.
//
// Suppression is not a session-wide switch. Vanilla plays body impacts far
// outside a knockdown - an actor's ragdoll bodies are in contact with the floor
// the whole time they are on their feet, the very thing GameFeed's phase gate
// keeps quiet on our side - so dropping those left the game quieter than vanilla
// with nothing put back.
//
// So the drop is *claimed*, per actor, by the same tick that publishes the phase:
//
//   ragdoll   we are playing this collision ourselves, so vanilla's is a double
//   get-up    we play nothing here, but the blend from simulation back to
//             animation drags the ragdoll bodies through contacts that are not
//             collisions, and vanilla renders that as a burst of body impacts
//             that no fall produced
//   animated  claimed only under `GameIntegration:bAnimatedMode`, which is the
//             one setting that makes us play an animated actor's contacts
//
// Everything else keeps vanilla's sound: the NPC walking past, the clutter, the
// corpse outside the cull radius, any impact set we do not own at all.
//
// ── why a position and not a reference ───────────────────────────────────────
//
// `ImpactSoundData` carries the record, a world point and vanilla's light/heavy
// decision, and nothing else. On the collision path `objectToFollow` is null and
// the two refs live in the arguments of the enclosing per-pair helper, which the
// patched call cannot see. The world point is the only thing the tick and the
// play have in common.
//
// ── threading ────────────────────────────────────────────────────────────────
//
// Written by the game thread in GameFeed::PublishTick, read on whatever thread
// the impact manager is on. Fixed storage and atomic scalars, so the reader
// cannot be led out of the array and never waits on the writer. It can judge an
// impact against a position half a tick old, which is the coarseness the phase
// gate already accepts.

#include <cstddef>

namespace rds::game {

/// Begin a tick's publication. Game thread only. Between this and the commit the
/// gate reads as empty, so an impact landing mid-update is left to vanilla.
void BeginVanillaGate();

/// Claim everything happening around `x, y, z` for this tick.
void AddVanillaGate(float x, float y, float z);

/// Publish what was claimed.
void CommitVanillaGate();

/// Drop every claim. Load screen, cell change, and any other point where the
/// tracked set goes away without a tick to say so.
void ClearVanillaGate();

/// How far from a claiming actor an impact still counts as theirs, in world
/// units. Read once at data load; see SuppressionConfig::suppressionRadius.
void SetVanillaGateRadius(float units);

/// Is that world point somebody's? The whole question the hook asks.
[[nodiscard]] bool VanillaGateCovers(float x, float y, float z);

}  // namespace rds::game
