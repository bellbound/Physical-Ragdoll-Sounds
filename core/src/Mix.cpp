#include "rds/Mix.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>

namespace rds {
namespace {

/// Below this a cue is silence and mixing it is wasted work. -80 dB is far under
/// the engine's own voice floor and well under anything the design calls quiet.
constexpr float kSilenceGain = 1.0e-4f;

/// The pitch floor. A cue should never ask for zero, but a zero step is an
/// infinite loop rather than a quiet sound, so it is worth refusing here.
constexpr float kMinPitch = 0.01f;

}  // namespace

float DbToLinear(float db) { return std::pow(10.0f, db / 20.0f); }

void MixVoice(std::span<const float> source, std::ptrdiff_t startFrame, float gain, float pitch,
              float fadeInMs, float fadeOutMs, int sampleRate, float* dst, std::size_t dstFrames,
              std::size_t stride) {
    if (source.size() < 2 || dst == nullptr || dstFrames == 0 || gain <= kSilenceGain) {
        return;
    }

    const double step = std::max(kMinPitch, pitch);
    const double sourceLength = static_cast<double>(source.size());

    // A negative start means the source began before this buffer does. Skip into
    // it rather than dropping it: on a loop re-issue that is the overlap, and on a
    // composite it is a cue whose layer offset put it behind the group start.
    double pos = 0.0;
    std::size_t first = 0;
    if (startFrame < 0) {
        pos = static_cast<double>(-startFrame) * step;
        if (pos >= sourceLength - 1.0) {
            return;
        }
    } else {
        first = static_cast<std::size_t>(startFrame);
        if (first >= dstFrames) {
            return;
        }
    }

    // Where the voice runs out, in output frames from `first`. The pitch decides
    // it, which is why the fade-out is measured against this rather than against
    // the source length.
    const auto voiceFrames =
        static_cast<std::size_t>(std::max(0.0, (sourceLength - 1.0 - pos) / step));

    const auto fadeIn = static_cast<std::size_t>(
        std::max(0.0f, fadeInMs) * 0.001f * static_cast<float>(sampleRate));
    const auto fadeOut = std::min(
        voiceFrames, static_cast<std::size_t>(std::max(0.0f, fadeOutMs) * 0.001f *
                                              static_cast<float>(sampleRate)));

    for (std::size_t i = first; i < dstFrames; ++i) {
        if (pos >= sourceLength - 1.0) {
            break;
        }
        const std::size_t index = static_cast<std::size_t>(pos);
        const float frac = static_cast<float>(pos - static_cast<double>(index));
        const float sample = source[index] * (1.0f - frac) + source[index + 1] * frac;

        const std::size_t done = i - first;
        float g = gain;
        if (fadeIn > 0 && done < fadeIn) {
            g *= static_cast<float>(done) / static_cast<float>(fadeIn);
        }
        if (fadeOut > 0 && done + fadeOut > voiceFrames) {
            const std::size_t left = voiceFrames > done ? voiceFrames - done : 0;
            g *= static_cast<float>(left) / static_cast<float>(fadeOut);
        }
        dst[i * stride] += sample * g;
        pos += step;
    }
}

bool MixComposite(std::span<const Cue> cues, ISampleSource& cache, const MixParams& params,
                  MixBuffer& out, std::optional<TimeMs> timeBaseMs) {
    out.samples.clear();
    out.sampleRate = params.sampleRate;
    out.rawPeak = 0.0f;
    out.boneIndex = -1;
    out.position = {};
    out.limbIndex = 0;
    out.collapsed = false;
    if (cues.empty() || params.sampleRate <= 0) {
        return false;
    }

    // Frame zero is the earliest cue, unless the caller named the instant - see
    // the header for why the game does. Everything downstream - the blob, the
    // handle, the start time we schedule against - is relative to this instant.
    TimeMs earliest = cues.front().timeMs;
    for (const Cue& cue : cues) {
        earliest = std::min(earliest, cue.timeMs);
    }
    // Never later than the earliest cue: a base past a cue would place it at a
    // negative frame, and MixVoice would swallow it rather than say so.
    if (timeBaseMs) {
        earliest = std::min(earliest, *timeBaseMs);
    }
    out.startMs = earliest;

    // A layer so far behind the moment that its cue time cannot be meant. Dropped
    // rather than clamped: a layer moved in time is a wrong sound, where a missing
    // one is an obvious one - and it is the only term here that can run away,
    // since the other comes off a file on disk.
    const auto absurd = [&](const Cue& cue) {
        return cue.timeMs - earliest > static_cast<double>(params.maxOffsetMs);
    };

    // How long the buffer has to be: the latest layer's offset plus its own
    // length. Measured from the sources rather than the slot briefs, because a
    // recording is only ever roughly the length its brief asked for and a buffer
    // sized from the brief truncates whichever ran long.
    //
    // **Not capped.** It used to be, at 600 ms, and `head_impact`'s longest
    // recording is 670 - so the mod cut 70 ms off a shipped sound and nobody was
    // told. Nothing downstream wants it bounded: voices do not queue, so a long
    // composite holds its own blob for its own length and delays no other sound.
    double neededMs = 0.0;
    float loudest = -1000.0f;
    for (const Cue& cue : cues) {
        if (cue.op != CueOp::kPlayOneShot) {
            continue;
        }
        const PcmBuffer& source = cache.Get(cue.slot, cue.variant);
        if (source.Empty()) {
            continue;
        }
        if (absurd(cue)) {
            spdlog::warn("mix: {} sits {:.0f} ms behind its moment, past the {:.0f} ms guard - "
                         "dropped as a bad cue time",
                         ToString(cue.slot), cue.timeMs - earliest, params.maxOffsetMs);
            continue;
        }
        const double pitch = std::max(kMinPitch, cue.pitch);
        const double lengthMs = static_cast<double>(source.LengthMs()) / pitch;
        neededMs = std::max(neededMs, (cue.timeMs - earliest) + lengthMs);

        if (cue.gainDb > loudest) {
            loudest = cue.gainDb;
            out.position = cue.position;
            out.boneIndex = cue.boneIndex;
            out.limbIndex = cue.limbIndex;
            out.collapsed = cue.collapsed;
        }
    }
    if (neededMs <= 0.0) {
        return false;
    }

    const std::size_t frames =
        static_cast<std::size_t>(neededMs * 0.001 * static_cast<double>(params.sampleRate)) + 1;
    out.samples.assign(frames, 0.0f);

    for (const Cue& cue : cues) {
        if (cue.op != CueOp::kPlayOneShot) {
            continue;
        }
        const PcmBuffer& source = cache.Get(cue.slot, cue.variant);
        if (source.Empty() || absurd(cue)) {
            continue;
        }
        const auto startFrame = static_cast<std::ptrdiff_t>(
            (cue.timeMs - earliest) * 0.001 * static_cast<double>(params.sampleRate));
        MixVoice(source.samples, startFrame, DbToLinear(cue.gainDb), cue.pitch, cue.fadeMs, 0.0f,
                 params.sampleRate, out.samples.data(), frames, 1);
    }

    const float ceiling = std::max(0.1f, params.clipCeiling);
    for (float& sample : out.samples) {
        out.rawPeak = std::max(out.rawPeak, std::fabs(sample));
        // tanh rather than a hard clamp: at these levels it is the difference
        // between a stacked burst sounding compressed and sounding broken, and
        // below the knee it is very nearly a straight line.
        sample = std::tanh(sample / ceiling) * ceiling;
    }
    return true;
}

bool MixLoop(SlotId slot, std::uint8_t variant, float gainDb, float pitch, float lengthMs,
             float crossfadeMs, float startFadeMs, ISampleSource& cache, const MixParams& params,
             MixBuffer& out) {
    out.samples.clear();
    out.sampleRate = params.sampleRate;
    out.rawPeak = 0.0f;
    out.startMs = 0.0;
    if (params.sampleRate <= 0 || lengthMs <= 0.0f) {
        return false;
    }

    const PcmBuffer& source = cache.Get(slot, variant);
    if (source.samples.size() < 4) {
        return false;
    }

    const double step = std::max(kMinPitch, pitch);
    const std::size_t frames =
        static_cast<std::size_t>(static_cast<double>(lengthMs) * 0.001 *
                                 static_cast<double>(params.sampleRate)) +
        1;
    out.samples.assign(frames, 0.0f);

    // One tile, at pitch. Shorter than the source when pitched up, which is why
    // the tile length is computed rather than taken from the file.
    const auto tileFrames = static_cast<std::size_t>(
        std::max(2.0, (static_cast<double>(source.samples.size()) - 1.0) / step));
    const auto fadeFrames = std::min<std::size_t>(
        tileFrames / 2, static_cast<std::size_t>(std::max(0.0f, crossfadeMs) * 0.001f *
                                                 static_cast<float>(params.sampleRate)));

    const float gain = DbToLinear(gainDb);
    const auto advance = tileFrames > fadeFrames ? tileFrames - fadeFrames : tileFrames;

    for (std::ptrdiff_t start = 0; static_cast<std::size_t>(start) < frames;
         start += static_cast<std::ptrdiff_t>(advance)) {
        // Each tile fades in over the seam and the one before it fades out across
        // the same window. Linear rather than equal-power: the two tiles are the
        // same signal a loop-length apart and correlate, so a linear pair sums
        // flat where an equal-power pair would bulge.
        const float fadeIn = start == 0 ? std::max(0.0f, startFadeMs) : crossfadeMs;
        MixVoice(source.samples, start, gain, pitch, fadeIn, crossfadeMs, params.sampleRate,
                 out.samples.data(), frames, 1);
    }

    // The buffer's own end, which the tiling above never reaches. Left alone it
    // stops at whatever amplitude the source happened to be at, which is a step at
    // every re-issue - and it leaves the replacement's opening with nothing to fade
    // against, so the two ran together at full level for the length of the overlap
    // and then one of them stopped dead. Faded here, a re-issue is a crossfade.
    const auto tailFrames =
        std::min(frames, static_cast<std::size_t>(std::max(0.0f, crossfadeMs) * 0.001f *
                                                  static_cast<float>(params.sampleRate)));
    for (std::size_t i = 0; i < tailFrames; ++i) {
        out.samples[frames - 1 - i] *= static_cast<float>(i) / static_cast<float>(tailFrames);
    }

    for (float& sample : out.samples) {
        out.rawPeak = std::max(out.rawPeak, std::fabs(sample));
    }
    return true;
}

}  // namespace rds
