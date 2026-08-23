#include "rds/Link.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstring>
#include <format>

#include "rds/ConfigSchema.h"
#include "rds/SlotManifest.h"

#pragma comment(lib, "ws2_32.lib")

namespace rds::link {
namespace {

std::atomic<int> g_winsockUsers{0};

[[nodiscard]] std::string_view Trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    return text;
}

/// Walk `text` line by line, handing each trimmed non-empty line to `fn`.
template <class Fn>
void ForEachLine(std::string_view text, Fn&& fn) {
    while (!text.empty()) {
        const auto end = text.find('\n');
        const std::string_view line = Trim(text.substr(0, end));
        if (!line.empty()) {
            fn(line);
        }
        if (end == std::string_view::npos) {
            return;
        }
        text.remove_prefix(end + 1);
    }
}

/// `a=b` split on the *first* `=`, because a value can contain one and a key
/// cannot.
[[nodiscard]] bool SplitKeyValue(std::string_view line, std::string_view& key,
                                 std::string_view& value) {
    const auto eq = line.find('=');
    if (eq == std::string_view::npos) {
        return false;
    }
    key = Trim(line.substr(0, eq));
    value = Trim(line.substr(eq + 1));
    return !key.empty();
}

[[nodiscard]] double ToDouble(std::string_view text, double fallback = 0.0) {
    text = Trim(text);
    double value{};
    if (text.empty() ||
        std::from_chars(text.data(), text.data() + text.size(), value).ec != std::errc{}) {
        return fallback;
    }
    return value;
}

[[nodiscard]] std::int64_t ToInt(std::string_view text, std::int64_t fallback = 0) {
    text = Trim(text);
    std::int64_t value{};
    if (text.empty() ||
        std::from_chars(text.data(), text.data() + text.size(), value).ec != std::errc{}) {
        return fallback;
    }
    return value;
}

[[nodiscard]] std::uint64_t ToHex(std::string_view text, std::uint64_t fallback = 0) {
    text = Trim(text);
    std::uint64_t value{};
    if (text.empty() ||
        std::from_chars(text.data(), text.data() + text.size(), value, 16).ec != std::errc{}) {
        return fallback;
    }
    return value;
}

/// Split on `|`, keeping empty fields, because position is what identifies a
/// column in the limb rows.
void SplitPipe(std::string_view text, std::vector<std::string_view>& out) {
    out.clear();
    std::size_t start = 0;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == '|') {
            out.push_back(text.substr(start, i - start));
            start = i + 1;
        }
    }
}

[[nodiscard]] SOCKET Handle(std::uintptr_t raw) { return static_cast<SOCKET>(raw); }

}  // namespace

std::string_view ToString(Msg type) {
    switch (type) {
        case Msg::kHello: return "hello";
        case Msg::kWelcome: return "welcome";
        case Msg::kAlgorithm: return "algorithm";
        case Msg::kSfxTable: return "sfx";
        case Msg::kLibraryPath: return "library-path";
        case Msg::kClearOverrides: return "clear-overrides";
        case Msg::kProfile: return "profile";
        case Msg::kEvents: return "events";
        case Msg::kStatus: return "status";
        case Msg::kPing: return "ping";
        case Msg::kPong: return "pong";
    }
    return "?";
}

// ═════════════════════════════════════════════════════════════════════════════
// the socket
// ═════════════════════════════════════════════════════════════════════════════

void SocketStartup() {
    if (g_winsockUsers.fetch_add(1, std::memory_order_acq_rel) == 0) {
        WSADATA data{};
        WSAStartup(MAKEWORD(2, 2), &data);
    }
}

void SocketShutdown() {
    if (g_winsockUsers.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        WSACleanup();
    }
}

Socket::~Socket() { Close(); }

Socket::Socket(Socket&& other) noexcept : m_handle(other.m_handle) { other.m_handle = kInvalid; }

Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        Close();
        m_handle = other.m_handle;
        other.m_handle = kInvalid;
    }
    return *this;
}

void Socket::Close() {
    if (m_handle == kInvalid) {
        return;
    }
    ::closesocket(Handle(m_handle));
    m_handle = kInvalid;
    SocketShutdown();
}

bool Socket::Connect(std::uint16_t port, int timeoutMs) {
    Close();
    SocketStartup();

    const SOCKET handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (handle == INVALID_SOCKET) {
        SocketShutdown();
        return false;
    }

    // Non-blocking for the connect only, then straight back to blocking. A
    // blocking connect to a port nothing is listening on returns fast on
    // loopback, but "fast" is not "bounded", and this runs on the game's own
    // reconnect thread every few seconds for the whole session.
    u_long nonBlocking = 1;
    ::ioctlsocket(handle, FIONBIO, &nonBlocking);

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = ::htons(port);
    address.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);

    bool connected = false;
    if (::connect(handle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
        connected = true;
    } else if (::WSAGetLastError() == WSAEWOULDBLOCK) {
        fd_set writable{};
        FD_ZERO(&writable);
        FD_SET(handle, &writable);
        timeval timeout{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
        if (::select(0, nullptr, &writable, nullptr, &timeout) > 0) {
            int error = 0;
            int length = sizeof(error);
            connected = ::getsockopt(handle, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error),
                                     &length) == 0 &&
                        error == 0;
        }
    }

    if (!connected) {
        ::closesocket(handle);
        SocketShutdown();
        return false;
    }

    nonBlocking = 0;
    ::ioctlsocket(handle, FIONBIO, &nonBlocking);
    // Loopback with Nagle on turns a 40-byte status packet into a 40 ms wait.
    BOOL noDelay = TRUE;
    ::setsockopt(handle, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay),
                 sizeof(noDelay));
    m_handle = static_cast<std::uintptr_t>(handle);
    return true;
}

bool Socket::Listen(std::uint16_t port, std::string& error) {
    Close();
    SocketStartup();

    const SOCKET handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (handle == INVALID_SOCKET) {
        error = std::format("socket() failed ({})", ::WSAGetLastError());
        SocketShutdown();
        return false;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = ::htons(port);
    // Loopback rather than INADDR_ANY, deliberately: this socket accepts a
    // config that replaces the running algorithm and hands back every contact in
    // the world. It has no business being reachable from the network.
    address.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);

    if (::bind(handle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        const int code = ::WSAGetLastError();
        error = code == WSAEADDRINUSE
                    ? std::format("port {} is already in use - another testbench is running", port)
                    : std::format("bind to {} failed ({})", port, code);
        ::closesocket(handle);
        SocketShutdown();
        return false;
    }
    if (::listen(handle, 4) != 0) {
        error = std::format("listen failed ({})", ::WSAGetLastError());
        ::closesocket(handle);
        SocketShutdown();
        return false;
    }

    m_handle = static_cast<std::uintptr_t>(handle);
    return true;
}

Socket Socket::Accept(int timeoutMs) {
    Socket out;
    if (!Valid() || !Readable(timeoutMs)) {
        return out;
    }
    const SOCKET handle = ::accept(Handle(m_handle), nullptr, nullptr);
    if (handle == INVALID_SOCKET) {
        return out;
    }
    SocketStartup();  // the accepted socket carries its own winsock reference
    BOOL noDelay = TRUE;
    ::setsockopt(handle, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay),
                 sizeof(noDelay));
    out.m_handle = static_cast<std::uintptr_t>(handle);
    return out;
}

bool Socket::Readable(int timeoutMs) const {
    if (!Valid()) {
        return false;
    }
    fd_set readable{};
    FD_ZERO(&readable);
    FD_SET(Handle(m_handle), &readable);
    timeval timeout{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    return ::select(0, &readable, nullptr, nullptr, &timeout) > 0;
}

bool Socket::SendAll(const void* data, std::size_t bytes) {
    if (!Valid()) {
        return false;
    }
    const auto* cursor = static_cast<const char*>(data);
    while (bytes > 0) {
        const int chunk = ::send(Handle(m_handle), cursor, static_cast<int>(std::min<std::size_t>(bytes, 1 << 16)), 0);
        if (chunk <= 0) {
            return false;
        }
        cursor += chunk;
        bytes -= static_cast<std::size_t>(chunk);
    }
    return true;
}

int Socket::RecvAll(void* data, std::size_t bytes, int timeoutMs) {
    if (!Valid()) {
        return -1;
    }
    auto* cursor = static_cast<char*>(data);
    std::size_t remaining = bytes;
    bool started = false;
    while (remaining > 0) {
        if (!Readable(timeoutMs)) {
            // Nothing yet is only "nothing" before the first byte. Half a frame
            // followed by silence is a peer that died mid-write, and pretending
            // otherwise would leave the next read parsing a payload as a header.
            return started ? -1 : 0;
        }
        const int chunk =
            ::recv(Handle(m_handle), cursor, static_cast<int>(std::min<std::size_t>(remaining, 1 << 16)), 0);
        if (chunk <= 0) {
            return -1;
        }
        started = true;
        cursor += chunk;
        remaining -= static_cast<std::size_t>(chunk);
    }
    return 1;
}

// ── framing ──────────────────────────────────────────────────────────────────

bool WriteMessage(Socket& socket, Msg type, const void* payload, std::size_t bytes) {
    if (bytes > kMaxPayload) {
        return false;
    }
    Header header;
    header.type = static_cast<std::uint16_t>(type);
    header.size = static_cast<std::uint32_t>(bytes);

    // Header and payload in one buffer, so a frame is one send() and cannot be
    // interleaved with another thread's frame on the same socket. The alternative
    // is a lock around two sends, which is the same cost and one more thing to
    // get wrong.
    std::vector<std::byte> frame(sizeof(Header) + bytes);
    std::memcpy(frame.data(), &header, sizeof(header));
    if (bytes > 0) {
        std::memcpy(frame.data() + sizeof(Header), payload, bytes);
    }
    return socket.SendAll(frame.data(), frame.size());
}

bool WriteMessage(Socket& socket, Msg type, std::string_view text) {
    return WriteMessage(socket, type, text.data(), text.size());
}

int ReadMessage(Socket& socket, Msg& type, std::vector<std::byte>& payload, int timeoutMs) {
    Header header{};
    const int got = socket.RecvAll(&header, sizeof(header), timeoutMs);
    if (got != 1) {
        return got;
    }
    if (header.magic != kMagic || header.size > kMaxPayload) {
        // Not a resynchronisable condition: the stream is a byte offset out and
        // every subsequent read would be garbage. Dropping the connection is the
        // only honest answer.
        return -1;
    }
    payload.resize(header.size);
    if (header.size > 0 && socket.RecvAll(payload.data(), payload.size(), 5000) != 1) {
        return -1;
    }
    type = static_cast<Msg>(header.type);
    return 1;
}

// ═════════════════════════════════════════════════════════════════════════════
// the text payloads
// ═════════════════════════════════════════════════════════════════════════════

std::string EncodeAlgorithm(const AlgorithmConfig& config) {
    std::string out;
    out.reserve(6 * 1024);
    for (const ParamDesc& p : AlgorithmParams()) {
        if (p.type == ParamType::kString) {
            out += std::format("{}:{}={}\n", p.section, p.key, GetParamString(&config, p));
            continue;
        }
        out += std::format("{}:{}={}\n", p.section, p.key, FormatParam(p, GetParam(&config, p)));
    }
    return out;
}

void DecodeAlgorithm(std::string_view text, AlgorithmConfig& out) {
    const auto params = AlgorithmParams();
    ForEachLine(text, [&](std::string_view line) {
        std::string_view qualified;
        std::string_view value;
        if (!SplitKeyValue(line, qualified, value)) {
            return;
        }
        const auto colon = qualified.find(':');
        if (colon == std::string_view::npos) {
            return;
        }
        const std::string_view section = qualified.substr(0, colon);
        const std::string_view key = qualified.substr(colon + 1);
        for (const ParamDesc& p : params) {
            if (p.section != section || p.key != key) {
                continue;
            }
            if (p.type == ParamType::kString) {
                SetParamString(&out, p, value);
            } else {
                SetParam(&out, p, ParseParam(p, value));
            }
            return;
        }
        // Unknown key: a testbench newer than the DLL. Silently skipped, which
        // is the whole reason this message is text.
    });
}

std::string EncodeSfx(const SfxAssignments& assignments) {
    std::string out;
    for (const SlotDesc& slot : Slots()) {
        const SlotAssignment& assignment = assignments.For(slot.id);
        std::string files;
        for (const std::string& file : assignment.files) {
            if (!files.empty()) {
                files += '|';
            }
            files += file;
        }
        out += std::format("{}={}\n", slot.name, files);
        out += std::format("{}.loop={}\n", slot.name, assignment.looping ? 1 : 0);
    }
    return out;
}

void DecodeSfx(std::string_view text, SfxAssignments& out) {
    const auto slots = Slots();
    std::vector<std::string_view> parts;
    ForEachLine(text, [&](std::string_view line) {
        std::string_view key;
        std::string_view value;
        if (!SplitKeyValue(line, key, value)) {
            return;
        }
        bool isLoop = false;
        if (key.size() > 5 && key.substr(key.size() - 5) == ".loop") {
            isLoop = true;
            key.remove_suffix(5);
        }
        for (const SlotDesc& slot : slots) {
            if (slot.name != key) {
                continue;
            }
            SlotAssignment& assignment = out.For(slot.id);
            if (isLoop) {
                assignment.looping = ToInt(value) != 0;
                return;
            }
            assignment.files.clear();
            if (value.empty()) {
                return;
            }
            SplitPipe(value, parts);
            for (const std::string_view file : parts) {
                const std::string_view trimmed = Trim(file);
                if (!trimmed.empty()) {
                    assignment.files.emplace_back(trimmed);
                }
            }
            return;
        }
    });
}

std::string EncodeProfile(const ProfileMessage& message) {
    const ActorProfile& profile = message.profile;
    std::string out;
    out += std::format("actor_id={:08X}\n", static_cast<std::uint32_t>(profile.actorId));
    out += std::format("form_id={}\n", message.formId);
    out += std::format("name={}\n", profile.name);
    out += std::format("cell={}\n", message.cell);
    out += std::format("is_player={}\n", profile.isPlayer ? 1 : 0);
    out += std::format("scale={:.4f}\n", profile.scale);
    out += std::format("session_ms={:.3f}\n", message.sessionMs);
    // One line per limb, positional. Site, chain and coverage travel as their
    // integer values rather than as names: this message never touches a disk, so
    // there is no file to stay readable for, and the two ends are the same
    // enum by construction.
    for (std::size_t i = 0; i < profile.limbs.size(); ++i) {
        const LimbInfo& limb = profile.limbs[i];
        out += std::format("limb={}|{}|{}|{}|{}|{:.4f}|{:.3f}|{:016X}\n", i, limb.boneName,
                           static_cast<int>(limb.site), static_cast<int>(limb.chain),
                           static_cast<int>(limb.coverage), limb.havokMass, limb.radius,
                           limb.bodyId);
    }
    if (profile.reverb.valid) {
        out += std::format("acoustic_space={}\n", profile.reverb.acousticSpace);
        out += std::format("decay_ms={}\n", profile.reverb.decayTimeMs);
    }
    return out;
}

bool DecodeProfile(std::string_view text, ProfileMessage& out) {
    out = ProfileMessage{};
    bool sawActor = false;
    std::vector<std::string_view> fields;
    ForEachLine(text, [&](std::string_view line) {
        std::string_view key;
        std::string_view value;
        if (!SplitKeyValue(line, key, value)) {
            return;
        }
        if (key == "actor_id") {
            out.profile.actorId = static_cast<ActorId>(ToHex(value));
            sawActor = true;
        } else if (key == "form_id") {
            out.formId.assign(value);
        } else if (key == "name") {
            out.profile.name.assign(value);
        } else if (key == "cell") {
            out.cell.assign(value);
        } else if (key == "is_player") {
            out.profile.isPlayer = ToInt(value) != 0;
        } else if (key == "scale") {
            out.profile.scale = static_cast<float>(ToDouble(value, 1.0));
        } else if (key == "session_ms") {
            out.sessionMs = ToDouble(value);
        } else if (key == "acoustic_space") {
            out.profile.reverb.valid = true;
            out.profile.reverb.acousticSpace.assign(value);
        } else if (key == "decay_ms") {
            out.profile.reverb.decayTimeMs = static_cast<int>(ToInt(value));
        } else if (key == "limb") {
            SplitPipe(value, fields);
            if (fields.size() < 8) {
                return;
            }
            const auto index = static_cast<std::size_t>(std::max<std::int64_t>(0, ToInt(fields[0])));
            if (out.profile.limbs.size() <= index) {
                out.profile.limbs.resize(index + 1);
            }
            LimbInfo& limb = out.profile.limbs[index];
            limb.boneName.assign(fields[1]);
            limb.site = static_cast<LimbSite>(ToInt(fields[2]));
            limb.chain = static_cast<LimbChain>(ToInt(fields[3]));
            limb.coverage = static_cast<Coverage>(ToInt(fields[4]));
            limb.havokMass = static_cast<float>(ToDouble(fields[5]));
            limb.radius = static_cast<float>(ToDouble(fields[6]));
            limb.bodyId = ToHex(fields[7]);
        }
    });
    return sawActor;
}

}  // namespace rds::link
