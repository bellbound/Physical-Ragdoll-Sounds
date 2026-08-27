#pragma once

// The game's half of the devbench link.
//
// One background thread owns the socket. The game thread never blocks on it and
// never touches winsock: it appends finished frames to a staging buffer and reads
// back whatever the testbench pushed, both under one short mutex. Failure is
// silent - see Link.h.
//
// Out: every FeedEvent the engine drains, plus one profile per tracked actor and
// a status heartbeat. Teed off the feed rather than published separately (see
// LinkTap), so what the testbench replays is byte for byte what the engine
// consumed.
//
// In: an algorithm config, an sfx table, a path to the sfx library, and which mix
// the game should play. All four land in `Pending` and are applied by the game
// thread on its next tick - reloading a sound bank from a socket thread is a data
// race with the audio path.

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "rds/Config.h"
#include "rds/Feed.h"
#include "rds/Link.h"
#include "rds/Sfx.h"

namespace rds::game {

class DevLink {
public:
    ~DevLink();

    /// Spawn the socket thread. A no-op when `config.enabled` is false, the
    /// shipping case: no socket call at all, one branch a frame.
    void Start(const DevbenchConfig& config);
    void Stop();

    [[nodiscard]] bool Running() const { return m_running.load(std::memory_order_relaxed); }
    [[nodiscard]] bool Connected() const { return m_connected.load(std::memory_order_relaxed); }

    // ── outbound, all game thread ────────────────────────────────────────────

    void PushEvents(const FeedEvent* events, std::size_t count);
    void PushProfile(const link::ProfileMessage& message);
    void PushStatus(const link::StatusPacket& status);

    /// The cell the player is in, for the sidecar the testbench writes. Only the
    /// last value before a profile goes out is used.
    void SetCell(std::string cell);
    [[nodiscard]] std::string Cell() const;

    /// True once this actor's profile has gone out since it was last attached.
    /// The tap asks so it does not re-encode eighteen limbs every frame.
    [[nodiscard]] bool ProfileSent(ActorId actor) const;
    void ForgetProfile(ActorId actor);

    // ── inbound ──────────────────────────────────────────────────────────────

    /// What the testbench pushed. Every flag is one-shot: taking it clears it.
    struct Pending {
        bool algorithm{};
        bool sfx{};
        bool library{};
        bool clear{};
        /// The testbench's Use Vanilla Audio switch changed; `useVanillaAudio` says
        /// which way. Separate from `algorithm` because it is not a config: it puts
        /// vanilla's impact sounds back and takes ours away, which is done to the
        /// game rather than to the mix.
        bool audioMode{};
        bool useVanillaAudio{};
        ConfigSet config{};
        SfxAssignments sfxTable{};
        std::string libraryPath;

        [[nodiscard]] bool Any() const {
            return algorithm || sfx || library || clear || audioMode;
        }
    };

    /// Game thread. False when nothing changed, which is nearly every frame.
    [[nodiscard]] bool Take(Pending& out);

    /// Which overrides the testbench currently has in force, for the status
    /// heartbeat: bit 0 algorithm, bit 1 sfx, bit 2 library path.
    [[nodiscard]] std::uint32_t OverrideFlags() const {
        return m_overrideFlags.load(std::memory_order_relaxed);
    }

    /// Frames the staging buffer threw away because the testbench was not draining
    /// fast enough. Non-zero means the cap below is wrong - worth one log line at
    /// idle rather than a warning per frame.
    [[nodiscard]] std::uint64_t DroppedFrames() const {
        return m_droppedFrames.load(std::memory_order_relaxed);
    }

private:
    void Run();
    /// One connected session: handshake, then pump until the peer goes away.
    void Session(link::Socket& socket);
    /// Append one finished frame to the staging buffer, dropping it if the
    /// buffer is already over the cap.
    void Queue(link::Msg type, const void* payload, std::size_t bytes);

    /// How much unsent traffic may pile up before frames start being dropped. Four
    /// megabytes is about ten seconds of a busy knockdown at 128 bytes an event;
    /// past that the testbench is not reading, and holding the backlog only turns
    /// a stalled tool into a growing heap inside Skyrim.
    static constexpr std::size_t kMaxStagedBytes = 4u * 1024u * 1024u;

    DevbenchConfig m_config{};
    std::thread m_thread;
    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_connected{false};
    std::atomic<bool> m_acceptsEvents{true};
    std::atomic<std::uint32_t> m_overrideFlags{0};
    std::atomic<std::uint64_t> m_droppedFrames{0};

    mutable std::mutex m_outMutex;
    std::vector<std::byte> m_staged;

    mutable std::mutex m_inMutex;
    Pending m_pending;

    mutable std::mutex m_stateMutex;
    std::string m_cell;
    std::unordered_set<ActorId> m_profilesSent;
};

/// The tee. Wraps the live feed, hands everything through unchanged, and copies
/// each drained batch into the link on the way past. A wrapper rather than a hook
/// inside GameFeed because the engine decides what a frame's events *are* - it
/// calls Drain with the tick's clock, and anything published on a different
/// schedule would be a second opinion about the same stream.
class LinkTap final : public IFeed {
public:
    LinkTap(IFeed& inner, DevLink& link) : m_inner(inner), m_link(link) {}

    bool Drain(TimeMs untilMs, std::vector<FeedEvent>& out) override;
    [[nodiscard]] const ActorProfile* Profile(ActorId actor) const override {
        return m_inner.Profile(actor);
    }
    [[nodiscard]] const ListenerState& Listener() const override { return m_inner.Listener(); }
    [[nodiscard]] float FrameTimeSec() const override { return m_inner.FrameTimeSec(); }

private:
    IFeed& m_inner;
    DevLink& m_link;
};

}  // namespace rds::game
