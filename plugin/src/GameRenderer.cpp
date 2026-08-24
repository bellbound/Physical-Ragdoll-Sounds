#include "GameRenderer.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

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
///   0x0010 - what `Actor::PlayVoiceFile` always passes, and what SkyrimNet
///            mirrors. This was left out on the reasoning that we are not
///            dialogue, which was reasoning from the flag's company rather than
///            from what it does - nobody has established what it does. Both of
///            the paths known to work set it and we were the one that did not,
///            so it goes back in until there is a reason to leave it out.
constexpr std::uint32_t kSoundFlags = 0x8000 | 0x0080 | 0x0010;

/// Ragdoll impacts should outlive ambience but never a line of dialogue.
constexpr std::uint8_t kPriority = 0x30;

/// How long after a voice's own length its blob may be retired.
///
/// Play() only queues; the engine opens the sound a few audio ticks later, so a
/// blob retired on the nose can be refused before it was ever served. This is the
/// same start latency SkyrimNet allows 750 ms of grace for, rounded up.
constexpr double kRetireGraceMs = 1000.0;

/// How many frames a one-shot's placement is re-sent for before we give up.
///
/// One send is not enough and was the bug: `SetPosition` before `Play` is a
/// message to a sound the engine has not prepared yet and is dropped, and the
/// single re-send that covered that was gated on `IsPlaying()`, which a 300 ms
/// composite can finish without ever being observed true - at which point the
/// voice keeps the position it was opened with, which is the world origin. An
/// unplaced voice does not go quiet, it goes *somewhere else*: attenuated by
/// however far the origin is and arriving from a fixed direction that has
/// nothing to do with the body. Twelve frames is a sixth of a second at 60 fps
/// and an eighth at VR rates, and it costs at most twelve messages per voice.
constexpr std::uint8_t kPlaceAttempts = 12;

/// How often the placement line is worth saying, in milliseconds. It is at info
/// rather than debug on purpose: "our audio is N metres from your ears and the
/// body is not" is the one thing a shipping log has to be able to answer about a
/// mod that sounds like it is coming from the wrong place, and nobody reproduces
/// that with debug logging already on.
constexpr double kPlaceLogIntervalMs = 2000.0;

/// Skyrim world units per metre. Only ever used to put a human number beside a
/// distance in the log.
constexpr float kUnitsPerMetre = 69.99124f;

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

void GameRenderer::SetActorPositionResolver(ActorPositionResolver resolver) {
    m_actorPositionResolver = std::move(resolver);
}

void GameRenderer::Counters(std::uint64_t& cuesIn, std::uint64_t& voicesOut) const {
    cuesIn = m_cuesIn;
    voicesOut = m_voicesOut;
}

RE::NiAVObject* GameRenderer::NodeFor(ActorId actorId, std::int32_t boneIndex,
                                      std::uint16_t limbIndex, bool collapsed) const {
    // Everything follows a node. Not a world coordinate: `SetPosition` is a
    // message the engine takes and does not keep, and an unfollowed sound sits
    // at the origin - the middle of the cell, a fixed direction away from
    // wherever you are standing, and quiet with it. Vanilla's voice path ends on
    // SetObjectToFollow and SkyrimNet calls SetPosition exactly zero times in
    // twenty-two placements; we were the only one placing by coordinate and the
    // only one with the symptom.
    //
    // Which node is the whole question, and there are three answers in order.

    // 1. A named bone. The player's own ragdoll, where a collapse to one point
    //    at arm's length sounds like the audio is inside your head.
    if (boneIndex >= 0 && m_boneResolver) {
        if (auto* bone = m_boneResolver(actorId, boneIndex)) {
            return bone;
        }
    }

    // 2. The body, but only when the arbitrator actually asked for that. Stage 4
    //    rule 5 collapses a hero moment onto one point because several points
    //    read as several events; the root is that one point, and unlike the
    //    world coordinate it used to be, it moves with the body instead of
    //    standing where the body was a second ago.
    if (collapsed && m_rootResolver) {
        if (auto* root = m_rootResolver(actorId)) {
            return root;
        }
    }

    // 3. Otherwise the limb that made the contact. This is the default and it is
    //    the one the design argues for: physics owns *where*, and a listener
    //    checks it against what they can see - "a sound must come from where the
    //    limb hit". An NPC's limbs resolve exactly the same way the player's do;
    //    nothing here was ever player-only.
    if (m_boneResolver) {
        if (auto* limb = m_boneResolver(actorId, static_cast<std::int32_t>(limbIndex))) {
            return limb;
        }
    }

    // The body as a backstop - an unrecognised skeleton, or 3D that went away
    // mid-fall. Better a voice on the wrong bone than a voice at the origin.
    if (m_rootResolver) {
        return m_rootResolver(actorId);
    }
    return nullptr;
}

void GameRenderer::SetRootResolver(RootResolver resolver) { m_rootResolver = std::move(resolver); }

void GameRenderer::Emit(const Cue& cue) {
    // Counted before it is dropped, so the testbench's heartbeat shows cues
    // arriving with no voices leaving - which is what muted looks like from the
    // other end, and is worth being able to tell apart from an engine that has
    // stopped proposing anything.
    ++m_cuesIn;
    if (m_muted) {
        return;
    }

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
    // attenuates with distance, means the world origin - which is not silence,
    // it is a quiet sound arriving from the wrong direction.
    //
    // Sent here as well as after Play because a placement that does land here is
    // one the first frame does not have to fix. It is not relied on: see
    // kPlaceAttempts.
    if (follow != nullptr) {
        handle.SetObjectToFollow(follow);
    } else if (!handle.SetPosition(ToNiPoint(position)) && !m_warnedPlacement) {
        m_warnedPlacement = true;
        spdlog::warn("render: the engine refused a position before Play, so every voice depends on "
                     "the re-send after it. If impacts sound like they come from one fixed spot "
                     "rather than from the body, this is why.");
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

std::string GameRenderer::DescribeAgainstActor(const Voice& voice) const {
    // Which of the two placements this voice got. A coordinate is now the
    // failure case rather than the normal one, so it says so.
    std::string suffix = voice.follow != nullptr
                             ? " [following a node]"
                             : " [STATIC COORDINATE - no node resolved, expect the cell origin]";
    Vec3 actor{};
    if (!m_actorPositionResolver || !m_actorPositionResolver(voice.actorId, actor)) {
        return suffix;
    }
    // The number that separates "the body really is over there" from "our
    // contact points are wrong". A contact is on the body, so this should be
    // within a body's reach - tens of units, not hundreds.
    const float dx = voice.position.x - actor.x;
    const float dy = voice.position.y - actor.y;
    const float dz = voice.position.z - actor.z;
    const float off = std::sqrt(dx * dx + dy * dy + dz * dz);
    return std::format(", {:.0f} units off the body at ({:.0f}, {:.0f}, {:.0f}){}", off, actor.x,
                       actor.y, actor.z, suffix);
}

void GameRenderer::Place(Voice& voice, bool playing) {
    if (!voice.handle.IsValid()) {
        return;
    }
    ++voice.placeAttempts;

    bool took = true;
    if (voice.follow != nullptr) {
        // SetObjectToFollow has no return to check, so an attachment is only ever
        // known to have taken by being re-sent until the sound is playing.
        voice.handle.SetObjectToFollow(voice.follow);
    } else {
        took = voice.handle.SetPosition(ToNiPoint(voice.position));
    }

    // A placement is only trusted once it has been sent on a frame the engine
    // admits the sound is playing on. Before that the message can be dropped and
    // nothing says so.
    if (playing && took) {
        voice.placed = true;

        if (voice.watch) {
            const float dx = voice.position.x - m_listener.x;
            const float dy = voice.position.y - m_listener.y;
            const float dz = voice.position.z - m_listener.z;
            const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
            const float peakDb =
                voice.peak > 1.0e-6f ? 20.0f * std::log10(voice.peak) : -120.0f;
            spdlog::info("placed: sound {} at ({:.0f}, {:.0f}, {:.0f}) on attempt {}, {:.0f} units "
                         "({:.1f} m) from the ears at ({:.0f}, {:.0f}, {:.0f}), buffer peak "
                         "{:.1f} dBFS{}",
                         voice.handle.soundID, voice.position.x, voice.position.y,
                         voice.position.z, voice.placeAttempts, distance,
                         distance / kUnitsPerMetre, m_listener.x, m_listener.y, m_listener.z,
                         peakDb, DescribeAgainstActor(voice));
        }
    }

    if (!took && !m_warnedPlacement) {
        m_warnedPlacement = true;
        spdlog::warn("render: SetPosition was refused for sound {} on attempt {} - the voice is "
                     "playing wherever the engine last put it, most likely the world origin",
                     voice.handle.soundID, voice.placeAttempts);
    }
}

bool GameRenderer::StartComposite(const MixBuffer& mixed, ActorId actorId, TimeMs nowMs) {
    EncodeWavPcm16Into(mixed.samples, mixed.sampleRate, m_encoded);

    RE::NiAVObject* follow =
        NodeFor(actorId, mixed.boneIndex, mixed.limbIndex, mixed.collapsed);

    Voice voice;
    if (!Open(m_encoded, mixed.position, follow, voice.handle, voice.token)) {
        return false;
    }
    voice.startedMs = nowMs;
    voice.expiresMs = nowMs + static_cast<double>(mixed.LengthMs()) + kRetireGraceMs;
    voice.position = mixed.position;
    voice.follow = follow;
    voice.peak = mixed.rawPeak;
    voice.actorId = actorId;
    voice.bufferMs = mixed.LengthMs();
    if (nowMs - m_lastPlaceLogMs >= kPlaceLogIntervalMs) {
        m_lastPlaceLogMs = nowMs;
        voice.watch = true;
    }
    m_voices.push_back(voice);
    return true;
}

bool GameRenderer::StartLoopVoice(Loop& loop, TimeMs nowMs) {
    if (!MixLoop(loop.slot, loop.variant, loop.gainDb, loop.pitch, kLoopBufferMs, kLoopOverlapMs,
                 m_cache, m_mixParams, m_mix)) {
        return false;
    }
    EncodeWavPcm16Into(m_mix.samples, m_mix.sampleRate, m_encoded);

    // A loop is a continuous texture rather than a moment, so it hangs on the
    // body: a scrape that jumped between limbs as the contact moved would smear
    // rather than track.
    RE::NiAVObject* follow = NodeFor(loop.actorId, loop.boneIndex, 0, true);

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

        // The one message that cannot be baked into the buffer, and the one the
        // engine can silently drop: a placement asked for before the sound has
        // been prepared does not take. So it is re-sent on every frame until it
        // has landed on a frame the handle reports itself playing on, bounded by
        // kPlaceAttempts.
        if (!voice.placed && voice.placeAttempts < kPlaceAttempts) {
            Place(voice, voice.handle.IsValid() && voice.handle.IsPlaying());
            if (!voice.placed && voice.placeAttempts >= kPlaceAttempts && !m_warnedPlacement) {
                m_warnedPlacement = true;
                spdlog::warn("render: sound {} never reported itself playing across {} frames, so "
                             "its position was never confirmed. Impacts will sound displaced and "
                             "quiet; check iOutputModelFormID and the blob registry.",
                             voice.handle.soundID, kPlaceAttempts);
            }
        }

        if (voice.watch && voice.handle.IsValid()) {
            const bool playing = voice.handle.IsPlaying();
            if (playing && voice.firstPlayingMs < 0.0) {
                voice.firstPlayingMs = nowMs;
            }
            if (voice.wasPlaying && !playing) {
                // Stopped. Everything the frame-boundary assumption turns on, in
                // one line: how long the audio was, when the engine admitted to
                // starting it, and when it actually finished.
                const double ranFor = nowMs - voice.startedMs;
                const double slack = ranFor - static_cast<double>(voice.bufferMs);
                spdlog::info("timing: sound {} - {:.0f} ms of audio, first reported playing at "
                             "+{:.0f} ms, silent at +{:.0f} ms, so the engine added about "
                             "{:.0f} ms before the first sample",
                             voice.handle.soundID, voice.bufferMs,
                             voice.firstPlayingMs < 0.0 ? -1.0 : voice.firstPlayingMs -
                                                                    voice.startedMs,
                             ranFor, std::max(0.0, slack));
                voice.watch = false;
            }
            voice.wasPlaying = playing;
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

void GameRenderer::SetMuted(bool muted) {
    if (m_muted == muted) {
        return;
    }
    m_muted = muted;
    if (muted) {
        // Not just the cues to come: a loop is re-issued rather than played once,
        // so a scrape already running would keep going for as long as the buffer
        // it is on. Everything in flight goes with the switch.
        StopAll();
    }
    spdlog::info("renderer: {}", muted ? "muted - vanilla's own impacts are what you are hearing"
                                       : "unmuted - back to our mix");
}

}  // namespace rds::game
