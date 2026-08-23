#pragma once

// Cue list to samples.
//
// This used to be testbench-only, and that was the one place the two halves were
// allowed to differ: the testbench mixed a cue list into a buffer, and the game
// handed the engine one voice per layer and let frame boundaries decide when each
// arrived. They cannot both be right. At 60 fps a frame is 16.7 ms and the
// composite the whole design rests on is transient +0 / surface +8 / body +20 /
// sub +65 - so the first two land on the same frame and the mechanism is gone.
//
// So the game mixes its composites too, in-process, and hands the engine one
// voice per acoustic moment. That is what this header is for, and it is why it
// lives in core/ rather than in the testbench: the placement, the resample and
// the gain are now the same arithmetic in both halves, so tuning offline says
// something about the game rather than something near it.
//
// What is NOT here, deliberately: panning and a master limiter. The engine
// spatialises our mono buffer through its own output model, and it has no bus to
// limit. Both stay on the testbench side of the line (00 section 13).

#include <cstdint>
#include <span>
#include <vector>

#include "rds/Cue.h"
#include "rds/Pcm.h"
#include "rds/Types.h"

namespace rds {

/// Read `source` at `pitch` with linear interpolation and add it into `dst` at
/// `startFrame`, at `gain`, fading in over `fadeInMs` and out over `fadeOutMs`.
///
/// The fade-out is measured back from where the voice actually ends, which the
/// pitch decides, so a tile pitched up still fades over the same milliseconds
/// rather than the same fraction of itself.
///
/// `stride` is how many floats one output frame occupies, so a caller mixing
/// interleaved stereo passes 2 and points `dst` at the channel it wants. That is
/// the only concession to the testbench in here, and it costs one resample per
/// channel rather than a second copy of this loop somewhere else.
void MixVoice(std::span<const float> source, std::ptrdiff_t startFrame, float gain, float pitch,
              float fadeInMs, float fadeOutMs, int sampleRate, float* dst, std::size_t dstFrames,
              std::size_t stride);

/// Linear gain from decibels. The one conversion both halves must agree on.
[[nodiscard]] float DbToLinear(float db);

struct MixParams {
    int sampleRate{48000};

    /// Hard cap on one composite. The longest slot is `imp_sub` at 400 ms and the
    /// latest layer offset is +65, so 600 leaves room without letting a bad cue
    /// time allocate something absurd.
    float maxLengthMs{600.0f};

    /// Soft-clip ceiling applied to the composite's own sum.
    ///
    /// This is not the testbench's master limiter and must not grow into one. We
    /// are summing four layers ourselves now, so inter-layer clipping inside one
    /// composite is genuinely ours to prevent - a wrap is a full-scale click.
    /// Across composites the engine still sums with no bus and no limiter, which
    /// is exactly the thing the testbench's pre-limiter peak readout exists to
    /// keep honest.
    float clipCeiling{0.98f};
};

/// One mixed acoustic moment, ready to be encoded and handed to the engine.
struct MixBuffer {
    std::vector<float> samples;
    int sampleRate{48000};

    /// Absolute engine time of the earliest cue in the group - when this buffer
    /// should start, not when it was mixed.
    TimeMs startMs{};

    /// Peak before the soft clip. Over 1.0 means the clip did work, which is
    /// worth a log line: it says the layer balance is hotter than the config
    /// thinks.
    float rawPeak{};

    /// Where the group sounds from. The loudest cue wins, because a composite is
    /// one event and several points read as several events (00 section 4, stage
    /// 4 rule 5).
    Vec3 position{};
    std::int32_t boneIndex{-1};

    [[nodiscard]] bool Empty() const { return samples.empty(); }
    [[nodiscard]] float LengthMs() const {
        return sampleRate > 0 ? 1000.0f * static_cast<float>(samples.size()) /
                                    static_cast<float>(sampleRate)
                              : 0.0f;
    }
};

/// Mix one composite: the one-shot cues that share an acoustic moment.
///
/// Offsets come out of the cues themselves - each carries an absolute time that
/// already includes its layer offset - so the earliest cue defines frame zero and
/// everything else lands at its true distance from it, to the sample. That is the
/// whole point of mixing rather than scheduling.
///
/// `out` is a reference rather than a return value so the caller can hold a pool
/// and keep the steady path free of allocation; it is resized, never reallocated,
/// once the pool has warmed.
///
/// False when there was nothing to mix - every cue empty, or a slot with no file
/// and no stand-in behind it.
bool MixComposite(std::span<const Cue> cues, PcmCache& cache, const MixParams& params, MixBuffer& out);

/// Render a loop slot into a buffer at least `lengthMs` long by tiling the source
/// with a short crossfade at the seam.
///
/// The engine's whole-file loop flag has no setter on `BSSoundHandle` and no
/// worked example anywhere in this tree, so a loop is a long one-shot that gets
/// re-issued. Tiling here rather than trusting the flag means the seam is ours to
/// make inaudible, which for a cloth bed and a stone scrape it comfortably is.
bool MixLoop(SlotId slot, std::uint8_t variant, float gainDb, float pitch, float lengthMs,
             float crossfadeMs, PcmCache& cache, const MixParams& params, MixBuffer& out);

}  // namespace rds
