#include "GameRenderer.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>

#include "AudioBlobs.h"

namespace rds::game {
namespace {

/// Flags for GetSoundHandle.
///
///   0x8000 - resolve through the external audio interface rather than the file
///            system. This is the whole mechanism.
///   0x0080 - do not reuse or clone a cached or active sound with a matching
///            resource id. Ours are single-use, and a reused voice would play the
///            previous composite.
///
/// SkyrimNet also passes 0x0010, which is the flag vanilla dialogue always sets.
/// We are not dialogue, so it is left out.
constexpr std::uint32_t kSoundFlags = 0x8000 | 0x0080;

/// Ragdoll impacts should outlive ambience but never a line of dialogue.
constexpr std::uint8_t kPriority = 0x30;

/// How long after a voice's own length its blob may be retired.
///
/// Play() only queues; the engine opens the sound a few audio ticks later, so a
/// blob retired on the nose can be refused before it was ever served. This is the
/// same start latency SkyrimNet allows 750 ms of grace for, rounded up.
constexpr double kRetireGraceMs = 1000.0;

/// How long a loop buffer is, and how much of the next one overlaps the last.
///
/// Four seconds is long enough that a re-issue is rare and short enough that a
/// gain change is picked up promptly; the overlap is crossfaded inside the buffer
/// by MixLoop, so the seam is ours rather than the engine's.
constexpr float kLoopBufferMs = 4000.0f;
constexpr float kLoopOverlapMs = 150.0f;

/// What the engine reads back off our descriptor. Mirrored vtable layout; slot
/// order from BSISoundDescriptor.h.
class PlaybackCharacteristics {
public:
    virtual ~PlaybackCharacteristics() = default;               // 00
    virtual std::uint8_t GetFrequencyShift() { return 0; }      // 01 - pitch is baked, never shifted
    virtual std::uint8_t GetFrequencyVariance() { return 0; }   // 02 - and never wobbled
    virtual std::uint8_t GetPriority() { return kPriority; }    // 03
    virtual std::uint16_t GetStaticAttenuation() { return 0; }  // 04 - gain is baked too
    virtual std::uint8_t GetDBVariance() { return 0; }          // 05
};

/// Describes our audio the way the engine expects, so GetSoundHandle applies the
/// whole Resolution itself: output model, priority, attenuation. The caller's
/// flags are OR'd into this, so 0x8000 still routes the open to our interface.
class SoundDescriptor {
public:
    virtual ~SoundDescriptor() = default;  // 00

    // 01 - the engine copies this out and applies it to the BSGameSound.
    virtual bool DoResolve(RE::BSISoundDescriptor::Resolution& a_resolution) {
        a_resolution.resourceID = resourceID;
        a_resolution.formID = 0;
        a_resolution.alternateFormID = 0;
        a_resolution.flags = 0;  // OR'd with the caller's flags by GetSoundHandle
        a_resolution.playbackCharacteristics =
            reinterpret_cast<RE::BSISoundDescriptor::BSIPlaybackCharacteristics*>(&characteristics);
        a_resolution.outputModel = const_cast<RE::BSISoundOutputModel*>(outputModel);
        a_resolution.soundCategory = nullptr;
        return true;
    }

    virtual void Unk_02() {}  // 02 - unidentified; never observed being called

    RE::BSResource::ID resourceID{};
    const RE::BSISoundOutputModel* outputModel = nullptr;
    PlaybackCharacteristics characteristics{};
};

[[nodiscard]] RE::NiPoint3 ToNiPoint(const Vec3& v) { return RE::NiPoint3{v.x, v.y, v.z}; }

}  // namespace

GameRenderer::GameRenderer() {
    m_groups.reserve(16);
    m_voices.reserve(32);
    m_loops.reserve(8);
    m_encoded.reserve(64 * 1024);
    m_mixParams.sampleRate = kSampleRate;
}

GameRenderer::~GameRenderer() = default;

void GameRenderer::SetSoundBank(SoundBank* bank) {
    m_bank = bank;
    m_cache.SetBank(bank, kSampleRate);
}

void GameRenderer::SetBoneResolver(BoneResolver resolver) { m_boneResolver = std::move(resolver); }

void GameRenderer::Counters(std::uint64_t& cuesIn, std::uint64_t& voicesOut) const {
    cuesIn = m_cuesIn;
    voicesOut = m_voicesOut;
}

RE::NiAVObject* GameRenderer::NodeFor(ActorId actorId, std::int32_t boneIndex) const {
    if (boneIndex < 0 || !m_boneResolver) {
        return nullptr;
    }
    return m_boneResolver(actorId, boneIndex);
}

void GameRenderer::Emit(const Cue& cue) {
    ++m_cuesIn;

    switch (cue.op) {
        case CueOp::kPlayOneShot: {
            // Grouped, not played. A cue's time is absolute and the sub is
            // deliberately 65 ms in the future, so playing on arrival would
            // collapse the layer stack into one instant.
            const auto it = std::ranges::find_if(m_groups, [&](const Group& g) {
                return g.actorId == cue.actorId && g.sourceSeq == cue.sourceSeq;
            });
            if (it != m_groups.end()) {
                it->earliestMs = std::min(it->earliestMs, cue.timeMs);
                it->cues.push_back(cue);
                return;
            }
            Group group;
            group.actorId = cue.actorId;
            group.sourceSeq = cue.sourceSeq;
            group.earliestMs = cue.timeMs;
            group.cues.push_back(cue);
            m_groups.push_back(std::move(group));
            return;
        }

        case CueOp::kStartLoop: {
            // A second start on a live voice id is an update, not a second voice.
            // Without this a strategy that re-arms its loop leaves the first one
            // running forever with nothing holding its id.
            const auto existing = std::ranges::find_if(
                m_loops, [&](const Loop& l) { return l.voiceId == cue.voiceId; });
            if (existing != m_loops.end()) {
                existing->gainDb = cue.gainDb;
                existing->pitch = cue.pitch;
                existing->position = cue.position;
                existing->boneIndex = cue.boneIndex;
                return;
            }
            Loop loop;
            loop.voiceId = cue.voiceId;
            loop.slot = cue.slot;
            loop.variant = cue.variant;
            loop.gainDb = cue.gainDb;
            loop.pitch = cue.pitch;
            loop.position = cue.position;
            loop.boneIndex = cue.boneIndex;
            loop.actorId = cue.actorId;
            m_loops.push_back(loop);
            return;
        }

        case CueOp::kUpdateLoop: {
            const auto it = std::ranges::find_if(
                m_loops, [&](const Loop& l) { return l.voiceId == cue.voiceId; });
            if (it == m_loops.end()) {
                return;
            }
            // Position takes effect at once, on the live handle. Gain and pitch
            // are baked into the buffer, so they are picked up by the next
            // re-issue - which is at most kLoopBufferMs away and inaudible as a
            // step, because the crossfade carries it.
            it->gainDb = cue.gainDb;
            it->pitch = cue.pitch;
            it->position = cue.position;
            it->boneIndex = cue.boneIndex;
            if (it->handle.IsValid() && it->boneIndex < 0) {
                it->handle.SetPosition(ToNiPoint(it->position));
            }
            return;
        }

        case CueOp::kStopLoop: {
            const auto it = std::ranges::find_if(
                m_loops, [&](const Loop& l) { return l.voiceId == cue.voiceId; });
            if (it == m_loops.end()) {
                return;
            }
            if (it->handle.IsValid()) {
                it->handle.FadeOutAndRelease(static_cast<std::uint16_t>(std::max(0.0f, cue.fadeMs)));
            }
            BlobRegistry::Get().Retire(it->token);
            m_loops.erase(it);
            return;
        }
    }
}

bool GameRenderer::Open(std::span<const std::uint8_t> wav, const Vec3& position,
                        RE::NiAVObject* follow, RE::BSSoundHandle& handleOut,
                        std::uint64_t& tokenOut) {
    auto* manager = RE::BSAudioManager::GetSingleton();
    if (manager == nullptr || wav.empty()) {
        return false;
    }

    auto& registry = BlobRegistry::Get();
    if (!registry.Installed()) {
        return false;
    }

    std::uint32_t fileId = 0;
    std::uint32_t dirId = 0;
    const std::uint64_t token = registry.Register(wav, fileId, dirId);
    if (token == 0) {
        return false;
    }

    SoundDescriptor descriptor;
    descriptor.resourceID.file = fileId;
    descriptor.resourceID.dir = dirId;
    std::memcpy(descriptor.resourceID.ext, "wav", 3);
    descriptor.outputModel = DefaultOutputModel();

    if (descriptor.outputModel == nullptr && !m_warnedNoModel) {
        m_warnedNoModel = true;
        spdlog::error("render: no sound output model, so every voice will be flat, unattenuated "
                      "and will follow you around the room. Check iOutputModelFormID.");
    }

    // Stack local: GetSoundHandle resolves synchronously and copies the
    // Resolution out.
    RE::BSSoundHandle handle{};
    const bool opened = manager->GetSoundHandle(
        handle, reinterpret_cast<RE::BSISoundDescriptor*>(&descriptor), kSoundFlags);
    if (!opened || !handle.IsValid()) {
        spdlog::error("render: GetSoundHandle refused file {} ({} bytes)", fileId, wav.size());
        registry.Retire(token);
        return false;
    }

    // Every engine sound needs somewhere to be. Unattached, under a model that
    // attenuates with distance, means the world origin - which is inaudible.
    if (follow != nullptr) {
        handle.SetObjectToFollow(follow);
    } else {
        handle.SetPosition(ToNiPoint(position));
    }

    if (!handle.Play()) {
        spdlog::error("render: Play() failed for sound {}", handle.soundID);
        handle.Stop();
        registry.Retire(token);
        return false;
    }

    handleOut = handle;
    tokenOut = token;
    ++m_voicesOut;
    return true;
}

bool GameRenderer::StartComposite(const MixBuffer& mixed, ActorId actorId, TimeMs nowMs) {
    EncodeWavPcm16Into(mixed.samples, mixed.sampleRate, m_encoded);

    RE::NiAVObject* follow = NodeFor(actorId, mixed.boneIndex);

    Voice voice;
    if (!Open(m_encoded, mixed.position, follow, voice.handle, voice.token)) {
        return false;
    }
    voice.startedMs = nowMs;
    voice.expiresMs = nowMs + static_cast<double>(mixed.LengthMs()) + kRetireGraceMs;
    voice.position = mixed.position;
    voice.follow = follow;
    m_voices.push_back(voice);
    return true;
}

bool GameRenderer::StartLoopVoice(Loop& loop, TimeMs nowMs) {
    if (!MixLoop(loop.slot, loop.variant, loop.gainDb, loop.pitch, kLoopBufferMs, kLoopOverlapMs,
                 m_cache, m_mixParams, m_mix)) {
        return false;
    }
    EncodeWavPcm16Into(m_mix.samples, m_mix.sampleRate, m_encoded);

    RE::NiAVObject* follow = NodeFor(loop.actorId, loop.boneIndex);

    RE::BSSoundHandle handle{};
    std::uint64_t token = 0;
    if (!Open(m_encoded, loop.position, follow, handle, token)) {
        return false;
    }

    // The one being replaced is left to run out its own crossfade tail rather
    // than being cut: MixLoop faded it down inside the buffer, so stopping it here
    // would put the seam back.
    if (loop.handle.IsValid()) {
        BlobRegistry::Get().Retire(loop.token);
    }
    loop.handle = handle;
    loop.token = token;
    loop.issuedMs = nowMs;
    loop.endsMs = nowMs + static_cast<double>(m_mix.LengthMs());
    return true;
}

void GameRenderer::ReleaseVoice(Voice& voice) {
    BlobRegistry::Get().Retire(voice.token);
    voice.token = 0;
    voice.handle = RE::BSSoundHandle{};
}

void GameRenderer::Update(TimeMs nowMs) {
    // -- groups that have come due ------------------------------------------
    //
    // A group is complete by the time we get here: Engine::Emit writes every
    // layer of a proposal inside one Tick and plugin.cpp calls this after it.
    for (std::size_t i = 0; i < m_groups.size();) {
        Group& group = m_groups[i];
        if (group.earliestMs > nowMs) {
            ++i;
            continue;
        }

        if (MixComposite(group.cues, m_cache, m_mixParams, m_mix) && !m_mix.Empty()) {
            if (m_mix.rawPeak > 1.0f) {
                spdlog::debug("render: composite seq {} peaked at {:.2f} before the clip - the "
                              "layer balance is hotter than the config thinks",
                              group.sourceSeq, m_mix.rawPeak);
            }
            if (!StartComposite(m_mix, group.actorId, nowMs)) {
                spdlog::debug("render: seq {} could not be started", group.sourceSeq);
            } else {
                spdlog::debug("render: seq {} - {} layers into one {:.0f} ms voice, peak {:.2f}",
                              group.sourceSeq, group.cues.size(), m_mix.LengthMs(), m_mix.rawPeak);
            }
        }

        m_groups.erase(m_groups.begin() + static_cast<std::ptrdiff_t>(i));
    }

    // -- loops ---------------------------------------------------------------
    for (std::size_t i = 0; i < m_loops.size();) {
        Loop& loop = m_loops[i];
        // Re-issued one overlap before the buffer runs out, so the crossfade
        // MixLoop baked into the head of the new one lands over the tail of the
        // old one rather than after it.
        const bool neverStarted = !loop.handle.IsValid();
        const bool nearlyOut = nowMs >= loop.endsMs - static_cast<double>(kLoopOverlapMs);
        if (neverStarted || nearlyOut) {
            if (!StartLoopVoice(loop, nowMs)) {
                // Dropped rather than retried. A loop slot that cannot be
                // rendered now will not start rendering in a second, and retrying
                // every frame would turn one missing file into a per-frame cost
                // for the rest of the session.
                spdlog::warn("render: loop {} on {} could not be started; dropping it",
                             loop.voiceId, ToString(loop.slot));
                BlobRegistry::Get().Retire(loop.token);
                m_loops.erase(m_loops.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
        }
        ++i;
    }

    // -- finished one-shots ---------------------------------------------------
    for (std::size_t i = 0; i < m_voices.size();) {
        Voice& voice = m_voices[i];

        // The one message that cannot be baked into the buffer. An attachment
        // requested before the engine has prepared the sound does not fully take,
        // so it is re-sent once, on the first frame the handle admits to playing.
        if (!voice.reattached && voice.handle.IsValid() && voice.handle.IsPlaying()) {
            if (voice.follow != nullptr) {
                voice.handle.SetObjectToFollow(voice.follow);
            } else {
                voice.handle.SetPosition(ToNiPoint(voice.position));
            }
            voice.reattached = true;
        }

        if (nowMs >= voice.expiresMs) {
            ReleaseVoice(voice);
            m_voices.erase(m_voices.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        ++i;
    }

    BlobRegistry::Get().Collect();
}

void GameRenderer::StopAll() {
    auto& registry = BlobRegistry::Get();

    for (Voice& voice : m_voices) {
        if (voice.handle.IsValid()) {
            voice.handle.Stop();
        }
        registry.Retire(voice.token);
    }
    m_voices.clear();

    for (Loop& loop : m_loops) {
        if (loop.handle.IsValid()) {
            loop.handle.Stop();
        }
        registry.Retire(loop.token);
    }
    m_loops.clear();

    m_groups.clear();
    registry.Collect();
}

}  // namespace rds::game
