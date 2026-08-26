#include "Obs.h"

// Windows first: winhttp.h and bcrypt.h both need it, and the PCH has already
// pulled it in behind CommonLib.
#include <windows.h>

#include <bcrypt.h>
#include <wincrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <format>
#include <functional>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

#include <spdlog/spdlog.h>

#include <nlohmann/json.hpp>


// ── where this came from ─────────────────────────────────────────────────────
//
// A port of skse/QuickModMenuNG/src/integration/Obs.cpp, which already solved
// every hard part of driving OBS once, against the same OBS. Three things were
// replaced and nothing else:
//
//   Settings()  ->  Settings(), filled from RagdollSounds.ini
//   Util::Notify / NotifyF         ->  Note / NoteF, which land in the log and in
//                                      the one line the connection row shows
//   Defer(After)      ->  Defer / DeferAfter, drained by Pump() once
//                                      a frame from the UI thread
//
// It is a copy rather than a shared file because the two live in different
// builds with different dependencies, and the alternative - a third library
// existing only so two programs can talk to OBS - is worse than the duplication.
//
// ── why any of this ──────────────────────────────────────────────────────────
//
// obs-websocket is the only runtime control surface OBS has. --startrecording is
// a launch flag rather than a command, and driving OBS's global hotkeys with
// SendInput is silent when it fails and cannot read back whether a recording is
// actually running - which is the one thing the menu needs to know before it can
// draw a row.
//
// The protocol is JSON over a plain ws:// socket on loopback, so the transport is
// WinHTTP's own WebSocket API and this file costs the build nothing: winhttp for
// the socket, bcrypt for the SHA256 the handshake needs, crypt32 for base64, and
// nlohmann_json, which was already here. Pulling in a WebSocket library would
// have brought OpenSSL along for a connection to 127.0.0.1 that has no use for
// it.
//
// ── the threads ──────────────────────────────────────────────────────────────
//
// Nothing here may run on the game thread and nothing here may make the game
// thread wait, so the socket gets two threads of its own:
//
//   The writer owns the connection. It performs the handshake, then loops on a
//   command queue, sending each entry as one op-6 frame. It reconnects with
//   backoff and is the only thread that ever sends.
//
//   The reader blocks in WinHttpWebSocketReceive and dispatches whatever comes
//   back - a response to a pending requestId, or an event that moves the status
//   snapshot. WinHTTP allows one outstanding send and one outstanding receive on
//   the same handle concurrently, which is exactly this shape.
//
// The reader is spawned per connection and both threads hold a shared_ptr to the
// connection, whose destructor closes the handles. That is deliberate: it means
// no thread is ever inside a WinHTTP call on a handle another thread has just
// closed, which is the usual way this sort of code crashes. Ending a connection
// is therefore a close *frame* (WinHttpWebSocketShutdown), not a closed handle -
// OBS answers it, the reader's receive returns, and the last shared_ptr to go
// does the cleanup.
//
// The menu never touches any of it. It reads a snapshot of atomics, the same
// contract as ImpactRecorder::IsRecording(), because a page is rebuilt on every
// probe round and must never block.

namespace tb::obs {

namespace {
using Clock_ = std::chrono::steady_clock;
}

// ── the three things the port replaced ───────────────────────────────────────

namespace {

std::mutex g_shimMutex;
Config g_settings;

/// Callbacks waiting for the UI thread. QuickModMenuNG had a game thread to hand
/// them to; here Pump() is the equivalent, called once a frame from Draw().
struct Deferred {
    std::function<void()> fn;
    Clock_::time_point due;
};
std::vector<Deferred> g_deferred;

/// The last thing worth showing a human, and a monotonic stamp so the UI can
/// fade it. One line rather than a log window: everything here is also in the
/// testbench's own log, and the connection row has space for a sentence.
std::string g_note;
std::uint64_t g_noteSeq = 0;

void Note(const std::string& text) {
    spdlog::info("Obs: {}", text);
    const std::lock_guard lock(g_shimMutex);
    g_note = text;
    ++g_noteSeq;
}

template <class... Args>
void NoteF(std::format_string<Args...> format, Args&&... args) {
    Note(std::format(format, std::forward<Args>(args)...));
}

void Defer(std::function<void()> fn) {
    const std::lock_guard lock(g_shimMutex);
    g_deferred.push_back({std::move(fn), Clock_::now()});
}

void DeferAfter(std::uint32_t delayMs, std::function<void()> fn) {
    const std::lock_guard lock(g_shimMutex);
    g_deferred.push_back({std::move(fn), Clock_::now() + std::chrono::milliseconds(delayMs)});
}

[[nodiscard]] const Config& Settings() { return g_settings; }

}  // namespace

void Configure(const Config& config) {
    const std::lock_guard lock(g_shimMutex);
    g_settings = config;
}

std::string LastNote(std::uint64_t& seq) {
    const std::lock_guard lock(g_shimMutex);
    seq = g_noteSeq;
    return g_note;
}

void Pump() {
    std::vector<std::function<void()>> due;
    {
        const std::lock_guard lock(g_shimMutex);
        const auto now = Clock_::now();
        for (auto it = g_deferred.begin(); it != g_deferred.end();) {
            if (it->due <= now) {
                due.push_back(std::move(it->fn));
                it = g_deferred.erase(it);
            } else {
                ++it;
            }
        }
    }
    // Outside the lock: a callback that queues another one would deadlock, and
    // StartTake's armed callback does exactly that.
    for (auto& fn : due) {
        fn();
    }
}

namespace {

using json = nlohmann::json;

constexpr const char* kDefaultHost = "127.0.0.1";
constexpr std::uint16_t kDefaultPort = 4455;
constexpr int kRpcVersion = 1;

/// EventSubscription::Outputs. RecordStateChanged and the replay-buffer events,
/// and nothing else - everything OBS could otherwise flood us with is a message
/// the reader would parse only to throw away.
constexpr int kSubscribeOutputs = 1 << 6;

/// How often the timecode is refreshed while recording. Only while recording,
/// and only because the row shows a running clock; the recording *state* itself
/// arrives as an event and is never polled for.
constexpr auto kPollInterval = std::chrono::milliseconds(1000);

constexpr auto kBackoffFirst = std::chrono::milliseconds(1000);
constexpr auto kBackoffMax = std::chrono::milliseconds(30000);

/// Handshake reads. The websocket's own receive is left infinite afterwards - a
/// connection with nothing to say is the ordinary case and must not time out.
constexpr DWORD kHandshakeTimeoutMs = 10000;
constexpr DWORD kConnectTimeoutMs = 3000;

/// How long to let OBS finish writing a stopped recording before giving up on
/// the stopped event and taking the file as it stands. A long hybrid-MP4 take
/// rewrites its header on close, and that is seconds, not milliseconds.
constexpr std::uint32_t kStopFinaliseTimeoutMs = 60000;

// ── strings and crypto ───────────────────────────────────────────────────────

std::wstring Widen(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                         nullptr, 0);
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

std::string Base64(const std::uint8_t* data, DWORD size) {
    DWORD chars = 0;
    if (!CryptBinaryToStringA(data, size, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr,
                              &chars)) {
        return {};
    }
    std::string out(chars, '\0');
    if (!CryptBinaryToStringA(data, size, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, out.data(),
                              &chars)) {
        return {};
    }
    // The sizing call counts the terminator; the writing call does not.
    out.resize(chars);
    return out;
}

/// The three-call form rather than the one-shot BCryptHash: this project builds
/// against _WIN32_WINNT=0x0601 and the one-shot is declared behind a Windows 8
/// guard, so it is simply not there. These three are Vista-era and always are.
bool Sha256(const std::string& input, std::uint8_t (&digest)[32]) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
        return false;
    }

    bool ok = false;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0) {
        ok = BCryptHashData(hash,
                            reinterpret_cast<PUCHAR>(const_cast<char*>(input.data())),
                            static_cast<ULONG>(input.size()), 0) >= 0 &&
             BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0;
        BCryptDestroyHash(hash);
    }

    BCryptCloseAlgorithmProvider(algorithm, 0);
    return ok;
}

/// base64(sha256(base64(sha256(password + salt)) + challenge)).
///
/// The inner hash is base64'd *before* the challenge is appended. Every
/// hand-rolled obs-websocket client gets that step wrong once, and the only
/// symptom is OBS closing the socket with 4009 and nothing at all on this side.
std::string AuthString(const std::string& password, const std::string& salt,
                       const std::string& challenge) {
    std::uint8_t digest[32];
    if (!Sha256(password + salt, digest)) {
        return {};
    }
    const auto secret = Base64(digest, sizeof(digest));
    if (secret.empty() || !Sha256(secret + challenge, digest)) {
        return {};
    }
    return Base64(digest, sizeof(digest));
}

/// nlohmann's value() throws on a key that is present but null, which OBS uses
/// freely - an unset profile parameter, a scene name on a desynced canvas.
std::string StringOr(const json& object, const char* key, std::string fallback = {}) {
    if (const auto found = object.find(key); found != object.end() && found->is_string()) {
        return found->get<std::string>();
    }
    return fallback;
}

std::uint64_t NumberOr(const json& object, const char* key, std::uint64_t fallback = 0) {
    if (const auto found = object.find(key); found != object.end() && found->is_number()) {
        return found->get<std::uint64_t>();
    }
    return fallback;
}

bool BoolOr(const json& object, const char* key, bool fallback = false) {
    if (const auto found = object.find(key); found != object.end() && found->is_boolean()) {
        return found->get<bool>();
    }
    return fallback;
}

// ── where OBS is, and what the password is ───────────────────────────────────

struct Endpoint {
    std::string host = kDefaultHost;
    std::uint16_t port = kDefaultPort;
    std::string password;
    /// False when OBS's own config says the server is switched off, which is a
    /// different failure from "nothing is listening" and deserves a different
    /// sentence.
    bool serverEnabled = true;
    bool fromObsConfig = false;
};

std::filesystem::path ObsConfigPath() {
    const char* appData = std::getenv("APPDATA");
    if (!appData) {
        return {};
    }
    return std::filesystem::path(appData) / "obs-studio" / "plugin_config" / "obs-websocket" /
           "config.json";
}

/// The ini wins wherever it has an answer; everything it leaves blank comes from
/// OBS's own config. That is the whole point - obs-websocket generates a password
/// for itself on first load and writes it down, so asking the player to copy it
/// into a second file on the same machine is make-work.
Endpoint Resolve() {
    const auto& config = Settings();
    Endpoint endpoint;
    endpoint.host = config.host.empty() ? kDefaultHost : config.host;
    endpoint.port = config.port == 0 ? kDefaultPort : config.port;
    endpoint.password = config.password;

    const bool needPort = config.port == 0;
    const bool needPassword = config.password.empty();
    if (!needPort && !needPassword) {
        return endpoint;
    }

    const auto path = ObsConfigPath();
    std::error_code ec;
    if (path.empty() || !std::filesystem::exists(path, ec)) {
        spdlog::info("Obs: no OBS config at {} - using the ini and the defaults",
                     path.empty() ? std::string("%APPDATA%") : path.string());
        return endpoint;
    }

    try {
        std::ifstream in(path);
        const auto parsed = json::parse(in);
        if (needPort) {
            if (const auto port = NumberOr(parsed, "server_port", 0); port >= 1 && port <= 65535) {
                endpoint.port = static_cast<std::uint16_t>(port);
            }
        }
        if (needPassword && BoolOr(parsed, "auth_required", true)) {
            endpoint.password = StringOr(parsed, "server_password");
        }
        endpoint.serverEnabled = BoolOr(parsed, "server_enabled", true);
        endpoint.fromObsConfig = true;
        spdlog::info("Obs: read port {} and {} from OBS's own config (server_enabled={})",
                     endpoint.port, endpoint.password.empty() ? "no password" : "a password",
                     endpoint.serverEnabled);
    } catch (const std::exception& e) {
        spdlog::warn("Obs: could not read {}: {}", path.string(), e.what());
    }
    // Host is never in that file; loopback is the only address it listens on that
    // this plugin has any business using, so the ini is the only way to move it.
    return endpoint;
}

// ── the connection ───────────────────────────────────────────────────────────

struct Conn {
    HINTERNET session = nullptr;
    HINTERNET connect = nullptr;
    HINTERNET request = nullptr;
    HINTERNET socket = nullptr;
    /// Cleared when a close frame has been sent, so the reader knows the receive
    /// that follows is the wind-down rather than a fault.
    std::atomic<bool> closing{false};

    Conn() = default;
    Conn(const Conn&) = delete;
    Conn& operator=(const Conn&) = delete;

    ~Conn() {
        // Whichever of the writer and the reader lets go last runs this, which is
        // why neither of them ever closes a handle by hand.
        if (socket) {
            WinHttpCloseHandle(socket);
        }
        if (request) {
            WinHttpCloseHandle(request);
        }
        if (connect) {
            WinHttpCloseHandle(connect);
        }
        if (session) {
            WinHttpCloseHandle(session);
        }
    }
};

using ConnPtr = std::shared_ptr<Conn>;

bool SendText(Conn& conn, const std::string& text) {
    const auto rc = WinHttpWebSocketSend(conn.socket, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                         const_cast<char*>(text.data()),
                                         static_cast<DWORD>(text.size()));
    if (rc != NO_ERROR) {
        spdlog::warn("Obs: send failed ({})", rc);
        return false;
    }
    // The Identify frame carries the challenge response. It is worthless once
    // the challenge is spent, but it does not belong in a log either.
    spdlog::trace("Obs: -> {}",
                  text.find("\"authentication\"") == std::string::npos ? text
                                                                       : "{op:1, Identify}");
    return true;
}

/// One whole message, however many frames it arrives in. GetSceneList on a real
/// profile is several KB and does arrive fragmented, so the buffer type has to be
/// checked rather than assumed.
///
/// `tolerateIdle` is for the reader, where a socket with nothing to say is the
/// ordinary case: the receive timeout is lifted after the handshake, but if that
/// option did not take, a timeout must not be mistaken for a dropped connection.
/// The handshake passes false, because there it means OBS has stopped talking
/// mid-conversation.
bool ReceiveText(Conn& conn, std::string& out, bool tolerateIdle = false) {
    out.clear();
    char buffer[4096];
    for (;;) {
        DWORD read = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};
        const auto rc =
            WinHttpWebSocketReceive(conn.socket, buffer, sizeof(buffer), &read, &type);
        if (rc == ERROR_WINHTTP_TIMEOUT && tolerateIdle && !conn.closing.load()) {
            continue;
        }
        if (rc != NO_ERROR) {
            if (!conn.closing.load()) {
                spdlog::info("Obs: receive ended ({})", rc);
            }
            return false;
        }
        if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
            return false;
        }
        out.append(buffer, read);
        if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE ||
            type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) {
            spdlog::trace("Obs: <- {}", out);
            return true;
        }
    }
}

ConnPtr Dial(const Endpoint& endpoint, std::string& error) {
    auto conn = std::make_shared<Conn>();

    conn->session = WinHttpOpen(L"QuickModMenuNG/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!conn->session) {
        error = std::format("WinHttpOpen failed ({})", GetLastError());
        return nullptr;
    }
    // Resolve, connect and send are all bounded; the handshake read is bounded
    // too, and only the websocket's own receive is left to block forever.
    WinHttpSetTimeouts(conn->session, kConnectTimeoutMs, kConnectTimeoutMs, kConnectTimeoutMs,
                       kHandshakeTimeoutMs);

    conn->connect = WinHttpConnect(conn->session, Widen(endpoint.host).c_str(), endpoint.port, 0);
    if (!conn->connect) {
        error = std::format("WinHttpConnect failed ({})", GetLastError());
        return nullptr;
    }

    conn->request = WinHttpOpenRequest(conn->connect, L"GET", L"/", nullptr, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!conn->request) {
        error = std::format("WinHttpOpenRequest failed ({})", GetLastError());
        return nullptr;
    }
    if (!WinHttpSetOption(conn->request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)) {
        error = std::format("this Windows has no WebSocket support ({})", GetLastError());
        return nullptr;
    }
    if (!WinHttpSendRequest(conn->request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        error = std::format("nothing is listening on {}:{} ({})", endpoint.host, endpoint.port,
                            GetLastError());
        return nullptr;
    }
    if (!WinHttpReceiveResponse(conn->request, nullptr)) {
        error = std::format("no upgrade response ({})", GetLastError());
        return nullptr;
    }

    conn->socket = WinHttpWebSocketCompleteUpgrade(conn->request, 0);
    if (!conn->socket) {
        error = std::format("the server refused the WebSocket upgrade ({}) - is that OBS?",
                            GetLastError());
        return nullptr;
    }
    // The request handle has done its job. Released now rather than at
    // destruction so a long-lived connection is not holding it open.
    WinHttpCloseHandle(conn->request);
    conn->request = nullptr;
    return conn;
}

// ── shared state ─────────────────────────────────────────────────────────────

/// `code` is obs-websocket's RequestStatus code, which is the only way to tell
/// "OBS cannot do this" from "OBS has already done this" - see SendPause, where
/// the difference decides whether pausing is switched off for the session.
using Reply = std::function<void(bool ok, std::uint64_t code, const json& data,
                                 const std::string& comment, std::uint64_t rttMs)>;

struct Command {
    std::string type;
    json data;
    Reply reply;
};

struct PendingRequest {
    std::string type;
    Reply reply;
    Clock_::time_point sentAt;
};

std::mutex g_mutex;  // the queue, the pending map, the connection, the strings
std::condition_variable g_writerCv;
std::deque<Command> g_queue;
std::unordered_map<std::string, PendingRequest> g_pending;
ConnPtr g_conn;
std::uint64_t g_nextRequestId = 1;

std::string g_scene;
std::vector<std::string> g_scenes;
std::string g_savedFilenameFormat;
bool g_haveSavedFilenameFormat = false;
std::string g_lastOutputPath;
std::string g_obsVersion;
std::string g_recordDirectory;
/// Fired once when OBS reports the output actually running, so an impact take
/// can set its t0 to a moment that is inside the video.
std::function<void(bool)> g_takeArmed;
/// Fired once when OBS reports the output actually stopped. StopRecord answers
/// as soon as the stop is *asked for*, and its outputPath names a file OBS is
/// still finalising - moving it then leaves the mp4 with no moov atom and no
/// frames to decode. The stopped event is the file being closed.
std::function<void(std::string)> g_takeStopped;

std::atomic<bool> g_threadRunning{false};
std::atomic<bool> g_wantConnected{false};
std::atomic<bool> g_connected{false};
std::atomic<bool> g_recording{false};
std::atomic<bool> g_paused{false};
std::atomic<bool> g_autoPaused{false};
/// A pause or resume that has been sent and not yet answered.
std::atomic<bool> g_pauseInFlight{false};
std::atomic<bool> g_manualPaused{false};
std::atomic<bool> g_takeActive{false};
std::atomic<bool> g_chapterSupported{false};
std::atomic<bool> g_startOnMenuClose{false};
/// One failure sentence per outage, not one per queued command.
std::atomic<bool> g_announcedFailure{false};
std::atomic<Tri> g_pauseSupported{Tri::Unknown};
std::atomic<std::uint64_t> g_durationMs{0};

void EnsureThread();

void Enqueue(std::string type, json data, Reply reply = {}) {
    if (!Configured()) {
        return;
    }
    g_wantConnected.store(true);
    {
        const std::lock_guard lock(g_mutex);
        g_queue.push_back({std::move(type), std::move(data), std::move(reply)});
    }
    EnsureThread();
    g_writerCv.notify_all();
}

/// Everything still waiting on a socket that has gone away. Cleared rather than
/// left to rot: a reply closure captures whatever the caller handed it, and one
/// that never runs is a take that never learns it has no video.
void FailPending(const std::string& reason) {
    std::unordered_map<std::string, PendingRequest> pending;
    std::function<void(bool)> armed;
    std::function<void(std::string)> stopped;
    std::string lastPath;
    {
        const std::lock_guard lock(g_mutex);
        pending.swap(g_pending);
        armed.swap(g_takeArmed);
        stopped.swap(g_takeStopped);
        lastPath = g_lastOutputPath;
    }
    for (auto& [id, request] : pending) {
        if (request.reply) {
            request.reply(false, 0, json::object(), reason, 0);
        }
    }
    if (armed) {
        armed(false);
    }
    // The socket went away mid-stop. OBS is still finalising on its own side, so
    // the path is worth handing over - it is the take's video either way, and a
    // waiter left armed here is a take that never finishes at all.
    if (stopped) {
        stopped(lastPath);
    }
}

// ── incoming ─────────────────────────────────────────────────────────────────

void OnRecordState(const json& data) {
    const auto state = StringOr(data, "outputState");
    const bool active = BoolOr(data, "outputActive", g_recording.load());

    if (state == "OBS_WEBSOCKET_OUTPUT_STARTED") {
        g_recording.store(true);
        g_paused.store(false);
        std::function<void(bool)> armed;
        {
            const std::lock_guard lock(g_mutex);
            armed.swap(g_takeArmed);
        }
        if (armed) {
            armed(true);
        }
    } else if (state == "OBS_WEBSOCKET_OUTPUT_STOPPED") {
        g_recording.store(false);
        g_paused.store(false);
        g_autoPaused.store(false);
        g_manualPaused.store(false);
        g_durationMs.store(0);
        std::function<void(std::string)> stopped;
        std::string path = StringOr(data, "outputPath");
        {
            const std::lock_guard lock(g_mutex);
            if (!path.empty()) {
                g_lastOutputPath = path;
            } else {
                // A stop event without a path still ends the wait, and the path
                // StopRecord gave us is the same file.
                path = g_lastOutputPath;
            }
            stopped.swap(g_takeStopped);
        }
        if (stopped) {
            stopped(path);
        }
    } else if (state == "OBS_WEBSOCKET_OUTPUT_PAUSED") {
        g_paused.store(true);
        g_pauseSupported.store(Tri::Yes);
    } else if (state == "OBS_WEBSOCKET_OUTPUT_RESUMED") {
        g_paused.store(false);
        g_pauseSupported.store(Tri::Yes);
    } else {
        g_recording.store(active);
    }
}

void OnEvent(const json& payload) {
    const auto type = StringOr(payload, "eventType");
    const auto data = payload.value("eventData", json::object());
    if (type == "RecordStateChanged") {
        OnRecordState(data);
    } else if (type == "CurrentProgramSceneChanged") {
        const std::lock_guard lock(g_mutex);
        g_scene = StringOr(data, "sceneName");
    }
}

void OnResponse(const json& payload) {
    const auto id = StringOr(payload, "requestId");
    PendingRequest request;
    {
        const std::lock_guard lock(g_mutex);
        const auto found = g_pending.find(id);
        if (found == g_pending.end()) {
            return;
        }
        request = std::move(found->second);
        g_pending.erase(found);
    }

    const auto status = payload.value("requestStatus", json::object());
    const bool ok = BoolOr(status, "result");
    const auto comment = StringOr(status, "comment");
    const auto data = payload.value("responseData", json::object());
    const auto rtt = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock_::now() - request.sentAt)
            .count());

    if (!ok) {
        spdlog::warn("Obs: {} refused: {}", request.type, comment.empty() ? "no reason" : comment);
    }
    if (request.reply) {
        request.reply(ok, NumberOr(status, "code"), data, comment, rtt);
    }
}

void OnMessage(const std::string& text) {
    json payload;
    try {
        payload = json::parse(text);
    } catch (const std::exception& e) {
        spdlog::warn("Obs: unparseable message: {}", e.what());
        return;
    }
    const auto op = NumberOr(payload, "op", 99);
    const auto data = payload.value("d", json::object());
    switch (op) {
    case 5:
        OnEvent(data);
        break;
    case 7:
        OnResponse(data);
        break;
    default:
        // Hello and Identified are consumed by the handshake before the reader
        // exists, so anything else here is a protocol version talking past us.
        spdlog::debug("Obs: ignoring op {}", op);
        break;
    }
}

void ReaderLoop(ConnPtr conn) {
    std::string text;
    while (ReceiveText(*conn, text, true)) {
        OnMessage(text);
    }

    bool wasCurrent = false;
    {
        const std::lock_guard lock(g_mutex);
        if (g_conn == conn) {
            g_conn.reset();
            wasCurrent = true;
        }
    }
    if (wasCurrent) {
        g_connected.store(false);
        g_recording.store(false);
        g_paused.store(false);
        g_autoPaused.store(false);
        g_manualPaused.store(false);
        g_pauseInFlight.store(false);
        FailPending("the OBS connection dropped");
        spdlog::info("Obs: disconnected");
        g_writerCv.notify_all();
    }
    // Last one out closes the handles.
}

// ── handshake ────────────────────────────────────────────────────────────────

bool Handshake(Conn& conn, const Endpoint& endpoint, std::string& error) {
    std::string text;
    if (!ReceiveText(conn, text)) {
        error = "OBS accepted the socket but sent no Hello";
        return false;
    }

    json hello;
    try {
        hello = json::parse(text);
    } catch (const std::exception& e) {
        error = std::format("the Hello did not parse: {}", e.what());
        return false;
    }
    if (NumberOr(hello, "op", 99) != 0) {
        error = "the first message was not a Hello";
        return false;
    }

    const auto data = hello.value("d", json::object());
    json identify{{"rpcVersion", kRpcVersion}, {"eventSubscriptions", kSubscribeOutputs}};

    if (const auto found = data.find("authentication");
        found != data.end() && found->is_object()) {
        if (endpoint.password.empty()) {
            error = "OBS wants a password and there is none - check [OBS] Password";
            return false;
        }
        const auto auth = AuthString(endpoint.password, StringOr(*found, "salt"),
                                     StringOr(*found, "challenge"));
        if (auth.empty()) {
            error = "could not compute the authentication string";
            return false;
        }
        identify["authentication"] = auth;
    }

    if (!SendText(conn, json{{"op", 1}, {"d", identify}}.dump())) {
        error = "could not send Identify";
        return false;
    }
    if (!ReceiveText(conn, text)) {
        // A password OBS does not accept is a close frame and nothing else, so
        // the guess is worth making here rather than leaving the log silent.
        error = "OBS closed the socket after Identify - wrong password?";
        return false;
    }
    try {
        if (NumberOr(json::parse(text), "op", 99) != 2) {
            error = "OBS did not answer Identify with Identified";
            return false;
        }
    } catch (const std::exception& e) {
        error = std::format("the Identified did not parse: {}", e.what());
        return false;
    }

    // Only now: up to here a silent OBS means a failed handshake and the read
    // wants a deadline, whereas from here a silent OBS is just an OBS with
    // nothing happening and the reader must sit on it indefinitely.
    DWORD infinite = 0;
    WinHttpSetOption(conn.socket, WINHTTP_OPTION_RECEIVE_TIMEOUT, &infinite, sizeof(infinite));

    spdlog::info("Obs: connected to {}:{} (obs-websocket {})", endpoint.host, endpoint.port,
                 StringOr(data, "obsWebSocketVersion", "?"));
    return true;
}

// ── what to ask the moment the socket is up ──────────────────────────────────

void ProbePauseSupport();

void OnConnected() {
    Enqueue("GetVersion", json::object(),
            [](bool ok, std::uint64_t, const json& data, const std::string&, std::uint64_t) {
                if (!ok) {
                    return;
                }
                const std::lock_guard lock(g_mutex);
                g_obsVersion = StringOr(data, "obsVersion");
                // There is no version test for CreateRecordChapter worth making
                // when OBS will simply list what it accepts.
                if (const auto found = data.find("availableRequests");
                    found != data.end() && found->is_array()) {
                    for (const auto& entry : *found) {
                        if (entry.is_string() && entry.get<std::string>() == "CreateRecordChapter") {
                            g_chapterSupported.store(true);
                            break;
                        }
                    }
                }
            });

    Enqueue("GetRecordStatus", json::object(),
            [](bool ok, std::uint64_t, const json& data, const std::string&, std::uint64_t) {
                if (!ok) {
                    return;
                }
                g_recording.store(BoolOr(data, "outputActive"));
                g_paused.store(BoolOr(data, "outputPaused"));
                g_durationMs.store(NumberOr(data, "outputDuration"));
            });

    Enqueue("GetSceneList", json::object(),
            [](bool ok, std::uint64_t, const json& data, const std::string&, std::uint64_t) {
                if (!ok) {
                    return;
                }
                std::vector<std::string> names;
                if (const auto found = data.find("scenes");
                    found != data.end() && found->is_array()) {
                    for (const auto& entry : *found) {
                        if (auto name = StringOr(entry, "sceneName"); !name.empty()) {
                            names.push_back(std::move(name));
                        }
                    }
                }
                {
                    const std::lock_guard lock(g_mutex);
                    g_scenes = std::move(names);
                    g_scene = StringOr(data, "currentProgramSceneName");
                }
            });

    Enqueue("GetRecordDirectory", json::object(),
            [](bool ok, std::uint64_t, const json& data, const std::string&, std::uint64_t) {
                if (!ok) {
                    return;
                }
                const std::lock_guard lock(g_mutex);
                g_recordDirectory = StringOr(data, "recordDirectory");
            });

    ProbePauseSupport();
}

/// A guess, and only a guess. OBS greys its own Pause button out whenever the
/// recording shares the stream encoder, and obs-websocket has no query for it -
/// so the profile is read to decide whether to draw the row at all, and the first
/// PauseRecord that comes back refused is what settles the question for good.
void ProbePauseSupport() {
    Enqueue("GetProfileParameter",
            json{{"parameterCategory", "Output"}, {"parameterName", "Mode"}},
            [](bool ok, std::uint64_t, const json& data, const std::string&, std::uint64_t) {
                if (!ok) {
                    return;
                }
                const auto mode = StringOr(data, "parameterValue",
                                           StringOr(data, "defaultParameterValue", "Simple"));
                const bool advanced = mode == "Advanced";
                const char* category = advanced ? "AdvOut" : "SimpleOutput";
                const char* name = advanced ? "RecEncoder" : "RecQuality";
                Enqueue("GetProfileParameter",
                        json{{"parameterCategory", category}, {"parameterName", name}},
                        [advanced](bool inner, std::uint64_t, const json& reply,
                                   const std::string&, std::uint64_t) {
                            if (!inner) {
                                return;
                            }
                            const auto value = StringOr(
                                reply, "parameterValue",
                                StringOr(reply, "defaultParameterValue"));
                            // "Same as stream" either way: the recording is the
                            // stream encoder's output and cannot be paused
                            // independently of it.
                            const bool shared = advanced ? value == "none" : value == "Stream";
                            g_pauseSupported.store(shared ? Tri::No : Tri::Yes);
                            spdlog::info("Obs: pausing looks {} ({} = {})",
                                         shared ? "unavailable" : "available",
                                         advanced ? "AdvOut/RecEncoder" : "SimpleOutput/RecQuality",
                                         value);
                        });
            });
}

// ── the writer ───────────────────────────────────────────────────────────────

void Announce(const std::string& text) {
    if (!g_announcedFailure.exchange(true)) {
        Note(text);
    } else {
        spdlog::warn("Obs: {}", text);
    }
}

/// True when there is something waiting that a connection would let us do. A
/// failed connect stays quiet unless somebody actually asked for something.
bool QueueHasWork() {
    const std::lock_guard lock(g_mutex);
    return !g_queue.empty();
}

void WriterLoop() {
    auto backoff = kBackoffFirst;
    auto lastPoll = Clock_::now();

    for (;;) {
        if (!g_connected.load()) {
            if (!g_wantConnected.load()) {
                std::unique_lock lock(g_mutex);
                g_writerCv.wait_for(lock, std::chrono::seconds(5));
                continue;
            }

            const auto endpoint = Resolve();
            if (!endpoint.serverEnabled) {
                Announce("OBS: the WebSocket server is switched off - Tools -> WebSocket Server "
                         "Settings");
                FailPending("the OBS WebSocket server is switched off");
                std::unique_lock lock(g_mutex);
                g_queue.clear();
                g_writerCv.wait_for(lock, kBackoffMax);
                continue;
            }

            std::string error;
            auto conn = Dial(endpoint, error);
            if (conn && !Handshake(*conn, endpoint, error)) {
                conn.reset();
            }
            if (!conn) {
                if (QueueHasWork()) {
                    Announce(std::format("OBS: {}", error));
                    FailPending(error);
                    const std::lock_guard lock(g_mutex);
                    g_queue.clear();
                } else {
                    spdlog::info("Obs: not connected: {}", error);
                }
                std::unique_lock lock(g_mutex);
                g_writerCv.wait_for(lock, backoff);
                backoff = std::min(backoff * 2, kBackoffMax);
                continue;
            }

            backoff = kBackoffFirst;
            g_announcedFailure.store(false);
            {
                const std::lock_guard lock(g_mutex);
                g_conn = conn;
            }
            g_connected.store(true);
            std::thread(ReaderLoop, conn).detach();
            OnConnected();
        }

        std::deque<Command> batch;
        ConnPtr conn;
        {
            std::unique_lock lock(g_mutex);
            if (g_queue.empty()) {
                g_writerCv.wait_for(lock, kPollInterval,
                                    [] { return !g_queue.empty() || !g_connected.load(); });
            }
            if (!g_connected.load()) {
                continue;
            }
            batch.swap(g_queue);
            conn = g_conn;
        }
        if (!conn) {
            continue;
        }

        for (auto& command : batch) {
            const auto id = std::format("qmm-{}", g_nextRequestId++);
            const json frame{{"op", 6},
                             {"d",
                              {{"requestType", command.type},
                               {"requestId", id},
                               {"requestData", command.data}}}};
            {
                const std::lock_guard lock(g_mutex);
                g_pending.emplace(id, PendingRequest{command.type, std::move(command.reply),
                                                     Clock_::now()});
            }
            if (!SendText(*conn, frame.dump())) {
                // The reader will notice the same break and do the tidying; all
                // that is needed here is to stop feeding a dead socket.
                g_connected.store(false);
                break;
            }
        }

        // Only while recording, and only for the timecode in the row - the
        // recording state itself arrives as an event.
        const auto now = Clock_::now();
        if (g_connected.load() && g_recording.load() && now - lastPoll >= kPollInterval) {
            lastPoll = now;
            Enqueue("GetRecordStatus", json::object(),
                    [](bool ok, std::uint64_t, const json& data, const std::string&, std::uint64_t) {
                        if (!ok) {
                            return;
                        }
                        g_recording.store(BoolOr(data, "outputActive"));
                        g_paused.store(BoolOr(data, "outputPaused"));
                        g_durationMs.store(NumberOr(data, "outputDuration"));
                    });
        }
    }
}

void EnsureThread() {
    if (g_threadRunning.exchange(true)) {
        return;
    }
    // Detached, like every other owned thread in this plugin. Skyrim has no
    // plugin unload, and joining one of these from a static destructor during
    // DLL teardown is how you hang the exit.
    std::thread(WriterLoop).detach();
}

/// Put OBS's own FilenameFormatting back. An empty saved value means it was
/// never set, and null is how obs-websocket says "back to the default".
void RestoreFilenameFormat() {
    std::string saved;
    {
        const std::lock_guard lock(g_mutex);
        if (!g_haveSavedFilenameFormat) {
            return;
        }
        saved = g_savedFilenameFormat;
        g_haveSavedFilenameFormat = false;
    }
    json data{{"parameterCategory", "Output"}, {"parameterName", "FilenameFormatting"}};
    if (saved.empty()) {
        data["parameterValue"] = nullptr;
    } else {
        data["parameterValue"] = saved;
    }
    Enqueue("SetProfileParameter", std::move(data));
}

/// RequestStatus codes that mean "the output is already in the state you asked
/// for", or "there is no output to ask about". None of them says anything about
/// whether pausing is possible, and treating them as if they did is how a
/// harmless duplicate request switches the feature off for the session.
constexpr std::uint64_t kOutputRunning = 702;
constexpr std::uint64_t kOutputNotRunning = 703;
constexpr std::uint64_t kOutputPaused = 704;
constexpr std::uint64_t kOutputNotPaused = 705;

void SendPause(bool paused) {
    // A pause and a resume racing each other would do exactly that: the menu
    // opening queues a pause, the row reads a snapshot that has not caught up,
    // and picking it sends a second one that OBS refuses because it is already
    // paused. Nothing is queued while an answer is outstanding.
    if (g_pauseInFlight.exchange(true)) {
        return;
    }
    Enqueue(paused ? "PauseRecord" : "ResumeRecord", json::object(),
            [paused](bool ok, std::uint64_t code, const json&, const std::string& comment,
                     std::uint64_t) {
                g_pauseInFlight.store(false);
                if (ok) {
                    g_pauseSupported.store(Tri::Yes);
                    return;
                }
                if (code == kOutputPaused || code == kOutputNotPaused) {
                    // Already there. Believe OBS over the snapshot and move on.
                    g_paused.store(code == kOutputPaused);
                    g_pauseSupported.store(Tri::Yes);
                    return;
                }
                if (code == kOutputNotRunning || code == kOutputRunning) {
                    // The recording ended under us. Nothing to say about pausing.
                    return;
                }
                // Anything else is OBS declining on the merits, and there is no
                // capability query - so this *is* the capability query. Latched
                // for the session: the pause rows and the auto-pause disappear
                // rather than failing again on every menu open.
                g_pauseSupported.store(Tri::No);
                g_autoPaused.store(false);
                g_manualPaused.store(false);
                NoteF("OBS cannot pause this recording{}{}",
                              comment.empty() ? "" : ": ", comment);
            });
}

}  // namespace

// ── public ───────────────────────────────────────────────────────────────────

bool Configured() { return Settings().enabled; }

bool Launch(std::string& error) {
    const std::filesystem::path& exe = Settings().obsExe;
    if (exe.empty()) {
        error = "no OBS path - set [Devbench] sObsPath in RagdollSounds.ini";
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::exists(exe, ec)) {
        error = std::format("{} is not there", exe.string());
        return false;
    }

    // Working directory set to obs64.exe's own folder, which OBS requires: it
    // resolves its locale, its plugins and its data relative to the process
    // working directory, and started from anywhere else it comes up with no
    // scenes and an error box.
    std::wstring commandLine = L"\"" + exe.wstring() + L"\" --disable-shutdown-check";
    const std::wstring directory = exe.parent_path().wstring();

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        directory.c_str(), &si, &pi)) {
        error = std::format("CreateProcess failed ({})", GetLastError());
        return false;
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    NoteF("started {} - it takes a few seconds to come up", exe.filename().string());
    return true;
}

Status Now() {
    Status status;
    status.connected = g_connected.load();
    status.recording = g_recording.load();
    status.paused = g_paused.load();
    status.autoPaused = g_autoPaused.load();
    status.pauseSupported = g_pauseSupported.load();
    status.chapterSupported = g_chapterSupported.load();
    status.durationMs = g_durationMs.load();
    {
        const std::lock_guard lock(g_mutex);
        status.scene = g_scene;
    }
    return status;
}

std::string Clock() {
    const auto seconds = g_durationMs.load() / 1000;
    const auto hours = seconds / 3600;
    if (hours > 0) {
        return std::format("{}:{:02}:{:02}", hours, (seconds / 60) % 60, seconds % 60);
    }
    return std::format("{:02}:{:02}", seconds / 60, seconds % 60);
}

std::string Summary() {
    if (!g_connected.load()) {
        return "not connected";
    }
    if (!g_recording.load()) {
        return "connected";
    }
    return std::format("{} {}", g_paused.load() ? "paused" : "recording", Clock());
}

void Connect() {
    if (!Configured()) {
        return;
    }
    g_wantConnected.store(true);
    EnsureThread();
    g_writerCv.notify_all();
}

void Disconnect() {
    ConnPtr conn;
    {
        const std::lock_guard lock(g_mutex);
        conn = g_conn;
    }
    g_wantConnected.store(false);
    if (conn && conn->socket) {
        // A close frame, not a closed handle: OBS answers it, the reader's
        // receive returns of its own accord, and nobody is ever inside a WinHTTP
        // call on a handle somebody else has freed.
        conn->closing.store(true);
        WinHttpWebSocketShutdown(conn->socket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
    }
}

void StartRecording() {
    if (!Configured()) {
        return;
    }
    // Asking again is asking to be told again. Without this, an OBS that is not
    // running explains itself once and is silent for the rest of the session.
    g_announcedFailure.store(false);
    if (const auto& scene = Settings().scene; !scene.empty()) {
        // Queued ahead of the start on the one socket, so the scene is live
        // before the output opens rather than a second into the take.
        Enqueue("SetCurrentProgramScene", json{{"sceneName", scene}});
    }
    Enqueue("StartRecord", json::object(),
            [](bool ok, std::uint64_t, const json&, const std::string& comment, std::uint64_t) {
                if (ok) {
                    Note("OBS is recording");
                } else {
                    NoteF("OBS did not start recording{}{}", comment.empty() ? "" : ": ",
                                  comment);
                }
            });
}

void StartRecordingOnMenuClose() {
    if (Configured()) {
        g_startOnMenuClose.store(true);
    }
}

void StopRecording() {
    Enqueue("StopRecord", json::object(),
            [](bool ok, std::uint64_t, const json& data, const std::string& comment, std::uint64_t) {
                if (!ok) {
                    NoteF("OBS did not stop recording{}{}", comment.empty() ? "" : ": ",
                                  comment);
                    return;
                }
                const auto path = StringOr(data, "outputPath");
                {
                    const std::lock_guard lock(g_mutex);
                    g_lastOutputPath = path;
                }
                if (path.empty()) {
                    Note("OBS stopped recording");
                } else {
                    NoteF("OBS wrote {}",
                                  std::filesystem::path(path).filename().string());
                }
            });
}

void SetManualPause(bool paused) {
    if (!Configured() || !g_recording.load() || g_pauseSupported.load() == Tri::No) {
        return;
    }
    g_manualPaused.store(paused);
    // A manual resume also clears the auto-pause, or closing the menu would
    // "resume" a recording the player has just resumed by hand.
    g_autoPaused.store(false);
    SendPause(paused);
}

void MenuOpened() {
    if (!Configured() || !Settings().autoPauseInMenu) {
        return;
    }
    if (!g_recording.load() || g_paused.load() || g_manualPaused.load()) {
        return;
    }
    if (g_pauseSupported.load() == Tri::No) {
        return;
    }
    // A take exists to be measured, and a paused stretch is a hole in the
    // timeline that the sync track then has to model. Adding an actor to a
    // running take means opening the menu, so this is not a rare case.
    if (g_takeActive.load() && !Settings().pauseImpactTakesInMenu) {
        return;
    }
    if (g_autoPaused.exchange(true)) {
        return;
    }
    SendPause(true);
}

void MenuClosed() {
    if (g_autoPaused.exchange(false) && !g_manualPaused.load()) {
        SendPause(false);
    }
    // The start waits for the menu to be down, so the list panel is not in the
    // first seconds of the take.
    if (g_startOnMenuClose.exchange(false)) {
        StartRecording();
    }
}

void Chapter(std::string name) {
    if (!Configured() || !g_chapterSupported.load() || !g_recording.load()) {
        return;
    }
    Enqueue("CreateRecordChapter", json{{"chapterName", std::move(name)}});
}

void SetTakeActive(bool active) { g_takeActive.store(active); }

void StartTake(std::string stem, std::uint32_t timeoutMs, std::function<void(bool)> armed) {
    auto fire = [armed = std::move(armed)](bool started) {
        Defer([armed, started]() { armed(started); });
    };
    if (!Configured()) {
        fire(false);
        return;
    }

    // Read the player's own format before overwriting it. Quietly rewriting
    // somebody's OBS profile and leaving it that way is not acceptable, and
    // there is nowhere else this value survives.
    Enqueue("GetProfileParameter",
            json{{"parameterCategory", "Output"}, {"parameterName", "FilenameFormatting"}},
            [](bool ok, std::uint64_t, const json& data, const std::string&, std::uint64_t) {
                if (!ok) {
                    return;
                }
                const std::lock_guard lock(g_mutex);
                g_savedFilenameFormat = StringOr(data, "parameterValue");
                g_haveSavedFilenameFormat = true;
            });
    Enqueue("SetProfileParameter", json{{"parameterCategory", "Output"},
                                        {"parameterName", "FilenameFormatting"},
                                        {"parameterValue", stem}});

    // Whichever comes first: OBS saying the output is running, OBS refusing, or
    // the timeout. The flag makes it once-only, so a late event after a timeout
    // cannot arm a take twice.
    auto once = std::make_shared<std::atomic_flag>();
    auto guarded = [once, fire](bool started) {
        if (once->test_and_set()) {
            return;
        }
        // A take with no video never reaches StopTake, so this is the only place
        // the player's own FilenameFormatting gets put back when OBS refuses the
        // start or never answers it. Leaving their profile rewritten because the
        // recording failed would be the worst of both.
        if (!started) {
            RestoreFilenameFormat();
        }
        fire(started);
    };
    {
        const std::lock_guard lock(g_mutex);
        g_takeArmed = guarded;
    }
    Enqueue("StartRecord", json::object(),
            [guarded](bool ok, std::uint64_t, const json&, const std::string& comment, std::uint64_t) {
                if (!ok) {
                    spdlog::warn("Obs: the take has no video: {}", comment);
                    guarded(false);
                }
            });
    DeferAfter(timeoutMs, [guarded]() { guarded(false); });
}

void StopTake(std::function<void(std::string)> done) {
    if (!Configured()) {
        Defer([done]() { done({}); });
        return;
    }

    // Once-only, so a stopped event arriving after the timeout cannot hand the
    // same take a second video.
    auto once = std::make_shared<std::atomic_flag>();
    auto guarded = [once, done](std::string path) {
        if (once->test_and_set()) {
            return;
        }
        Defer([done, path]() { done(path); });
    };
    {
        const std::lock_guard lock(g_mutex);
        g_takeStopped = guarded;
    }

    Enqueue("StopRecord", json::object(),
            [guarded](bool ok, std::uint64_t, const json& data, const std::string&, std::uint64_t) {
                const std::string path = ok ? StringOr(data, "outputPath") : std::string{};
                if (!path.empty()) {
                    const std::lock_guard lock(g_mutex);
                    g_lastOutputPath = path;
                }
                // Only a refusal finishes here. A stop that was accepted waits
                // for the stopped event, because the file is not closed yet.
                if (!ok) {
                    guarded({});
                }
            });
    // A long recording takes real time to finalise, and finishing the take
    // without its video is a better outcome than moving a half-written file. The
    // path is still handed over on the timeout - the take is the user's either
    // way - but it has had every chance to be a whole one first.
    DeferAfter(kStopFinaliseTimeoutMs, [guarded]() {
        std::string path;
        {
            const std::lock_guard lock(g_mutex);
            path = g_lastOutputPath;
        }
        guarded(path);
    });

    // After the stop, so the file being closed keeps the name it was opened with.
    RestoreFilenameFormat();
}

void SampleClock(std::function<void(std::uint64_t, std::uint64_t)> sample) {
    if (!Configured() || !g_connected.load() || !g_recording.load()) {
        return;
    }
    Enqueue("GetRecordStatus", json::object(),
            [sample](bool ok, std::uint64_t, const json& data, const std::string&, std::uint64_t rtt) {
                if (!ok) {
                    return;
                }
                const auto duration = NumberOr(data, "outputDuration");
                g_durationMs.store(duration);
                Defer([sample, duration, rtt]() { sample(duration, rtt); });
            });
}

std::string LastOutputPath() {
    const std::lock_guard lock(g_mutex);
    return g_lastOutputPath;
}

std::string Version() {
    const std::lock_guard lock(g_mutex);
    return g_obsVersion;
}

std::string RecordDirectory() {
    const std::lock_guard lock(g_mutex);
    return g_recordDirectory;
}

void SetScene(std::string scene) {
    if (scene.empty()) {
        return;
    }
    Enqueue("SetCurrentProgramScene", json{{"sceneName", std::move(scene)}});
}

std::vector<std::string> Scenes() {
    const std::lock_guard lock(g_mutex);
    return g_scenes;
}

}  // namespace tb::obs
