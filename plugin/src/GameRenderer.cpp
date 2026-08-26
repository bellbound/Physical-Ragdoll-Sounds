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

/// The same, for a loop's opening volume. See `Loop::volumeAttempts`.
///
/// A loop needs far less of this than a one-shot does - it re-sends on every
/// level change for as long as it runs - but it needs some: a slide shorter than
/// a couple of frames' start latency would otherwise play at the level the voice
/// happened to open with, which is full scale.
constexpr std::uint8_t kVolumeAttempts = 12;

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
/// Four seconds is long enough that a re-issue is rare; the overlap is
/// crossfaded inside the buffer by MixLoop, so the seam is ours rather than the
/// engine's.
///
/// It used to say "and short enough that a gain change is picked up promptly",
/// which was the bug rather than the design. A loop's gain was baked into its
/// buffer and only re-read here, so a slide shorter than four seconds - which is
/// nearly all of them - took one level at `kStartLoop` and held it to the stop
/// cue. Every slide in the game came out the same loudness and then cut off from
/// full level, because the speed ramp that is supposed to fade a grind out as
/// the body slows had no way to reach the voice. The testbench ramps between
/// control points (`Mixer.cpp`), which is why it sounded right there and wrong
/// in game.
///
/// **Level is a live message now** and does not wait for this at all. Pitch
/// still does - see `StartLoopVoice`.
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
        // Null until the esp shipped two of them, and null again if it is not
        // installed. An uncategorised sound plays at the level we mixed it at,
        // which is what the whole mod did before there were sliders.
        a_resolution.soundCategory = const_cast<RE::BSISoundCategory*>(soundCategory);
        return true;
    }

    virtual void Unk_02() {}  // 02 - unidentified; never observed being called

    RE::BSResource::ID resourceID{};
    const RE::BSISoundOutputModel* outputModel = nullptr;
    const RE::BSISoundCategory* soundCategory = nullptr;
    PlaybackCharacteristics characteristics{};
};

/// Which slider a cue plays under.
///
/// The reason and not the slot: `CrunchSlot` picks a different recording per
/// body part, so a slot-keyed mapping would have to be kept in step with that
/// list and would silently miss the day somebody adds one. `kCrunch` and `kGore`
/// are the two the design calls damage, and they are exactly what the gore
/// slider was asked to govern.
[[nodiscard]] SoundBus BusFor(CueReason reason) {
    switch (reason) {
        case CueReason::kCrunch:
        case CueReason::kGore:
            return SoundBus::kGore;
        default:
            return SoundBus::kMain;
    }
}

[[nodiscard]] const char* ToString(SoundBus bus) {
    return bus == SoundBus::kGore ? "gore" : "main";
}

[[nodiscard]] RE::NiPoint3 ToNiPoint(const Vec3& v) { return RE::NiPoint3{v.x, v.y, v.z}; }

}  // namespace

GameRenderer::GameRenderer() {
    m_groups.reserve(16);
    m_voices.reserve(32);
    m_loops.reserve(8);
    m_busCues.reserve(8);
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
                existing->limbIndex = cue.limbIndex;
                existing->collapsed = cue.collapsed;
                Follow(*existing);
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
            loop.limbIndex = cue.limbIndex;
            loop.collapsed = cue.collapsed;
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
            // Position and **level** take effect at once, on the live handle.
            // Pitch is still baked into the buffer and so is picked up by the
            // next re-issue, up to kLoopBufferMs away.
            //
            // The two are not symmetrical and that is not an oversight. Level is
            // a plain multiply, so moving it live changes nothing else. Pitch is
            // a resample of the tile, and the buffer is always kLoopBufferMs of
            // output whatever the pitch - so `endsMs` is pitch-independent as it
            // stands. Move pitch onto `SetFrequency` and the buffer drains at
            // frequency x rate instead, which means `endsMs` has to integrate the
            // rate over the life of the loop or every re-issue puts MixLoop's
            // crossfade in the wrong place. That is a real change and it is not
            // this one; the level is what a listener hears a slide *do*.
            it->gainDb = cue.gainDb;
            it->pitch = cue.pitch;
            it->position = cue.position;
            it->boneIndex = cue.boneIndex;
            it->limbIndex = cue.limbIndex;
            it->collapsed = cue.collapsed;
            ApplyLoopVolume(*it);
            // A limb grind moves between the bones of its own limb while it
            // runs, so the attachment is re-sent here rather than only at the
            // next re-issue - which is up to a buffer length away, and a scrape
            // that follows the wrong foot for four seconds is not following
            // anything. Follow falls back to the position when no node resolves.
            if (it->handle.IsValid() && !Follow(*it)) {
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
                        RE::NiAVObject* follow, const RE::BSISoundOutputModel* model, SoundBus bus,
                        RE::BSSoundHandle& handleOut, std::uint64_t& tokenOut) {
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
    descriptor.outputModel = model;
    // Resolved per voice rather than once: the lookup retries while it is null,
    // and a voice opened before the load order was up would otherwise pin the
    // answer for the session. It is two atomic loads once it has resolved.
    descriptor.soundCategory = CategoryFor(bus);

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

bool GameRenderer::GroupIsDry(std::span<const Cue> cues) {
    if (cues.empty()) {
        return false;
    }
    return std::ranges::all_of(cues, [](const Cue& cue) {
        return cue.reason == CueReason::kLimbTap || cue.reason == CueReason::kSurfaceSkin ||
               cue.reason == CueReason::kArmorSkin;
    });
}

bool GameRenderer::StartComposite(const MixBuffer& mixed, ActorId actorId, TimeMs nowMs, bool dry,
                                  SoundBus bus) {
    EncodeWavPcm16Into(mixed.samples, mixed.sampleRate, m_encoded);

    RE::NiAVObject* follow =
        NodeFor(actorId, mixed.boneIndex, mixed.limbIndex, mixed.collapsed);

    Voice voice;
    if (!Open(m_encoded, mixed.position, follow, dry ? TapOutputModel() : DefaultOutputModel(), bus,
              voice.handle, voice.token)) {
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

void GameRenderer::StartBus(const Group& group, SoundBus bus, TimeMs nowMs) {
    m_busCues.clear();
    for (const Cue& cue : group.cues) {
        if (BusFor(cue.reason) == bus) {
            m_busCues.push_back(cue);
        }
    }
    // The overwhelmingly common case: nothing in this moment was damage, so the
    // gore half is empty and this costs one pass over four cues.
    if (m_busCues.empty()) {
        return;
    }

    // `group.earliestMs` and not the subset's own earliest. See MixComposite.
    if (!MixComposite(m_busCues, m_cache, m_mixParams, m_mix, group.earliestMs) || m_mix.Empty()) {
        return;
    }

    if (m_mix.rawPeak > 1.0f) {
        spdlog::debug("render: composite seq {} ({}) peaked at {:.2f} before the clip - the "
                      "layer balance is hotter than the config thinks",
                      group.sourceSeq, ToString(bus), m_mix.rawPeak);
    }

    // Asked here rather than inside StartComposite because the mixed buffer has
    // no reasons left in it - the layers are summed by then, and which model the
    // sum wants is a question about the cues. Asked of this bus's cues, which is
    // the same question it always was: a tap and a crunch were never in one
    // voice together, they were in one group.
    const bool dry = GroupIsDry(m_busCues);
    if (!StartComposite(m_mix, group.actorId, nowMs, dry, bus)) {
        spdlog::debug("render: seq {} ({}) could not be started", group.sourceSeq, ToString(bus));
        return;
    }
    spdlog::debug("render: seq {} ({}) - {} layers into one {:.0f} ms {} voice, peak {:.2f}",
                  group.sourceSeq, ToString(bus), m_busCues.size(), m_mix.LengthMs(),
                  dry ? "dry" : "wet", m_mix.rawPeak);
}

bool GameRenderer::Follow(Loop& loop) {
    RE::NiAVObject* node = NodeFor(loop.actorId, loop.boneIndex, loop.limbIndex, loop.collapsed);
    if (node == nullptr) {
        return false;
    }
    // Only when it actually changed. SetObjectToFollow has no return to check
    // and re-sending it every tick of every loop is a message per loop per frame
    // for no effect at all.
    if (node != loop.follow) {
        loop.follow = node;
        if (loop.handle.IsValid()) {
            loop.handle.SetObjectToFollow(node);
        }
    }
    return true;
}

bool GameRenderer::StartLoopVoice(Loop& loop, TimeMs nowMs) {
    // Rendered at full scale, with the level carried by `SetVolume` instead.
    //
    // `BSGameSound::SetVolume` clamps to 1.0, so a volume message can only ever
    // attenuate - which is all the mod ever asks of a loop, since every gain in
    // the config is negative. Only a positive excess (a slot trim over 0 dB) has
    // to go into the buffer, and that is what `bakedDb` is.
    //
    // Baking nothing is also the better of the two for quality: the blob is
    // PCM16, so baking a -30 dB slide level into it threw away five bits before
    // the engine ever saw the samples. Full scale in, attenuate on the handle.
    loop.bakedDb = std::max(0.0f, loop.gainDb);
    if (!MixLoop(loop.slot, loop.variant, loop.bakedDb, loop.pitch, kLoopBufferMs, kLoopOverlapMs,
                 m_cache, m_mixParams, m_mix)) {
        return false;
    }
    EncodeWavPcm16Into(m_mix.samples, m_mix.sampleRate, m_encoded);

    // Where the grind is. The engine already says which limb it came from and
    // this used to discard it and pin every loop to the actor's root - so a
    // dragging foot played from the pelvis, and the player's own scrape, which
    // the mod goes out of its way to attach to the player's bones, played from
    // inside their head. What the discard was guarding against - a scrape
    // hopping between limbs smearing instead of tracking - is now handled where
    // it belongs, on the hop.
    RE::NiAVObject* follow =
        NodeFor(loop.actorId, loop.boneIndex, loop.limbIndex, loop.collapsed);
    loop.follow = follow;

    RE::BSSoundHandle handle{};
    std::uint64_t token = 0;
    // Always the main bus. A scrape and a cloth bed are texture, not damage, and
    // there is no loop the gore slider should reach.
    if (!Open(m_encoded, loop.position, follow, DefaultOutputModel(), SoundBus::kMain, handle,
              token)) {
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
    // A new handle is a new voice, so the level has to be sent again - and
    // re-sent for a few frames, because the engine drops the message until the
    // voice exists.
    loop.sentVolume = -1.0f;
    loop.volumeAttempts = kVolumeAttempts;
    ApplyLoopVolume(loop);
    return true;
}

/// Push a loop's level onto its live handle.
///
/// The whole of a loop's dynamics in the game, and the thing that was missing.
/// Sent when it has moved and while the opening attempts are still counting
/// down; not every frame unconditionally, because that is a message per loop per
/// frame for no effect - the same reason `Follow` checks before re-sending.
void GameRenderer::ApplyLoopVolume(Loop& loop) {
    if (!loop.handle.IsValid()) {
        return;
    }
    // What is left after whatever went into the buffer. Never over 1.0, so it is
    // never fighting the clamp in `BSGameSound::SetVolume`.
    const float volume = std::clamp(DbToLinear(loop.gainDb - loop.bakedDb), 0.0f, 1.0f);
    if (loop.volumeAttempts == 0 && std::fabs(volume - loop.sentVolume) < 0.0005f) {
        return;
    }
    if (loop.volumeAttempts > 0) {
        --loop.volumeAttempts;
    }
    loop.sentVolume = volume;
    loop.handle.SetVolume(volume);
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

        // One moment, but up to two voices: the damage layers play on their own
        // sound category so the player has a gore slider that does not take the
        // impacts with it. Both halves are mixed against the moment's own
        // earliest time and opened in this same frame, so the +20 ms a crunch
        // sits behind its transient survives the split to the sample - which is
        // the property mixing exists for and the one a naive split would lose.
        StartBus(group, SoundBus::kMain, nowMs);
        StartBus(group, SoundBus::kGore, nowMs);

        m_groups.erase(m_groups.begin() + static_cast<std::ptrdiff_t>(i));
    }

    // -- loops ---------------------------------------------------------------
    for (std::size_t i = 0; i < m_loops.size();) {
        Loop& loop = m_loops[i];
        // Re-issued one overlap before the buffer runs out, so the crossfade
        // MixLoop baked into the head of the new one lands over the tail of the
        // old one rather than after it.
        // The opening volume, re-sent until it has certainly landed. A slide
        // that ends inside the start latency would otherwise be left at the
        // level the voice opened with, which is now full scale rather than a
        // baked-in attenuation - so this is the one place where dropping the
        // message got *louder* rather than quieter.
        if (loop.volumeAttempts > 0) {
            ApplyLoopVolume(loop);
        }

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
