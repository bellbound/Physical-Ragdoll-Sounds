#pragma once

// Stage 5 in the game: an ICueSink that turns cues into BSSoundHandles.
//
// It does NOT turn one cue into one voice. A composite is four cues - transient
// +0, surface +8, body +20, sub +65 - and voices start on frame boundaries, so at
// 60 fps the first two would land in the same frame and the mechanism the design
// rests on would be gone. The cues that share an acoustic moment are mixed here,
// to the sample, and the engine is handed one voice.
//
// Grouping needs nothing new from the engine: Engine::Emit stamps every layer of
// a proposal with the same actorId and sourceSeq inside one Tick, and plugin.cpp
// calls Update after Tick. So (actorId, sourceSeq) is the composite id and a group
// is always complete by the time we look at it.
//
// Three things fall out of mixing:
//
//   - Layer offsets are sample-accurate rather than frame-quantised.
//   - Gain and pitch are baked into the buffer rather than sent as BSSoundHandle
//     messages, which the engine drops until the source voice exists - for a
//     300 ms impact, the whole sound.
//   - One voice per impact instead of four.
//
// One thing splits a group: the sound category. The crunch and gore layers play
// on their own so the Audio page can carry a gore slider, and a category is a
// property of a voice rather than of a sample. They still share the group's time
// base, so the split changes no timing - see StartBus.
//
// Position still has to be sent to the handle, which is why SkyrimNet's re-send
// survives here in miniature: an attachment requested before the engine has
// prepared the sound does not fully take.
//
// LOAD-BEARING: every voice is given a BGSSoundOutput model (see AudioBlobs.h).
// Do NOT reintroduce a distance term in the cue gain - falloff is the game's,
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
/// Supplied by the feed, which knows how an actor's limbs were resolved. Null
/// means "we cannot attach", and the voice takes a world position.
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
    /// back into the samples the engine chose - via SoundBank::Get and never
    /// Resolve, which would advance the shuffle bag and hand back a different file.
    void SetSoundBank(SoundBank* bank);

    void SetBoneResolver(BoneResolver resolver);

    void SetRootResolver(RootResolver resolver);

    /// Only the placement log reads this. See ActorPositionResolver.
    void SetActorPositionResolver(ActorPositionResolver resolver);

    /// Where the ears are this frame, in world units. Only for the placement log -
    /// the engine does its own listener maths and this never enters the mix.
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
    /// The far half of the testbench's Use Vanilla Audio switch: with vanilla's
    /// body impacts put back and ours dropped here, the two mixes can be heard
    /// against each other inside one session.
    ///
    /// Muted here rather than by not ticking the engine, because the engine's tick
    /// is what drains the feed and the feed is what the testbench is recording.
    void SetMuted(bool muted);
    [[nodiscard]] bool Muted() const { return m_muted; }

    // -- ICueSink -------------------------------------------------------------
    void Emit(const Cue& cue) override;

    /// Cues taken in and engine voices actually started, since the last reset. The
    /// ratio between them says how much of the cue list is layers of one moment
    /// rather than separate moments, which nothing else reports.
    void Counters(std::uint64_t& cuesIn, std::uint64_t& voicesOut) const;

private:
    /// The mix rate. 48 kHz because that is what the whole pack is, so no file is
    /// resampled on the way in and the only interpolation is the cue's own pitch.
    static constexpr int kSampleRate = 48000;

    /// One acoustic moment's worth of cues, keyed by the engine's own provenance.
    struct Group {
        ActorId actorId{};
        std::uint32_t sourceSeq{};
        TimeMs earliestMs{};
        std::vector<Cue> cues;
    };

    /// A one-shot in flight. Held so its blob can be retired once the engine has
    /// certainly finished with it, and its placement re-sent until it sticks.
    struct Voice {
        RE::BSSoundHandle handle{};
        std::uint64_t token{};
        TimeMs startedMs{};
        TimeMs expiresMs{};
        Vec3 position{};
        ActorId actorId{};
        RE::NiAVObject* follow{};
        /// How many frames the placement has been sent on. Bounded, so a handle
        /// that never admits to playing costs a fixed number of messages.
        std::uint8_t placeAttempts{};
        /// Set once the placement has been sent on a frame the handle reported
        /// itself playing on, which is the first moment the engine is known to
        /// have somewhere to put it.
        bool placed{};
        /// One voice per log interval is followed from Play() to silence, to
        /// measure what the engine adds on its own.
        ///
        /// The design assumes voices start on a frame boundary and treats the
        /// 7-20 ms of quantisation as acceptable. If the engine's own start latency
        /// dwarfs a frame, how often our hook runs is not what decides when a sound
        /// arrives and making it faster buys nothing.
        ///
        /// Indirect but sound: we know the buffer length, so if the handle stops
        /// reporting itself playing about one buffer-length after Play() then
        /// playback began at once and `IsPlaying` was slow to admit it. Later than
        /// that is real latency the engine added before the first sample.
        bool watch{};
        float bufferMs{};
        TimeMs firstPlayingMs{-1.0};
        bool wasPlaying{};

        /// The mixed buffer's peak before the clip, so the placement line can say
        /// how hot what we handed the engine was. "Too quiet" has two halves - what
        /// we render and what the game does to it - and one log has to separate them.
        float peak{};
    };

    /// A loop in flight. The engine has no reachable whole-file loop flag, so a
    /// loop is a long buffer re-issued before it runs out, with the overlap
    /// crossfaded inside the buffer.
    struct Loop {
        std::uint32_t voiceId{};
        SlotId slot{};
        std::uint8_t variant{};
        float gainDb{};
        float pitch{1.0f};
        Vec3 position{};
        std::int32_t boneIndex{-1};
        /// The limb the loop is on. This side used to throw it away and pin every
        /// loop to the actor's root; the worry behind that - a scrape hopping
        /// between limbs would smear rather than track - was legitimate, but the
        /// cure belongs on the hop and is applied there (`fLimbHoldMs`).
        std::uint16_t limbIndex{};
        /// Whether the engine asked for one point. Loops never do - a hero moment
        /// is a moment and a loop is a texture that outlasts one.
        bool collapsed{};
        /// The node the loop is following, so a change of limb reaches a live handle
        /// rather than waiting up to four seconds for the next re-issue.
        RE::NiAVObject* follow{};
        ActorId actorId{};
        RE::BSSoundHandle handle{};
        std::uint64_t token{};
        TimeMs issuedMs{};
        TimeMs endsMs{};
        bool stopping{};

        /// How much of the level is in the *buffer* rather than on the handle. Zero
        /// for everything the mod asks for, since every loop gain in the config is
        /// an attenuation and `SetVolume` clamps to 1.0. A slot trim can push a loop
        /// over 0 dB, and only that excess is baked.
        float bakedDb{};

        /// The volume last sent, and how many more frames to keep re-sending it.
        /// The engine drops handle messages until the source voice exists - the
        /// trap `kPlaceAttempts` exists for on one-shots. A long slide sends an
        /// update every tick so a few dropped cost nothing, but a *short* one may
        /// send only one, and that one landing in the gap would leave the grind at
        /// whatever volume the voice opened with.
        float sentVolume{-1.0f};
        std::uint8_t volumeAttempts{};
    };

    bool StartComposite(const MixBuffer& mixed, ActorId actorId, TimeMs nowMs, bool dry,
                        SoundBus bus);

    /// Mix and start the half of a group that plays on one bus, or do nothing when
    /// no cue in it does - one acoustic moment becoming one voice per slider.
    ///
    /// The split is by category and nothing else: both buses are mixed against the
    /// SAME time base, the group's earliest cue, so the second buffer opens with
    /// however much leading silence its first layer sits behind by and the two
    /// voices start on the same frame with their offsets intact. Letting each half
    /// find its own frame zero would put frame quantisation back between the
    /// transient and the crunch.
    void StartBus(const Group& group, SoundBus bus, TimeMs nowMs);

    /// Whether this group is one the dry model applies to: true only when every cue
    /// in it is a tap or a colour layer on one. Reverb send is a property of the
    /// model and a model is chosen per voice, so a group holding anything else has
    /// to be opened wet or the impact inside it would go dry with the tap.
    [[nodiscard]] static bool GroupIsDry(std::span<const Cue> cues);

    /// Point a loop at the node it should be following now, sending it to the live
    /// handle if the answer changed. False when nothing resolved, which is the
    /// caller's cue to fall back on a world position.
    bool Follow(Loop& loop);

    /// Send a one-shot's position or attachment to its handle, and log whether the
    /// engine took it. Called on the frame the voice starts and the ones after -
    /// see Voice::placed.
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

    /// Open a handle on `wav` and place it. Everything both paths share. `bus`
    /// picks the sound category - the volume slider the player sees - set on the
    /// descriptor rather than the handle, because the engine applies a descriptor's
    /// category as it builds the sound while a handle message is dropped until the
    /// source voice exists.
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
    /// One bus's share of a group, gathered here rather than allocated per call: a
    /// knockdown is several groups a frame.
    std::vector<Cue> m_busCues;

    std::uint64_t m_cuesIn{};
    std::uint64_t m_voicesOut{};
    bool m_warnedNoModel{};
    bool m_muted{};
    Vec3 m_listener{};
    /// The placement line is worth one voice per burst, not one per voice: it asks
    /// "is our audio where the body is", which is about a knockdown.
    TimeMs m_lastPlaceLogMs{-1.0e9};
    /// Said once. A handle that refuses its position is not a per-voice event,
    /// it is the whole mod playing at the world origin.
    bool m_warnedPlacement{};
};

}  // namespace rds::game
