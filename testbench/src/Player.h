#pragma once

// The transport: a miniaudio device reading one of two mixed buffers.
//
// The buffer can be replaced while it is playing, at the current position, with
// no restart and no gap - which is the whole point of the config panel. The
// audio thread only ever reads a raw pointer out of an atomic; the UI thread
// keeps the last few buffers alive in a small ring so the one the callback is
// mid-way through cannot be freed under it.

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "Mixer.h"

struct ma_device;

namespace tb {

/// One library sound, decoded at the device's rate so the preview path has no
/// resampler in it.
///
/// Separate from MixedAudio because it is mono and because it is not a take: it
/// has no cues, no duration that anything else agrees with, and no side.
struct PreviewClip {
    std::vector<float> mono;
    std::string file;  ///< which library file this is, so the UI can say so

    [[nodiscard]] bool Empty() const { return mono.empty(); }
};

class Player {
public:
    Player();
    ~Player();

    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    [[nodiscard]] bool Start(int sampleRate);
    void Stop();
    [[nodiscard]] bool Running() const { return m_running; }
    [[nodiscard]] const char* LastError() const { return m_error; }

    /// side 0 = config A, side 1 = config B. Swaps in place: the play position
    /// is untouched, so a slider move is heard on the next block.
    void SetBuffer(int side, std::shared_ptr<MixedAudio> buffer);
    [[nodiscard]] std::shared_ptr<MixedAudio> Buffer(int side) const { return m_keep[side]; }

    void Play();
    void Pause();
    void TogglePlay();
    [[nodiscard]] bool Playing() const { return m_playing.load(std::memory_order_relaxed); }

    void SeekMs(double ms);
    [[nodiscard]] double PositionMs() const;
    [[nodiscard]] double DurationMs() const;

    void SetLoop(bool on) { m_loop.store(on, std::memory_order_relaxed); }
    [[nodiscard]] bool Loop() const { return m_loop.load(std::memory_order_relaxed); }

    /// The draggable region. An empty or inverted region means the whole take.
    void SetLoopRegion(double startMs, double endMs);
    [[nodiscard]] double LoopStartMs() const { return m_loopStartMs.load(std::memory_order_relaxed); }
    [[nodiscard]] double LoopEndMs() const { return m_loopEndMs.load(std::memory_order_relaxed); }
    [[nodiscard]] bool HasRegion() const;

    /// While split, each loop alternates A -> B -> A.
    void SetSplit(bool on);
    [[nodiscard]] int ActiveSide() const { return m_side.load(std::memory_order_relaxed); }
    void SetActiveSide(int side) { m_side.store(side, std::memory_order_relaxed); }
    [[nodiscard]] std::uint32_t LoopCount() const { return m_loops.load(std::memory_order_relaxed); }

    /// Peak of the last block, for the meter.
    [[nodiscard]] float Level() const { return m_level.load(std::memory_order_relaxed); }

    // ── the preview voice ────────────────────────────────────────────────────
    //
    // A second voice mixed on top of the transport, for auditioning one library
    // file. Deliberately not the transport itself: comparing what a slot plays
    // now against what it would play means hearing them near each other, and a
    // preview that stopped the take would make that two clicks and a re-seek
    // every time. It is also why the transport keeps the spacebar and the
    // browser's own list gets its own key.

    /// Play `clip` from the start. A null clip stops the preview.
    void SetPreview(std::shared_ptr<PreviewClip> clip, bool loop);
    void StopPreview();
    [[nodiscard]] bool PreviewPlaying() const {
        return m_previewPlaying.load(std::memory_order_relaxed);
    }
    /// Which file the preview is on, empty when nothing is previewing. Read on
    /// the UI thread only.
    [[nodiscard]] std::string PreviewFile() const;
    /// 0..1 through the clip, for the little progress bar on a preview widget.
    [[nodiscard]] float PreviewProgress() const;
    void SetPreviewGainDb(float db) { m_previewGain.store(DbToLinear(db), std::memory_order_relaxed); }

    [[nodiscard]] int SampleRate() const { return m_sampleRate; }

private:
    static void Callback(ma_device* device, void* output, const void* input, unsigned frameCount);
    void Render(float* out, unsigned frames);
    void RenderPreview(float* out, unsigned frames, float& peak);
    [[nodiscard]] static float DbToLinear(float db);

    std::unique_ptr<ma_device> m_device;
    bool m_running{};
    char m_error[256]{};
    int m_sampleRate{48000};

    std::atomic<const MixedAudio*> m_active[2]{nullptr, nullptr};
    std::shared_ptr<MixedAudio> m_keep[2];
    std::vector<std::shared_ptr<MixedAudio>> m_retired;  ///< UI thread only

    std::atomic<std::uint64_t> m_cursor{0};
    std::atomic<bool> m_playing{false};
    std::atomic<bool> m_loop{true};
    std::atomic<bool> m_split{false};
    std::atomic<int> m_side{0};
    std::atomic<std::uint32_t> m_loops{0};
    std::atomic<double> m_loopStartMs{0.0};
    std::atomic<double> m_loopEndMs{0.0};
    std::atomic<float> m_level{0.0f};

    /// Same trick as the take buffers: the callback reads a raw pointer out of
    /// an atomic and the UI thread keeps the last few alive so the one being
    /// read cannot be freed under it.
    std::atomic<const PreviewClip*> m_preview{nullptr};
    std::shared_ptr<PreviewClip> m_previewKeep;
    std::vector<std::shared_ptr<PreviewClip>> m_previewRetired;
    std::atomic<std::uint64_t> m_previewCursor{0};
    std::atomic<bool> m_previewPlaying{false};
    std::atomic<bool> m_previewLoop{false};
    std::atomic<float> m_previewGain{1.0f};
};

}  // namespace tb
