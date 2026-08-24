#include "Player.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "miniaudio.h"

namespace tb {

Player::Player() : m_device(std::make_unique<ma_device>()) { std::memset(m_device.get(), 0, sizeof(ma_device)); }

Player::~Player() { Stop(); }

bool Player::Start(int sampleRate) {
    if (m_running) return true;
    m_sampleRate = sampleRate;

    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = 2;
    cfg.sampleRate = static_cast<ma_uint32>(sampleRate);
    cfg.dataCallback = &Player::Callback;
    cfg.pUserData = this;

    const ma_result r = ma_device_init(nullptr, &cfg, m_device.get());
    if (r != MA_SUCCESS) {
        std::snprintf(m_error, sizeof(m_error), "ma_device_init failed (%d)", static_cast<int>(r));
        return false;
    }
    const ma_result s = ma_device_start(m_device.get());
    if (s != MA_SUCCESS) {
        std::snprintf(m_error, sizeof(m_error), "ma_device_start failed (%d)", static_cast<int>(s));
        ma_device_uninit(m_device.get());
        return false;
    }
    m_sampleRate = static_cast<int>(m_device->sampleRate);
    m_running = true;
    return true;
}

void Player::Stop() {
    if (!m_running) return;
    ma_device_uninit(m_device.get());
    m_running = false;
}

void Player::SetBuffer(int side, std::shared_ptr<MixedAudio> buffer) {
    side = std::clamp(side, 0, 1);
    if (m_keep[side]) m_retired.push_back(m_keep[side]);
    m_keep[side] = std::move(buffer);
    m_active[side].store(m_keep[side].get(), std::memory_order_release);
    // The callback holds a raw pointer for at most one block, so a handful of
    // retired buffers is more than enough head-room to free them safely.
    while (m_retired.size() > 6) m_retired.erase(m_retired.begin());
}

void Player::Play() {
    const MixedAudio* buf = m_active[m_side.load(std::memory_order_relaxed)].load(std::memory_order_acquire);
    if (buf && m_cursor.load(std::memory_order_relaxed) >= buf->Frames()) m_cursor.store(0);
    m_playing.store(true, std::memory_order_relaxed);
}

void Player::Pause() { m_playing.store(false, std::memory_order_relaxed); }

void Player::TogglePlay() {
    if (Playing())
        Pause();
    else
        Play();
}

void Player::SeekMs(double ms) {
    m_cursor.store(static_cast<std::uint64_t>(std::max(0.0, ms) * 0.001 * m_sampleRate),
                   std::memory_order_relaxed);
}

double Player::PositionMs() const {
    return static_cast<double>(m_cursor.load(std::memory_order_relaxed)) * 1000.0 / m_sampleRate;
}

double Player::DurationMs() const {
    const MixedAudio* buf = m_active[m_side.load(std::memory_order_relaxed)].load(std::memory_order_acquire);
    return buf ? buf->durationMs : 0.0;
}

void Player::SetLoopRegion(double startMs, double endMs) {
    m_loopStartMs.store(std::max(0.0, startMs), std::memory_order_relaxed);
    m_loopEndMs.store(std::max(0.0, endMs), std::memory_order_relaxed);
}

bool Player::HasRegion() const {
    return m_loopEndMs.load(std::memory_order_relaxed) - m_loopStartMs.load(std::memory_order_relaxed) > 20.0;
}

void Player::SetPlayBounds(double startMs, double endMs) {
    m_boundsStartMs.store(std::max(0.0, startMs), std::memory_order_relaxed);
    m_boundsEndMs.store(std::max(0.0, endMs), std::memory_order_relaxed);
}

bool Player::HasPlayBounds() const {
    return m_boundsEndMs.load(std::memory_order_relaxed) -
               m_boundsStartMs.load(std::memory_order_relaxed) >
           20.0;
}

void Player::SetSplit(bool on) {
    m_split.store(on, std::memory_order_relaxed);
    if (!on) m_side.store(0, std::memory_order_relaxed);
}

float Player::DbToLinear(float db) { return std::pow(10.0f, db / 20.0f); }

void Player::SetPreview(std::shared_ptr<PreviewClip> clip, bool loop) {
    if (m_previewKeep) m_previewRetired.push_back(m_previewKeep);
    m_previewKeep = std::move(clip);
    m_previewLoop.store(loop, std::memory_order_relaxed);
    m_previewCursor.store(0, std::memory_order_relaxed);
    m_preview.store(m_previewKeep.get(), std::memory_order_release);
    m_previewPlaying.store(m_previewKeep && !m_previewKeep->Empty(), std::memory_order_relaxed);
    while (m_previewRetired.size() > 6) m_previewRetired.erase(m_previewRetired.begin());
}

void Player::StopPreview() { m_previewPlaying.store(false, std::memory_order_relaxed); }

std::string Player::PreviewFile() const {
    if (!m_previewPlaying.load(std::memory_order_relaxed) || !m_previewKeep) return {};
    return m_previewKeep->file;
}

float Player::PreviewProgress() const {
    const PreviewClip* clip = m_preview.load(std::memory_order_acquire);
    if (!clip || clip->mono.empty()) return 0.0f;
    const auto cursor = static_cast<double>(m_previewCursor.load(std::memory_order_relaxed));
    return static_cast<float>(std::clamp(cursor / static_cast<double>(clip->mono.size()), 0.0, 1.0));
}

void Player::RenderPreview(float* out, unsigned frames, float& peak) {
    if (!m_previewPlaying.load(std::memory_order_relaxed)) return;
    const PreviewClip* clip = m_preview.load(std::memory_order_acquire);
    if (!clip || clip->mono.empty()) return;

    const std::uint64_t total = clip->mono.size();
    const bool loop = m_previewLoop.load(std::memory_order_relaxed);
    const float gain = m_previewGain.load(std::memory_order_relaxed);
    std::uint64_t cursor = m_previewCursor.load(std::memory_order_relaxed);

    for (unsigned i = 0; i < frames; ++i) {
        if (cursor >= total) {
            if (!loop) {
                m_previewPlaying.store(false, std::memory_order_relaxed);
                break;
            }
            cursor = 0;
        }
        // Summed rather than replacing: a preview is heard *against* the take,
        // which is the whole reason it is a second voice.
        const float s = clip->mono[static_cast<std::size_t>(cursor)] * gain;
        out[i * 2] += s;
        out[i * 2 + 1] += s;
        peak = std::max(peak, std::fabs(out[i * 2]));
        ++cursor;
    }
    m_previewCursor.store(cursor, std::memory_order_relaxed);
}

void Player::Callback(ma_device* device, void* output, const void* /*input*/, unsigned frameCount) {
    auto* self = static_cast<Player*>(device->pUserData);
    self->Render(static_cast<float*>(output), frameCount);
}

void Player::Render(float* out, unsigned frames) {
    std::memset(out, 0, sizeof(float) * frames * 2);

    // The preview plays whether or not the transport is running - it is a second
    // voice, not a mode - so the two early exits below fall through to it rather
    // than returning.
    float peak = 0.0f;
    if (!m_playing.load(std::memory_order_relaxed)) {
        RenderPreview(out, frames, peak);
        m_level.store(peak, std::memory_order_relaxed);
        return;
    }

    int side = m_side.load(std::memory_order_relaxed);
    const MixedAudio* buf = m_active[side].load(std::memory_order_acquire);
    if (!buf || buf->Frames() == 0) {
        RenderPreview(out, frames, peak);
        m_level.store(peak, std::memory_order_relaxed);
        return;
    }

    const std::uint64_t total = buf->Frames();
    const bool loop = m_loop.load(std::memory_order_relaxed);
    const bool split = m_split.load(std::memory_order_relaxed);

    std::uint64_t regionStart = 0, regionEnd = total;
    const double ls = m_loopStartMs.load(std::memory_order_relaxed);
    const double le = m_loopEndMs.load(std::memory_order_relaxed);
    const bool explicitRegion = le - ls > 20.0;
    if (explicitRegion) {
        regionStart = static_cast<std::uint64_t>(ls * 0.001 * m_sampleRate);
        regionEnd = std::min<std::uint64_t>(total, static_cast<std::uint64_t>(le * 0.001 * m_sampleRate));
        if (regionEnd <= regionStart) {
            regionStart = 0;
            regionEnd = total;
        }
    } else if (HasPlayBounds()) {
        // Only where a drawn region would otherwise have left the whole take
        // playing. A selection always wins: it is the more specific answer to
        // "which part of this do you want to hear", and it is the one that was
        // asked for by hand.
        const double bs = m_boundsStartMs.load(std::memory_order_relaxed);
        const double be = m_boundsEndMs.load(std::memory_order_relaxed);
        regionStart = static_cast<std::uint64_t>(bs * 0.001 * m_sampleRate);
        regionEnd =
            std::min<std::uint64_t>(total, static_cast<std::uint64_t>(be * 0.001 * m_sampleRate));
        if (regionEnd <= regionStart) {
            regionStart = 0;
            regionEnd = total;
        }
    }

    std::uint64_t cursor = m_cursor.load(std::memory_order_relaxed);
    if (cursor < regionStart) cursor = regionStart;

    for (unsigned i = 0; i < frames; ++i) {
        if (cursor >= regionEnd) {
            if (!loop) {
                m_playing.store(false, std::memory_order_relaxed);
                break;
            }
            cursor = regionStart;
            m_loops.fetch_add(1, std::memory_order_relaxed);
            if (split) {
                side ^= 1;
                m_side.store(side, std::memory_order_relaxed);
                const MixedAudio* other = m_active[side].load(std::memory_order_acquire);
                if (other && other->Frames() > regionStart) {
                    buf = other;
                    // Two configs can produce two different tail lengths, so
                    // the whole-take end has to follow the buffer we swapped to.
                    if (!explicitRegion) regionEnd = other->Frames();
                }
            }
        }
        const std::size_t idx = static_cast<std::size_t>(cursor) * 2;
        if (idx + 1 < buf->stereo.size()) {
            const float l = buf->stereo[idx];
            const float r = buf->stereo[idx + 1];
            out[i * 2] = l;
            out[i * 2 + 1] = r;
            peak = std::max(peak, std::max(std::fabs(l), std::fabs(r)));
        }
        ++cursor;
    }

    m_cursor.store(cursor, std::memory_order_relaxed);
    RenderPreview(out, frames, peak);
    m_level.store(peak, std::memory_order_relaxed);
}

}  // namespace tb
