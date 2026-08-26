#pragma once

// The control socket: a second loopback listener, so a command line can edit
// the config of a testbench that is already running.
//
// ── why this exists ──────────────────────────────────────────────────────────
//
// Tuning by hand is sliders, and that is right - the panel exists because the
// parameter was declared. Tuning by *script* had no door at all: an AI or a
// batch file could write an ini into `configs/`, but the running program would
// never look at it, and the session in front of the user would still be playing
// what it was playing. The only way in was to close the testbench, which throws
// away the session that is the whole point of it (CLAUDE.md, first rule).
//
// So this is the same trick the devbench link plays on the game, pointed the
// other way: one loopback socket, one text payload of `key=value` lines, and
// what comes out the far end is the sliders moving. The change is heard on the
// next play - and pushed at the game on the same frame, because SyncToGame
// already sends whatever the focused side is holding.
//
// ── who is who ───────────────────────────────────────────────────────────────
//
// The **testbench listens**; the CLI connects, sends one request, waits for one
// reply and hangs up. One connection per command, which means a crashed or
// killed CLI leaves nothing behind, and a testbench that is not running is a
// refused connection rather than a hang.
//
// Port is the devbench port plus one - 27861 by default. Derived rather than
// configured, so this whole feature stays inside the throwaway half: nothing in
// `core/`, nothing in the shipped inis, no DLL to rebuild.
//
// ── the two threads ──────────────────────────────────────────────────────────
//
// The socket thread parses, queues, and blocks on the answer. The UI thread
// drains the queue once a frame (`App::PumpControl`) and answers. Nothing else
// may apply a request: a patch moves the config the mixer is reading and saves
// a file the picker is listing, and doing that from the socket thread is how
// two threads come to disagree about which config is loaded.
//
// A request that is never answered - the UI wedged, the window closing - times
// out on the socket thread and says so, rather than leaving the CLI to guess.
//
// ── the wire ─────────────────────────────────────────────────────────────────
//
// rds::link's framing, because it is already written and already correct:
// [u32 magic][u16 type][u16 flags][u32 size] then the payload. The two message
// ids live here rather than in rds::link::Msg - they never travel on the game's
// port, and the game's enum is a contract with a DLL that may be a week older
// than this build.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "rds/Link.h"

namespace tb {

/// CLI -> testbench, one text payload of `key=value` lines.
inline constexpr auto kControlRequest = static_cast<rds::link::Msg>(200);
/// testbench -> CLI, the same.
inline constexpr auto kControlReply = static_cast<rds::link::Msg>(201);

/// The devbench port plus one. See the header comment for why it is derived.
[[nodiscard]] inline std::uint16_t ControlPortFor(std::uint16_t devbenchPort) {
    return static_cast<std::uint16_t>(devbenchPort + 1);
}

/// One request, parsed. Unknown keys are dropped on the floor: a CLI a version
/// ahead of this build should degrade to "that flag did nothing", the same way
/// the config messages do.
struct ControlRequest {
    /// `status`, `list`, `get`, `set`, `load`. Anything else is an error the
    /// reply names.
    std::string op;

    /// `Section:Key=value`, in the order they were given. Applied all or
    /// nothing - a request with one bad key changes nothing, because half a
    /// patch is a config nobody asked for and nobody can name.
    std::vector<std::string> sets;

    /// Why. Goes into the new file's header comment and into the log, so a
    /// config found tomorrow says what it was trying.
    std::string note;

    /// Which named config to patch *from*. Empty means whatever the side is
    /// holding right now, unsaved edits and all.
    std::string base;

    /// Stem for the file to write. Empty means the next number in the family
    /// the side is on - `config_24_08_7` becomes `config_24_08_8`.
    std::string name;

    /// `get`: only parameters whose qualified key contains this, case
    /// insensitively. Empty means every one of them.
    std::string filter;

    /// 0 or 1. -1 means whichever side has focus, which is nearly always what a
    /// script means.
    int side{-1};

    /// Write the result as a new config and select it. On by default: a patch
    /// nobody can go back to is a patch that costs the session it landed in.
    bool save{true};
};

struct ControlJob {
    std::uint64_t id{};
    ControlRequest request;
};

class ControlServer {
public:
    ~ControlServer();

    ControlServer() = default;
    ControlServer(const ControlServer&) = delete;
    ControlServer& operator=(const ControlServer&) = delete;

    /// Open the listener and spawn its thread. False with the reason in
    /// `Error()` when the port is taken - which is the usual sign of a second
    /// testbench, exactly as it is for the game link.
    bool Start(std::uint16_t port);
    void Stop();

    [[nodiscard]] bool Listening() const { return m_listening.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint16_t Port() const { return m_port; }
    [[nodiscard]] std::string Error() const;
    /// How many requests this session has served, for the settings window.
    [[nodiscard]] std::uint64_t Served() const { return m_served.load(std::memory_order_relaxed); }

    /// Take the next request, or false when there is none. UI thread only.
    [[nodiscard]] bool Poll(ControlJob& job);

    /// Hand back the answer to a job `Poll` gave out. Must be called for every
    /// one of them: the CLI is holding a socket open until it arrives.
    void Complete(std::uint64_t id, std::string reply);

private:
    void Run(rds::link::Socket listener);
    void Serve(rds::link::Socket& client);

    std::uint16_t m_port{};
    std::thread m_thread;
    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_listening{false};
    std::atomic<std::uint64_t> m_served{0};

    mutable std::mutex m_errorMutex;
    std::string m_error;

    std::mutex m_jobMutex;
    std::condition_variable m_jobCv;
    std::deque<ControlJob> m_pending;
    /// Answered jobs, keyed by id. The waiting socket thread takes its own out;
    /// a reply for an id that has already given up is dropped.
    std::map<std::uint64_t, std::string> m_replies;
    std::uint64_t m_nextId{1};
};

/// When a file was created, as the 100 ns count from 1601 that Windows keeps.
///
/// `std::filesystem` has no creation time, and the configs need one: a save
/// writes a new file rather than over the old one, so creation order *is* the
/// order they were tried in, and that is the order the picker wants. Falls back
/// to the write time when the query fails, which is the same scale - MSVC's
/// file_time_type is a FILETIME count.
[[nodiscard]] std::uint64_t FileCreatedTicks(const std::filesystem::path& file);

}  // namespace tb
