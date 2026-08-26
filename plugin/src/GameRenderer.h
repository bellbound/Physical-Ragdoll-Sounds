#pragma once

// Stage 5 in the game: an ICueSink that turns cues into BSSoundHandles.
//
// It does NOT turn one cue into one voice. A composite is four cues - transient
// at +0, surface at +8, body at +20, sub at +65 - and voices start on frame
// boundaries, so at 60 fps the first two would land in the same frame and the
// mechanism the whole design rests on would be gone. Instead the cues that share
// an acoustic moment are mixed here, to the sample, and the engine is handed one
// voice.
//
// Grouping needs nothing new from the engine: Engine::Emit stamps every layer of
// a proposal with the same actorId and sourceSeq, all inside one Tick, and
// plugin.cpp calls Update after Tick. So (actorId, sourceSeq) is the composite
// id and a group is always complete by the time we look at it. Two strategies
// proposing on the same contact share the key and get mixed together, which is
// right - it is the same acoustic moment.
//
// Three things fall out of mixing, and all three are improvements:
//
//   - Layer offsets are sample-accurate rather than frame-quantised.
//   - Gain and pitch are baked into the buffer instead of being sent as
//     BSSoundHandle messages. That matters more than it sounds: the engine drops
//     those messages until the source voice exists, and for a 300 ms impact that
//     is the whole sound. SkyrimNet hit exactly this and has to re-send on the
//     first playing poll; we simply never send them.
//   - One voice per impact instead of four. That was worth four times the voice
//     budget back when there was one; it is now simply four times less work per
//     impact, and four times fewer handles for the engine to mix.
//
// One thing does split a group, and only one: the sound category. The crunch and
// gore layers play on their own category so the Audio settings page can carry a
// gore slider, and a category is a property of a voice rather than of a sample,
// so those layers need a voice of their own. They still share the group's time
// base, so the split costs a second handle on a damage contact and changes no
// timing - see StartBus.
//
// What still has to be sent to the handle is position, which is why the one
// re-send SkyrimNet does survives here in miniature: an attachment requested
// before the engine has prepared the sound does not fully take.
//
// LOAD-BEARING: every voice is given a BGSSoundOutput model (see AudioBlobs.h).
// And do NOT reintroduce a distance term in the cue gain - falloff is the game's,
// through that model, and doing it in both places attenuated every cue twice.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "AudioBlobs.h"
#include "rds/Cue.h"
#include "rds/Mix.h"
#include "rds/Pcm.h"
#include "rds/SlotManifest.h"

namespace rds::game {

/// Limb index to the node a voice should follow, for the player's own ragdoll.
/// Supplied by the feed, which is the half that knows how an actor's limbs were
/// resolved. Null means "we cannot attach", and the voice takes a world position.
using BoneResolver = std::function<RE::NiAVObject*(ActorId, std::int32_t)>;

/// The node an actor's voices follow when no individual bone is named.
using RootResolver = std::function<RE::NiAVObject*(ActorId)>;

/// Where the game thinks an actor is. Diagnostic only - it never enters the mix.
/// False when the actor is not tracked.
using ActorPositionResolver = std::function<bool(ActorId, Vec3&)>;

class GameRenderer final : public ICueSink {
public:
    GameRenderer();
    ~GameRenderer() override;

    /// The bank stays owned by the caller. Needed to turn a cue's (slot, variant)
    /// back into the samples the engine chose - through the cache, which goes via
    /// SoundBank::Get and never Resolve, because Resolve would advance the
    /// shuffle bag and hand back a different file than the cue names.
    void SetSoundBank(SoundBank* bank);

    void SetBoneResolver(BoneResolver resolver);

    void SetRootResolver(RootResolver resolver);

    /// Only the placement log reads this. See ActorPositionResolver.
    void SetActorPositionResolver(ActorPositionResolver resolver);

    /// Where the ears are this frame, in world units. Used only to say something
    /// useful in the log about a voice that has just been placed - the engine
    /// does its own listener maths and this never enters the mix.
    void SetListener(const Vec3& position) { m_listener = position; }

    /// Called from the frame hook, after Engine::Tick. Mixes and starts every
    /// group that has come due, refreshes running loops, and reclaims finished
    /// blobs.
    void Update(TimeMs nowMs);

    /// Stop everything. Load game, cell change, mod disabled. A scrape that
    /// survives a load screen plays in the main menu.
    void StopAll();

    /// Drop every cue instead of playing it, and stop what is already running.
    ///
    /// This is the far half of the testbench's Use Vanilla Audio switch: with
    /// vanilla's body impacts put back (VanillaSuppression.h) and ours dropped
    /// here, the two mixes can be heard against each other inside one session
    /// rather than across two launches - which is the only way a level or a
    /// character judgement about them means anything.
    ///
    /// Muted here rather than by not ticking the engine: the engine's tick is
    /// what drains the feed, and the feed is what the testbench is recording.
    /// Stop it and the A side of the comparison stops being observable.
    void SetMuted(bool muted);
    [[nodiscard]] bool Muted() const { return m_muted; }

    // -- ICueSink -------------------------------------------------------------
    void Emit(const Cue& cue) override;

    /// Cues taken in and engine voices actually started, since the last reset.
    ///
    /// Worth having both: the engine proposes in cues and one composite is one
    /// voice, so the two numbers are the mixing doing its job. The ratio between
    /// them is what says how much of the cue list is layers of one moment rather
    /// than separate moments - which is the question the design turns on, and
    /// nothing else reports it.
    void Counters(std::uint64_t& cuesIn, std::uint64_t& voicesOut) const;

private:
    /// The mix rate. 48 kHz because that is what the whole pack is, so no file is
    /// resampled on the way in and the mixer's only interpolation is the one a
    /// cue's pitch actually asks for.
    static constexpr int kSampleRate = 48000;

    /// One acoustic moment's worth of cues, keyed by the engine's own provenance.
    struct Group {
        ActorId actorId{};
        std::uint32_t sourceSeq{};
        TimeMs earliestMs{};
        std::vector<Cue> cues;
    };

    /// A one-shot in flight. Held only so its blob can be retired once the engine
    /// has certainly finished with it, and so its placement can be re-sent until
    /// it sticks.
    struct Voice {
        RE::BSSoundHandle handle{};
        std::uint64_t token{};
        TimeMs startedMs{};
        TimeMs expiresMs{};
        Vec3 position{};
        ActorId actorId{};
        RE::NiAVObject* follow{};
        /// How many frames the placement has been sent on. Bounded, so a handle
        /// that never admits to playing costs a fixed number of messages rather
        /// than one per frame for the whole voice.
        std::uint8_t placeAttempts{};
        /// Set once the placement has been sent on a frame the handle reported
        /// itself playing on, which is the first moment the engine is known to
        /// have somewhere to put it.
        bool placed{};
        /// One voice per log interval is followed from Play() to silence, to
        /// measure what the engine adds on its own.
        ///
        /// The design assumes voices start on a frame boundary and treats the
        /// resulting 7-20 ms of quantisation as acceptable. That assumption has
        /// never been tested, and it matters: if the engine's own start latency
        /// dwarfs a frame, then how often our hook runs is not what decides when
        /// a sound arrives, and making the hook faster would buy nothing.
        ///
        /// The measurement is indirect but sound. We know how long the buffer is.
        /// If the handle stops reporting itself playing about one buffer-length
        /// after Play(), playback began at once and `IsPlaying` was simply slow to
        /// admit it. If it stops a good deal later than that, the difference is
        /// real latency the engine added before the first sample.
        bool watch{};
        float bufferMs{};
        TimeMs firstPlayingMs{-1.0};
        bool wasPlaying{};

        /// The mixed buffer's peak before the clip, carried purely so the
        /// placement line can say how hot what we handed the engine actually was.
        /// "Too quiet" has two halves - what we render and what the game does to
        /// it afterwards - and they need telling apart from one log.
        float peak{};
    };

    /// A loop in flight. The engine has no reachable whole-file loop flag, so a
    /// loop is a long buffer that gets re-issued before it runs out, with the
    /// overlap crossfaded inside the buffer itself.
    struct Loop {
        std::uint32_t voiceId{};
        SlotId slot{};
        std::uint8_t variant{};
        float gainDb{};
        float pitch{1.0f};
        Vec3 position{};
        std::int32_t boneIndex{-1};
        /// The limb the loop is on. Honoured, unlike before: the engine has
        /// always attached a note saying which limb a grind came from, and this
        /// side used to throw it away and pin every loop to the actor's root.
        /// The worry behind that - a scrape hopping between limbs frame by frame
        /// would smear rather than track - was legitimate, but the cure belongs
        /// on the hop, and it is now applied there (`fLimbHoldMs`) rather than to
        /// every loop in the mod.
        std::uint16_t limbIndex{};
        /// Whether the engine asked for one point. Loops never do any more - a
        /// hero moment is a moment and a loop is a texture that outlasts one.
        bool collapsed{};
        /// The node the loop is currently following, so a change of limb can be
        /// sent to a live handle rather than waiting for the next re-issue up to
        /// four seconds later.
        RE::NiAVObject* follow{};
        ActorId actorId{};
        RE::BSSoundHandle handle{};
        std::uint64_t token{};
        TimeMs issuedMs{};
        TimeMs endsMs{};
        bool stopping{};

        /// How much of the level is in the *buffer* rather than on the handle.
        ///
        /// Zero for everything the mod actually asks for, because every loop gain
        /// in the config is an attenuation and `SetVolume` clamps to 1.0 - so the
        /// buffer is rendered at full scale and the whole level is a volume
        /// message. A slot trim can push a loop over 0 dB, and only that excess
        /// is baked, so the message stays inside the clamp.
        float bakedDb{};

        /// The volume last sent, and how many more frames to keep re-sending it.
        ///
        /// The engine drops handle messages until the source voice exists, which
        /// is the trap SkyrimNet documents and the one `kPlaceAttempts` exists for
        /// on one-shots. A loop is the easy case - a slide sends an update every
        /// tick, so a few dropped at the start cost nothing - but a *short* slide
        /// may only ever send one, and that one landing in the gap would leave the
        /// grind at whatever volume the voice opened with. So a start re-sends for
        /// `kVolumeAttempts` frames, exactly as a placement does.
        float sentVolume{-1.0f};
        std::uint8_t volumeAttempts{};
    };

    bool StartComposite(const MixBuffer& mixed, ActorId actorId, TimeMs nowMs, bool dry,
                        SoundBus bus);

    /// Mix and start the half of a group that plays on one bus, or do nothing
    /// when no cue in it does.
    ///
    /// This is where one acoustic moment becomes one voice per slider. The split
    /// is by category and by nothing else: the buses are mixed against the SAME
    /// time base - the group's earliest cue - so the second buffer opens with
    /// however much leading silence its first layer sits behind the moment by,
    /// and the two voices start on the same frame with their offsets intact.
    /// Letting each half find its own frame zero would put frame quantisation
    /// back between the transient and the crunch, which is the exact thing
    /// mixing in-process was introduced to remove.
    void StartBus(const Group& group, SoundBus bus, TimeMs nowMs);

    /// Whether this group is one the dry model applies to.
    ///
    /// True only when every cue in it is a tap or a colour layer on one. Reverb
    /// send is a property of the model and a model is chosen per voice, so a
    /// group holding anything else - a composite, a crunch, a second strategy's
    /// proposal on the same contact - has to be opened wet or the impact inside
    /// it would go dry with the tap. Asking it of the whole group rather than of
    /// the loudest cue is what keeps that from being a judgement call.
    [[nodiscard]] static bool GroupIsDry(std::span<const Cue> cues);

    /// Point a loop at the node it should be following now, sending it to the
    /// live handle if the answer changed. False when nothing resolved, which is
    /// the caller's cue to fall back on a world position.
    bool Follow(Loop& loop);

    /// Send a one-shot's position or attachment to its handle, and say in the log
    /// whether the engine took it. Called on the frame the voice starts and again
    /// on the frames after it - see Voice::placed.
    void Place(Voice& voice, bool playing);

    /// The " N units off the body" half of the placement line, or empty when the
    /// actor cannot be located.
    [[nodiscard]] std::string DescribeAgainstActor(const Voice& voice) const;
    bool StartLoopVoice(Loop& loop, TimeMs nowMs);
    void ApplyLoopVolume(Loop& loop);
    void ReleaseVoice(Voice& voice);
    /// Which node this voice hangs on: a named bone, the body when the moment
    /// was collapsed, otherwise the limb that made the contact.
    [[nodiscard]] RE::NiAVObject* NodeFor(ActorId actorId, std::int32_t boneIndex,
                                          std::uint16_t limbIndex, bool collapsed) const;

    /// Open a handle on `wav` and place it. Everything both paths share.
    ///
    /// `bus` picks the sound category, which is the volume slider the player
    /// sees. It is set on the descriptor rather than on the handle afterwards:
    /// the engine applies a descriptor's category itself as it builds the sound,
    /// where a message to a handle is dropped until the source voice exists.
    bool Open(std::span<const std::uint8_t> wav, const Vec3& position, RE::NiAVObject* follow,
              const RE::BSISoundOutputModel* model, SoundBus bus, RE::BSSoundHandle& handleOut,
              std::uint64_t& tokenOut);

    SoundBank* m_bank{};
    BoneResolver m_boneResolver;
    RootResolver m_rootResolver;
    ActorPositionResolver m_actorPositionResolver;
    PcmCache m_cache;
    MixParams m_mixParams;

    std::vector<Group> m_groups;
    std::vector<Voice> m_voices;
    std::vector<Loop> m_loops;

    /// Reused every frame so the steady path allocates nothing.
    MixBuffer m_mix;
    std::vector<std::uint8_t> m_encoded;
    /// One bus's share of a group, gathered here rather than allocated per call
    /// for the same reason - a knockdown is several groups a frame.
    std::vector<Cue> m_busCues;

    std::uint64_t m_cuesIn{};
    std::uint64_t m_voicesOut{};
    bool m_warnedNoModel{};
    bool m_muted{};
    Vec3 m_listener{};
    /// The placement line is worth one voice per burst, not one per voice: it
    /// answers "is our audio where the body is", which is a question about a
    /// knockdown rather than about a layer stack.
    TimeMs m_lastPlaceLogMs{-1.0e9};
    /// Said once. A handle that refuses its position is not a per-voice event,
    /// it is the whole mod playing at the world origin.
    bool m_warnedPlacement{};
};

}  // namespace rds::game
