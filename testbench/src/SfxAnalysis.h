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

/// What the repair pass fixes, and therefore what JudgeSfx does not have to.
///
/// One place for both because the two must not disagree: a threshold the repair
/// acts on and the judge also warns about is a badge that never clears, and one
/// neither of them owns is a fault that never shows. Every number here is
/// Slots.md §5.
namespace sfxrepair {

/// Slots.md §3: -1.5 dBFS everywhere, -1.0 for `imp_sub`.
constexpr float kPeakTargetDb = -1.5f;

/// The band a file is left alone in. It holds both pack targets, so adopting the
/// built pack does not renormalise `imp_sub` off its own spec, and it is narrow
/// enough that everything else is pulled onto -1.5.
constexpr float kPeakHotDb = -0.9f;
constexpr float kPeakQuietDb = -2.1f;

/// Under this there is nothing to normalise *to* - a file this far down is
/// silence with a noise floor, and lifting it 50 dB lifts the noise floor. It is
/// reported instead.
constexpr float kPeakFloorDb = -45.0f;

/// Head silence over this is trimmed. The cue time is the attack, so anything
/// in front of it is latency.
constexpr float kLeadInMs = 20.0f;

/// Trailing silence over this is trimmed, leaving `kTailGuardMs` of it. Not the
/// room tail - that is a judgement and stays a warning - only the digital
/// silence a bad export leaves behind.
constexpr float kTrailingMs = 50.0f;
constexpr float kTailGuardMs = 20.0f;

/// DC over this is subtracted.
constexpr float kDcOffset = 0.005f;

/// The end fade, when a one-shot stops dead rather than ending on one. `sfx.py
/// make`'s own 6 ms of cos^2, applied when the last few samples are still above
/// `kEndFadeRatio` of the file's peak - which is a discontinuity, where a level
/// threshold would also catch every file that ends on a specified quiet tail.
constexpr float kEndFadeMs = 6.0f;
/// 3% of the peak, which is a real cut rather than a tail that did not quite
/// reach zero. The pack's own one-shots come out of `make`'s fade at 0.0-2.3%,
/// because the high-pass and the resample after it ring past the fade - so a
/// tighter line re-fades files that verify_pack.py passes, and a discontinuity
/// under 3% is a click 30 dB down that nothing will hear under an impact.
constexpr float kEndFadeRatio = 0.03f;

/// Stereo correlation under this takes the left channel instead of folding both
/// - 03-Asset-Status.md §3.1, which is `make`'s own fix. Under `kChannelsDead`
/// the two are different recordings and the left alone is not what was picked.
constexpr float kCorrelationFold = 0.6f;
constexpr float kCorrelationDead = 0.45f;

}  // namespace sfxrepair

/// Fill every measured field of `entry` from `mono`, and set `loops`.
///
/// `entry.warnings` and `entry.suggested` are left alone: measuring and judging
/// are separate so a file can be re-judged after the slot targets change
/// without decoding it again.
void MeasureSfx(std::span<const float> mono, int sampleRate, rds::SfxEntry& entry);

/// What the file measures against the design's delivery rules (Slots.md §5).
///
/// Replaces `entry.warnings` wholesale, keeping only the blocking one the caller
/// adds when the file would not decode at all.
///
/// Two severities, and what separates them is whether anything can be done:
/// everything advisory has a fix and names it, while `dead` is the list nothing
/// recovers - a squared-off waveform, a noise floor inside 30 dB of the hero,
/// two channels that are different takes. Neither stops a file being assigned.
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
