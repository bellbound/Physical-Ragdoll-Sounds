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
//   - One voice per impact instead of four, against a global cap of sixteen.
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
    /// Worth having both: the engine's voice cap counts cues, and one composite is
    /// now one voice, so the real cost sits well under what the config budgets.
    /// Reporting the two side by side is what lets the cap be retuned by ear
    /// rather than guessed at.
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
        ActorId actorId{};
        RE::BSSoundHandle handle{};
        std::uint64_t token{};
        TimeMs issuedMs{};
        TimeMs endsMs{};
        bool stopping{};
    };

    bool StartComposite(const MixBuffer& mixed, ActorId actorId, TimeMs nowMs);

    /// Send a one-shot's position or attachment to its handle, and say in the log
    /// whether the engine took it. Called on the frame the voice starts and again
    /// on the frames after it - see Voice::placed.
    void Place(Voice& voice, bool playing);

    /// The " N units off the body" half of the placement line, or empty when the
    /// actor cannot be located.
    [[nodiscard]] std::string DescribeAgainstActor(const Voice& voice) const;
    bool StartLoopVoice(Loop& loop, TimeMs nowMs);
    void ReleaseVoice(Voice& voice);
    /// Which node this voice hangs on: a named bone, the body when the moment
    /// was collapsed, otherwise the limb that made the contact.
    [[nodiscard]] RE::NiAVObject* NodeFor(ActorId actorId, std::int32_t boneIndex,
                                          std::uint16_t limbIndex, bool collapsed) const;

    /// Open a handle on `wav` and place it. Everything both paths share.
    bool Open(std::span<const std::uint8_t> wav, const Vec3& position, RE::NiAVObject* follow,
              RE::BSSoundHandle& handleOut, std::uint64_t& tokenOut);

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
