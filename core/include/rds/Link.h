#pragma once

// The devbench link: one loopback TCP socket between the running game and the
// testbench.
//
// It lives in core/ because both halves speak it and a second encoder on either
// side is exactly how the two come to disagree about what a config was. Same
// reason Feed.h is here: one definition, two users.
//
// ── who is who ───────────────────────────────────────────────────────────────
//
// The **testbench listens** and the **game connects**. That way round because
// the game is the thing that comes and goes - you reload, you alt-tab, you
// restart Skyrim four times an afternoon - and a listener that outlives its
// client reconnects by itself. The reverse would need the testbench to poll for
// a port that is usually not there.
//
// Failure is always silent on the game side. The testbench not running is the
// ordinary case; a mod that logs a warning about it every three seconds is a mod
// whose log nobody reads.
//
// ── what travels ─────────────────────────────────────────────────────────────
//
//   bench -> game   the algorithm config, the sfx table, the sfx library path.
//                   All three as *text*, not as struct bytes: the config is
//                   already a flat table of named keys (ConfigSchema.h), text
//                   costs a few hundred bytes at the rate these are sent, and a
//                   testbench built an hour apart from the DLL then degrades to
//                   "that key was ignored" rather than to garbage floats.
//
//   game -> bench   actor profiles as text, and contacts as raw FeedEvent
//                   bytes. The events are the one place the volume argues for
//                   binary - a busy knockdown is a few thousand of them - and
//                   they are the one message where both ends are guaranteed to
//                   be the same build, because `sizeof(FeedEvent)` is checked in
//                   the handshake and the stream is refused if it differs.
//
// ── framing ──────────────────────────────────────────────────────────────────
//
// [u32 magic][u16 type][u16 flags][u32 payload bytes] then the payload. Native
// byte order and native struct layout throughout: both ends are MSVC x64 on the
// same machine, over loopback. Anything else here would be ceremony.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "rds/Config.h"
#include "rds/Feed.h"
#include "rds/Sfx.h"

namespace rds::link {

inline constexpr std::uint32_t kMagic = 0x21534452u;  ///< "RDS!"
inline constexpr std::uint16_t kVersion = 1;
inline constexpr std::uint16_t kDefaultPort = 27860;

/// The largest payload either side will accept. A frame bigger than this is a
/// desynchronised stream, not a big message, and reading it would be the whole
/// process hanging on a length somebody's memory corruption made up.
inline constexpr std::uint32_t kMaxPayload = 8u * 1024u * 1024u;

enum class Msg : std::uint16_t {
    kHello = 1,       ///< game -> bench. HelloPacket
    kWelcome,         ///< bench -> game. WelcomePacket
    kAlgorithm,       ///< bench -> game. `Section:Key=value` lines
    kSfxTable,        ///< bench -> game. `slot=file|file` and `slot.loop=1` lines
    kLibraryPath,     ///< bench -> game. one utf-8 path, no terminator
    kClearOverrides,  ///< bench -> game. empty; go back to what the inis say
    kProfile,         ///< game -> bench. one actor, as `key=value` lines
    kEvents,          ///< game -> bench. FeedEvent[], packed
    kStatus,          ///< game -> bench. StatusPacket, about once a second
    kPing,            ///< either way, empty
    kPong,            ///< either way, empty
    /// bench -> game. One byte: 1 puts vanilla's body impacts back and silences
    /// ours, 0 goes back to the mod's own mix. Appended rather than slotted in
    /// beside kClearOverrides so every value above stays what it was - a DLL and
    /// a testbench built a day apart still agree about the messages they share,
    /// and one that predates this simply never sends it.
    kAudioMode,
};

[[nodiscard]] std::string_view ToString(Msg type);

#pragma pack(push, 1)

struct Header {
    std::uint32_t magic{kMagic};
    std::uint16_t type{};
    std::uint16_t flags{};
    std::uint32_t size{};
};
static_assert(sizeof(Header) == 12);

/// What the game says on connect.
///
/// `feedEventSize` is the compatibility check that matters: the event stream is
/// raw struct bytes, so a testbench built against a different Feed.h has to
/// refuse it rather than reinterpret it. The config and sfx messages are text
/// and survive the mismatch, which is why the link stays useful either way.
struct HelloPacket {
    std::uint16_t version{kVersion};
    std::uint16_t feedEventSize{static_cast<std::uint16_t>(sizeof(FeedEvent))};
    std::uint32_t processId{};
    char build[32]{};  ///< __DATE__ of the DLL, so a stale game is visible at a glance
};

struct WelcomePacket {
    std::uint16_t version{kVersion};
    std::uint16_t feedEventSize{static_cast<std::uint16_t>(sizeof(FeedEvent))};
    /// False when the testbench will not read this game's events - a struct size
    /// mismatch. Config still flows; the stream does not.
    std::uint8_t acceptsEvents{1};
    std::uint8_t reserved[3]{};
    char build[32]{};
};

/// The heartbeat. Everything the connection row shows that is not a contact.
struct StatusPacket {
    double sessionMs{};            ///< the engine clock, so both ends can name the same instant
    std::uint32_t trackedActors{};
    std::uint32_t cuesEmitted{};   ///< since the session opened
    std::uint32_t voicesOut{};     ///< what those cues cost the game's audio engine
    std::uint32_t droppedContacts{};  ///< non-zero means the ring was too small
    std::uint32_t eventsSent{};
    std::uint32_t overrideFlags{};  ///< bit 0 algorithm, bit 1 sfx, bit 2 library path
};

#pragma pack(pop)

// ═════════════════════════════════════════════════════════════════════════════
// the socket
// ═════════════════════════════════════════════════════════════════════════════

/// A blocking loopback TCP socket, thin enough to read in one sitting.
///
/// The handle is a `uintptr_t` rather than a `SOCKET` so this header stays free
/// of winsock2.h - which pulls in windows.h, which would land in the plugin's
/// PCH and in the testbench's UI files alike for the sake of one integer.
class Socket {
public:
    Socket() = default;
    ~Socket();

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;

    [[nodiscard]] bool Valid() const { return m_handle != kInvalid; }
    void Close();

    /// Connect to 127.0.0.1 on `port`, giving up after `timeoutMs`. False on any
    /// failure, and deliberately without logging: "nothing is listening" is the
    /// normal answer.
    [[nodiscard]] bool Connect(std::uint16_t port, int timeoutMs);

    /// Listen on 127.0.0.1 only. `error` carries the reason on failure, because
    /// this one *is* worth showing - a port already in use is the testbench
    /// already running.
    [[nodiscard]] bool Listen(std::uint16_t port, std::string& error);

    /// Take the next connection, or an invalid socket if none arrives inside
    /// `timeoutMs`.
    [[nodiscard]] Socket Accept(int timeoutMs);

    /// Everything or nothing. False means the peer is gone.
    [[nodiscard]] bool SendAll(const void* data, std::size_t bytes);

    /// 1 read it all, 0 nothing arrived inside the timeout, -1 the peer closed
    /// or errored. A partial read blocks for up to `timeoutMs` *per chunk*: once
    /// a frame has started, finishing it is not optional.
    [[nodiscard]] int RecvAll(void* data, std::size_t bytes, int timeoutMs);

    [[nodiscard]] bool Readable(int timeoutMs) const;

private:
    static constexpr std::uintptr_t kInvalid = static_cast<std::uintptr_t>(~0ull);
    std::uintptr_t m_handle{kInvalid};
};

/// WSAStartup, refcounted, safe to call from anywhere. Every Socket that is
/// opened holds one of these for its lifetime, so nothing has to remember to
/// shut winsock down.
void SocketStartup();
void SocketShutdown();

// ── framing ──────────────────────────────────────────────────────────────────

[[nodiscard]] bool WriteMessage(Socket& socket, Msg type, const void* payload, std::size_t bytes);
[[nodiscard]] bool WriteMessage(Socket& socket, Msg type, std::string_view text);
[[nodiscard]] inline bool WriteMessage(Socket& socket, Msg type) {
    return WriteMessage(socket, type, nullptr, 0);
}

/// 1 got one, 0 nothing inside `timeoutMs`, -1 the peer went away or sent
/// something that is not a frame.
[[nodiscard]] int ReadMessage(Socket& socket, Msg& type, std::vector<std::byte>& payload,
                              int timeoutMs);

// ═════════════════════════════════════════════════════════════════════════════
// the payloads that are text
// ═════════════════════════════════════════════════════════════════════════════

/// Every algorithm parameter as `Section:Key=value`, in schema order.
[[nodiscard]] std::string EncodeAlgorithm(const ConfigSet& config);

/// Fill `out` from that text. Unknown keys are skipped, missing keys keep
/// whatever `out` already held - so a testbench and a DLL a version apart still
/// agree about every parameter they both know.
void DecodeAlgorithm(std::string_view text, ConfigSet& out);

/// `slot = file|file|file` plus `slot.loop = 0|1`, one slot per line.
[[nodiscard]] std::string EncodeSfx(const SfxAssignments& assignments);
void DecodeSfx(std::string_view text, SfxAssignments& out);

/// One actor, plus the two things about the session only the game knows - the
/// cell it happened in, and what the actor is called. The testbench needs both
/// to write a take's sidecar.
struct ProfileMessage {
    ActorProfile profile;
    std::string cell;
    std::string formId;    ///< eight hex digits, as the CSV writes it
    double sessionMs{};    ///< when the ragdoll attached, on the engine clock
};

[[nodiscard]] std::string EncodeProfile(const ProfileMessage& message);
[[nodiscard]] bool DecodeProfile(std::string_view text, ProfileMessage& out);

}  // namespace rds::link
