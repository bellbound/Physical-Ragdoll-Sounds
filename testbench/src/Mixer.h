#pragma once

// Cue list to a stereo float buffer.
//
// This is the testbench's ICueSink rendered offline: every cue is a
// ResolvedSound played at its gainDb and pitch, starting at cue.timeMs, with
// loops honouring kStartLoop / kUpdateLoop / kStopLoop. It deliberately does
// only what Cue.h says the game can do - gain, continuous pitch, position,
// looping, fades - because anything richer would tune the mix against
// something CommonLibVR cannot reproduce.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "rds/Cue.h"
#include "rds/Feed.h"
#include "rds/SlotManifest.h"

namespace tb {

struct MixedAudio {
    int sampleRate{48000};
    std::vector<float> stereo;  ///< interleaved L,R
    double durationMs{};
    float peak{};     ///< after the soft clip - what you hear
    float rawPeak{};  ///< before it. Over 1.0 means the limiter is doing work,
                      ///< which the game will not do for you

    [[nodiscard]] std::size_t Frames() const { return stereo.size() / 2; }
};

/// Decode any file miniaudio understands to mono float at `sampleRate`.
///
/// The library's own decode path, and the one the importer measures through, so
/// what the browser plays and what the analysis measured are the same samples.
[[nodiscard]] bool DecodeMonoFile(const std::string& path, std::vector<float>& out,
                                  int& sampleRate);

/// The same, at a fixed channel count and interleaved. Only the stereo
/// correlation probe wants this - everything downstream of a slot is mono.
[[nodiscard]] bool DecodeInterleaved(const std::string& path, int channels,
                                     std::vector<float>& out, int& sampleRate);

/// Renders and caches one mono buffer per (slot, variant).
///
/// A cue carries the slot and the variant the bank already picked, so this does
/// not go back through SoundBank::Resolve - that would advance the shuffle bag
/// and pick a different file than the engine did. Files are looked up by the
/// bank's own naming convention, `<slot name>_<NN>.wav`; anything missing falls
/// through to the procedural stand-in, which today is everything.
class SoundSource {
public:
    /// The bank stays owned by the caller and must outlive this.
    ///
    /// Through the bank rather than by rebuilding `<slot>_<NN>.wav` here, which
    /// is what this used to do and got wrong twice over: the variant index is a
    /// position in a sorted set and not the number in the filename, so variant 0
    /// looked for `imp_body_00.wav`, never found it, and played a procedural
    /// stand-in for the first variant of every slot. And now that a slot's files
    /// can be named anything at all, a filename is not something a second place
    /// can derive.
    void SetBank(const rds::SoundBank* bank, int sampleRate);

    /// Mono at the mixer's sample rate. Never fails; an unfilled voice slot
    /// comes back empty and the cue is skipped.
    const std::vector<float>& Get(rds::SlotId slot, std::uint8_t variant);

    [[nodiscard]] bool IsProcedural(rds::SlotId slot, std::uint8_t variant) const;

    /// "imp_sub: 0/2 files, procedural" - the same line the bank logs, for the UI.
    [[nodiscard]] std::vector<std::string> Report() const;

    /// Drop every decoded buffer. Called when a slot's assignment changes: the
    /// cache is keyed on (slot, variant) and that key now means a different file.
    void Invalidate();

    /// Play `path` for every variant of `slot`, whatever the bank holds.
    ///
    /// The picker's in-take audition. Every variant and not just the one being
    /// replaced, because the question is "what does this sound like here" and a
    /// take where one cue in three is the candidate and the rest are the old
    /// file answers a different one. Nothing is assigned by it and nothing is
    /// written: it lives until it is cleared, and the mix is what changes.
    ///
    /// Decoded once, here, so the callback-shaped work is not done per cue.
    void SetAudition(rds::SlotId slot, const std::string& path);
    void ClearAudition();

private:
    struct Entry {
        std::vector<float> samples;
        bool procedural{true};
    };
    std::map<std::uint32_t, Entry> m_cache;
    const rds::SoundBank* m_bank{};
    int m_sampleRate{48000};

    /// -1 when nothing is being auditioned. Held outside the cache because it is
    /// not keyed on a variant and must not survive an Invalidate.
    int m_auditionSlot{-1};
    std::string m_auditionPath;
    std::vector<float> m_auditionSamples;
};

/// Mix a cue list. `listener` positions the stereo image; cues attached to a
/// bone (boneIndex >= 0) sit centre, which is what the player's own ragdoll
/// does in the game.
/// `limiter` soft-clips the sum. The game has no bus and no limiter, so this is
/// the one thing here that is richer than what CommonLibVR can do - 00 section
/// 13 says anything richer sits behind a flag marked not shippable, and this is
/// that flag. `masterGainDb` is the audition level, not mix.masterGainDb.
MixedAudio MixCues(const std::vector<rds::Cue>& cues, double audioDurationMs,
                   const rds::ListenerState& listener, SoundSource& sources, int sampleRate,
                   float masterGainDb, bool limiter);

}  // namespace tb
