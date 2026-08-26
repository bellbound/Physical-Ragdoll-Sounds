#pragma once

// Reading wavs, and writing one back.
//
// The game half mixes its own composites now rather than handing the engine four
// files and hoping they land on the right frames, so both halves need the same
// two things: a slot's samples as mono float, and a complete RIFF/WAVE container
// to hand back. Both live here, in core/, so the testbench and the DLL cannot
// drift into decoding the pack differently and then disagreeing about what a
// config sounded like.
//
// No decoder library. The pack is 16-bit PCM out of `tools/sfx.py make`, and the
// chunk walk this needs is the one `SlotManifest` already had for measuring
// lengths - promoted here so there is one parser rather than two.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "rds/SlotManifest.h"

namespace rds {

/// Mono float PCM at a known rate, decoded from a wav on disk.
///
/// Empty is a real answer and every caller checks for it: the slot has no
/// recording, or the file it names would not decode. Nothing is synthesised to
/// cover either case, so an empty buffer is a layer that does not sound.
struct PcmBuffer {
    std::vector<float> samples;
    int sampleRate{48000};

    [[nodiscard]] bool Empty() const { return samples.empty(); }
    [[nodiscard]] float LengthMs() const {
        return sampleRate > 0 ? 1000.0f * static_cast<float>(samples.size()) /
                                    static_cast<float>(sampleRate)
                              : 0.0f;
    }
};

/// What a wav's header says, without reading its samples.
struct WavInfo {
    int sampleRate{};
    int channels{};
    int bitsPerSample{};
    bool floatFormat{};
    std::uint64_t frames{};

    [[nodiscard]] bool Valid() const { return sampleRate > 0 && channels > 0 && frames > 0; }
};

/// Header only. Cheap, and the load path uses it to size a buffer before reading.
[[nodiscard]] WavInfo ProbeWav(const std::string& path);

/// Length in milliseconds, or 0 for anything that is not a PCM wav we understand.
///
/// Read at bank load rather than guessed from the slot's declared range, because
/// a renderer sizes its mix buffer from it and a value that is short truncates
/// the tail - which on `imp_sub` is most of the sound.
[[nodiscard]] float WavLengthMs(const std::string& path);

/// Decode to mono float at `targetRate`, downmixing channels and resampling
/// linearly. Empty on anything unreadable; the caller falls back to the stand-in.
///
/// Linear resampling is right here and would not be for a general audio tool. The
/// shipped pack is all 48 kHz, so the resample is a no-op on every file in it and
/// this path only runs for something dropped in at another rate - where a 44.1
/// source is a ratio of 1.088 and its linear-interpolation artefact sits far
/// below the noise floor of a body impact.
///
/// (03-Asset-Status.md §1 says 44.1; it predates `tools/build_pack.py` moving to
/// 48 k and every file measures 48 k today.)
[[nodiscard]] PcmBuffer ReadWavMono(const std::string& path, int targetRate);

/// A complete RIFF/WAVE PCM-16 container, which is exactly what the engine's
/// external audio interface expects to be handed a pointer to.
///
/// Samples outside [-1, 1] are clipped rather than wrapped, because a wrap is a
/// full-scale click and a clip is the loud thing being slightly less loud.
///
/// The `Into` form writes through a buffer the caller owns, so the game half can
/// keep one per voice slot and encode a composite every few frames without ever
/// allocating. The returning form is the convenience wrapper for everything that
/// does not run on the game thread.
void EncodeWavPcm16Into(std::span<const float> mono, int sampleRate, std::vector<std::uint8_t>& out);

[[nodiscard]] std::vector<std::uint8_t> EncodeWavPcm16(std::span<const float> mono, int sampleRate);

/// Every slot variant the mixer has asked for, decoded once.
///
/// Keyed on (slot, variant) because that is what a `Cue` carries - the bank has
/// already made the choice and re-resolving would advance the shuffle bag and
/// hand back a different file than the cue names.
///
/// A variant with no file behind it falls through to `Synthesise`, so a partial
/// pack is a quieter mod rather than a broken one. That fallback is new to the
/// game half: it used to be testbench-only, because the game played files off
/// disk and had nowhere to put a buffer. Mixing our own composites removed that
/// asymmetry, and it is the reason the two halves can now share this cache.
class PcmCache {
public:
    /// The bank stays owned by the caller and must outlive the cache.
    void SetBank(const SoundBank* bank, int sampleRate);

    /// Mono at the cache's rate. Never fails; an unfilled voice slot comes back
    /// empty and the caller skips the cue.
    [[nodiscard]] const PcmBuffer& Get(SlotId slot, std::uint8_t variant);

    [[nodiscard]] int SampleRate() const { return m_sampleRate; }

    /// Drop everything. A bank reload invalidates every path we cached.
    void Clear();

private:
    [[nodiscard]] static std::uint32_t Key(SlotId slot, std::uint8_t variant);

    const SoundBank* m_bank{};
    int m_sampleRate{48000};
    std::vector<std::pair<std::uint32_t, PcmBuffer>> m_entries;
    PcmBuffer m_empty;
};

}  // namespace rds
