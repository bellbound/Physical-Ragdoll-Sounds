#pragma once

// The testbench's half of the devbench link.
//
// The testbench listens and the game connects - see rds/Link.h for why round
// that way. One socket thread owns the connection; the UI thread reads a
// snapshot and appends to a send queue, and never blocks on either.
//
// The whole point is that a config is one object with one definition
// (ConfigSchema.h) and a contact is one struct with one definition (Feed.h), so
// what the game runs is what the sliders say and what the testbench replays is
// what the game consumed - byte for byte, off the same drain.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "rds/Config.h"
#include "rds/Feed.h"
#include "rds/Link.h"
#include "rds/Sfx.h"

namespace tb {

/// Everything the connection row shows.
struct LinkSnapshot {
    bool listening{};
    bool connected{};
    /// Why the listener could not open, if it could not. Worth showing: the
    /// usual cause is a second testbench already running.
    std::string error;

    std::string peerBuild;   ///< __DATE__ of the DLL that connected
    std::uint32_t peerPid{};
    /// False when the game's FeedEvent layout differs from ours. Config still
    /// flows; the event stream does not, and saying so is better than showing an
    /// empty timeline.
    bool acceptsEvents{true};

    rds::link::StatusPacket game{};  ///< the game's own heartbeat
    double gameStatusAgeSec{};       ///< how long since one arrived

    std::uint64_t eventsReceived{};
    std::uint64_t contactsReceived{};
    double lastEventMs{};  ///< the newest event's stamp on the game's clock
};

/// One live take, as it accumulates. The profiles are kept per actor because a
/// knockdown near another tracked actor produces two.
struct LiveCapture {
    std::vector<rds::FeedEvent> events;
    std::map<rds::ActorId, rds::link::ProfileMessage> profiles;
    double startMs{};
    double endMs{};

    [[nodiscard]] bool Empty() const { return events.empty(); }
    [[nodiscard]] double DurationMs() const { return endMs - startMs; }
    /// The actor with the most contacts, which is the one the take is about.
    [[nodiscard]] const rds::link::ProfileMessage* Subject() const;
};

class GameLink {
public:
    ~GameLink();

    GameLink() = default;
    GameLink(const GameLink&) = delete;
    GameLink& operator=(const GameLink&) = delete;

    /// Open the listener and spawn its thread. False with a reason in the
    /// snapshot when the port could not be taken.
    bool Start(std::uint16_t port);
    void Stop();

    [[nodiscard]] LinkSnapshot Snapshot() const;
    [[nodiscard]] bool Connected() const { return m_connected.load(std::memory_order_relaxed); }

    // ── pushing config at the game ───────────────────────────────────────────
    //
    // All of them are fire and forget. Nothing waits for an answer, because the
    // answer is that the next contact sounds different.

    void PushAlgorithm(const rds::AlgorithmConfig& config);
    void PushSfx(const rds::SfxAssignments& assignments);
    void PushLibraryPath(const std::string& path);
    void PushClear();

    /// Which mix the game should be playing. True puts vanilla's own body
    /// impacts back and silences ours; false is the mod as it ships. Not a
    /// config override - it changes what the game is, not what the algorithm
    /// says - so it goes out whether or not the sliders are being pushed.
    void PushAudioMode(bool useVanillaAudio);

    // ── capturing what arrives ───────────────────────────────────────────────

    void BeginCapture();
    /// Stop, and hand over everything captured. Empty when nothing arrived,
    /// which is what "you pressed record and nobody fell over" looks like.
    [[nodiscard]] LiveCapture EndCapture();
    void CancelCapture();
    [[nodiscard]] bool Capturing() const { return m_capturing.load(std::memory_order_relaxed); }
    [[nodiscard]] std::size_t CapturedEvents() const;
    /// Take time of the newest captured event, so the UI can show a clock that
    /// tracks the game rather than the wall.
    [[nodiscard]] double CapturedDurationMs() const;
    /// Contacts, not every row - the number that says whether the take is worth
    /// keeping.
    [[nodiscard]] std::size_t CapturedContacts() const;

    // ── the game's clock, read from here ─────────────────────────────────────

    /// What the game's session clock reads right now, on this side of the
    /// socket. False when no heartbeat has arrived on this connection.
    ///
    /// This is the clock every event is stamped on, so it is the clock anything
    /// lined up against a take has to be expressed in - the video, in practice.
    /// Both ends stamp off `steady_clock` on the same machine, so the two run at
    /// the same rate and only the epoch differs; the heartbeat carries the game's
    /// reading once a second and the difference is that epoch.
    ///
    /// The difference is taken as the *smallest* any heartbeat has shown rather
    /// than the newest. A packet can only ever arrive late - queue, poll, socket -
    /// so every sample is the true epoch plus a positive delay, and the minimum
    /// over a take's worth of them is the sample that was least delayed. With a
    /// 1 Hz heartbeat, a 20 ms read poll at each end and loopback in between,
    /// that lands within a few milliseconds, which is inside a video frame.
    [[nodiscard]] bool GameClock(double& sessionMs) const;

private:
    void Run();
    void Serve(rds::link::Socket& client);
    void Queue(rds::link::Msg type, const void* payload, std::size_t bytes);
    void OnEvents(const std::byte* data, std::size_t bytes);
    void OnProfile(std::string_view text);

    std::uint16_t m_port{};
    std::thread m_thread;
    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_listening{false};
    std::atomic<bool> m_connected{false};
    std::atomic<bool> m_capturing{false};

    mutable std::mutex m_mutex;
    LinkSnapshot m_snapshot;
    LiveCapture m_capture;
    std::size_t m_capturedContacts{};
    /// Steady-clock stamp of the last status packet, for the age in the
    /// snapshot. Kept as a count of seconds since Start so the snapshot stays a
    /// plain struct.
    double m_lastStatusSec{};

    /// This process's own zero, so a local instant and the game's sessionMs can
    /// be subtracted from one another.
    std::chrono::steady_clock::time_point m_epoch{std::chrono::steady_clock::now()};
    /// Local ms minus the game's sessionMs, at the least-delayed heartbeat this
    /// connection has seen. Reset on connect, because the game restarts its
    /// session clock and the old epoch would be a lifetime out.
    double m_clockOffsetMs{};
    bool m_haveClockOffset{};

    mutable std::mutex m_outMutex;
    std::vector<std::byte> m_staged;
};

}  // namespace tb
