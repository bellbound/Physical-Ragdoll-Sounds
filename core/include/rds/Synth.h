#pragma once

// Procedural stand-ins for the slots with no wav behind them.
//
// None of the 29 authored files exist yet, so a slot resolves either to a file
// or to a stand-in generated from the slot's own brief. That is not a placeholder
// beep: the briefs in 02-SFX-Generation-Prompts.md are specific enough to
// synthesise against, and the sub sweep in particular is fully specified by
// 01 §1 - ~150 Hz to 45 Hz over 60 ms, into a 20-30 Hz floor by 180 ms. Building
// it makes the timing, the layer balance and the gates audible today, which is
// the only way to tune any of them before the assets land.
//
// Dropping a wav into the bank overrides its slot with no code change, and the
// stand-in is deliberately aimed at the same place in the mix as the file that
// will replace it, so the tuning survives the swap.
//
// The testbench renders a ResolvedSound through here; the game never calls it,
// because the game plays files off disk.

#include <cstdint>
#include <vector>

#include "rds/SlotManifest.h"

namespace rds {

/// Mono float PCM, which is what both the references and the asset spec are:
/// the clips measure 0.95-0.97 stereo correlation, so they are point sources
/// with reverb width and nothing is lost by being mono.
struct SynthBuffer {
    std::vector<float> samples;
    float sampleRate{44100.0f};

    [[nodiscard]] float LengthMs() const {
        return sampleRate > 0.0f ? 1000.0f * static_cast<float>(samples.size()) / sampleRate : 0.0f;
    }
};

/// Render one slot variant. Deterministic in (slot, variant, lengthMs), so the
/// same cue list mixes to the same audio twice - which is what makes an offline
/// A/B compare two configs rather than two dice rolls.
[[nodiscard]] SynthBuffer Synthesise(SlotId slot, std::uint8_t variant, float lengthMs,
                                     float sampleRate = 44100.0f);

/// The same, for a slot the bank has already resolved. A `ResolvedSound` that
/// names a real file is not this function's business - the caller plays the file.
[[nodiscard]] SynthBuffer Synthesise(const ResolvedSound& sound, float sampleRate = 44100.0f);

}  // namespace rds
