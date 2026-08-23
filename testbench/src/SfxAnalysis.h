#pragma once

// Measuring an sfx, and deciding what it is good for.
//
// This is `tools/sfx.py`'s `measure()` in C++, over the same definitions, so a
// number the browser shows can be compared against the tables in Slots.md and
// against 03-Asset-Status.md without translating anything. Where the two could
// have drifted they are noted in SfxAnalysis.cpp.
//
// Two deliberate differences from the python:
//
//   - Nothing is high-passed first. sfx.py judges character *after the slot's
//     own high-pass* because that is what ships, but a library file has no slot
//     yet - it is a candidate for all of them. So tilt here is the file's own,
//     which reads a little more bass-led than the same file's shipped tilt.
//   - The FFT is radix-2 and zero-pads to the next power of two. Band energies
//     are still exact (Parseval does not care about trailing zeros); the
//     centroid is interpolated, which matters not at all for a number Slots.md
//     §4 calls a loose sanity bound.
//
// Testbench-only. The game never measures anything - it reads what the importer
// wrote into the sidecar.

#include <span>
#include <string>
#include <vector>

#include "rds/Sfx.h"
#include "rds/SlotManifest.h"

namespace tb {

/// Fill every measured field of `entry` from `mono`, and set `loops`.
///
/// `entry.warnings` and `entry.suggested` are left alone: measuring and judging
/// are separate so a file can be re-judged after the slot targets change
/// without decoding it again.
void MeasureSfx(std::span<const float> mono, int sampleRate, rds::SfxEntry& entry);

/// What the file measures against the design's delivery rules (Slots.md §5).
///
/// Replaces `entry.warnings` wholesale. Everything here is advisory: the only
/// warning marked blocking is one the caller adds when the file would not
/// decode at all, which is the single case where "it still plays, just worse"
/// is not true.
void JudgeSfx(rds::SfxEntry& entry);

/// Slots this would suit, best first. At most three - a longer list is not a
/// suggestion any more.
[[nodiscard]] std::vector<rds::SlotId> SuggestSlots(const rds::SfxEntry& entry);

/// How well `entry` fits `slot`, 0 (unusable) to 1 (every target met). Used by
/// the browser to sort candidates when the window is opened for a slot.
[[nodiscard]] float SlotFit(const rds::SfxEntry& entry, rds::SlotId slot);

/// True when the duration is inside the slot's declared range widened by 25%
/// either way - the band the browser sorts to the top when picking for a slot.
[[nodiscard]] bool LengthSuits(float durationMs, rds::SlotId slot, bool slotLoops);

/// `punch-face-hard-3(fromnoisetosound.com)` to `punch-face-hard-3`.
///
/// Every generator and sample site stamps its own name into the download, and
/// the stamp is never part of what the sound is. Strips bracketed groups, the
/// separators they leave behind, and a trailing copy counter of the `(2)` kind.
[[nodiscard]] std::string TidyName(std::string_view stem);

}  // namespace tb
