#include "Link.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstring>

namespace tb {
namespace {

using Clock = std::chrono::steady_clock;

/// How long the read waits before going back round to flush the send queue.
/// Also the worst-case latency on a slider move reaching the game.
constexpr int kPollMs = 20;

[[nodiscard]] double SecondsSince(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

}  // namespace

const rds::link::ProfileMessage* LiveCapture::Subject() const {
    // The actor with the most contacts. A knockdown near a second tracked actor
    // produces two profiles, and the take is about whichever one actually fell
    // over - which is a count of contacts, not the order they arrived in.
    std::map<rds::ActorId, std::size_t> contacts;
    for (const rds::FeedEvent& event : events) {
        if (event.IsContact()) {
            ++contacts[event.actorId];
        }
    }
    const rds::link::ProfileMessage* best = nullptr;
    std::size_t bestCount = 0;
    for (const auto& [actor, profile] : profiles) {
        const auto it = contacts.find(actor);
        const std::size_t count = it == contacts.end() ? 0 : it->second;
        if (best == nullptr || count > bestCount) {
            best = &profile;
            bestCount = count;
        }
    }
    return best;
}

GameLink::~GameLink() { Stop(); }

bool GameLink::Start(std::uint16_t port) {
    Stop();
    m_port = port;
    m_stop.store(false, std::memory_order_relaxed);

    // Opened here rather than on the thread so a port clash is an answer this
    // call can give, and the UI can say "another testbench is running" at
    // startup instead of half a second later.
    rds::link::Socket listener;
    std::string error;
    if (!listener.Listen(port, error)) {
        std::lock_guard lock{m_mutex};
        m_snapshot = LinkSnapshot{};
        m_snapshot.error = error;
        spdlog::warn("link: {}", error);
        return false;
    }
    {
        std::lock_guard lock{m_mutex};
        m_snapshot = LinkSnapshot{};
        m_snapshot.listening = true;
    }
    m_listening.store(true, std::memory_order_relaxed);

    m_thread = std::thread([this, listener = std::move(listener)]() mutable {
        while (!m_stop.load(std::memory_order_relaxed)) {
            rds::link::Socket client = listener.Accept(200);
            if (!client.Valid()) {
                continue;
            }
            Serve(client);
            m_connected.store(false, std::memory_order_relaxed);
            {
                std::lock_guard lock{m_mutex};
                m_snapshot.connected = false;
                m_snapshot.peerBuild.clear();
                m_snapshot.peerPid = 0;
            }
            {
                std::lock_guard lock{m_outMutex};
                m_staged.clear();
            }
        }
    });
    spdlog::info("link: listening on 127.0.0.1:{}", port);
    return true;
}

void GameLink::Stop() {
    m_stop.store(true, std::memory_order_relaxed);
    if (m_thread.joinable()) {
        m_thread.join();
    }
    m_listening.store(false, std::memory_order_relaxed);
    m_connected.store(false, std::memory_order_relaxed);
}

LinkSnapshot GameLink::Snapshot() const {
    std::lock_guard lock{m_mutex};
    return m_snapshot;
}

// ── pushing ──────────────────────────────────────────────────────────────────

void GameLink::Queue(rds::link::Msg type, const void* payload, std::size_t bytes) {
    if (!m_connected.load(std::memory_order_relaxed)) {
        return;
    }
    rds::link::Header header;
    header.type = static_cast<std::uint16_t>(type);
    header.size = static_cast<std::uint32_t>(bytes);

    std::lock_guard lock{m_outMutex};
    const std::size_t at = m_staged.size();
    m_staged.resize(at + sizeof(header) + bytes);
    std::memcpy(m_staged.data() + at, &header, sizeof(header));
    if (bytes > 0) {
        std::memcpy(m_staged.data() + at + sizeof(header), payload, bytes);
    }
}

void GameLink::PushAlgorithm(const rds::ConfigSet& config) {
    const std::string text = rds::link::EncodeAlgorithm(config);
    Queue(rds::link::Msg::kAlgorithm, text.data(), text.size());
}

void GameLink::PushSfx(const rds::SfxAssignments& assignments) {
    const std::string text = rds::link::EncodeSfx(assignments);
    Queue(rds::link::Msg::kSfxTable, text.data(), text.size());
}

void GameLink::PushLibraryPath(const std::string& path) {
    Queue(rds::link::Msg::kLibraryPath, path.data(), path.size());
}

void GameLink::PushClear() { Queue(rds::link::Msg::kClearOverrides, nullptr, 0); }

void GameLink::PushAudioMode(bool useVanillaAudio) {
    const std::uint8_t byte = useVanillaAudio ? 1u : 0u;
    Queue(rds::link::Msg::kAudioMode, &byte, sizeof(byte));
}

// ── capturing ────────────────────────────────────────────────────────────────

void GameLink::BeginCapture() {
    {
        std::lock_guard lock{m_mutex};
        // Whatever profiles the session already sent, carried into the take:
        // the game sends one per actor per attach, and an actor tracked before
        // the record button was pressed would otherwise have none. Moved out
        // before the reset and put back after it, because a plain
        // `m_capture = LiveCapture{}` is what threw them away - and a take with
        // no profile is a take with no actor name and no limb table, every limb
        // cell in the impacts view reading "?".
        auto profiles = std::move(m_capture.profiles);
        m_capture = LiveCapture{};
        m_capture.profiles = std::move(profiles);
        m_capturedContacts = 0;
        m_capture.startMs = m_snapshot.lastEventMs;
    }
    m_capturing.store(true, std::memory_order_relaxed);
}

LiveCapture GameLink::EndCapture() {
    m_capturing.store(false, std::memory_order_relaxed);
    std::lock_guard lock{m_mutex};
    LiveCapture out = std::move(m_capture);
    m_capture = LiveCapture{};
    // The take keeps its own copy and the session keeps the originals: those
    // actors are still attached, and the next take needs their limb tables just
    // as much as this one did.
    m_capture.profiles = out.profiles;
    m_capturedContacts = 0;
    return out;
}

void GameLink::CancelCapture() {
    m_capturing.store(false, std::memory_order_relaxed);
    std::lock_guard lock{m_mutex};
    auto profiles = std::move(m_capture.profiles);
    m_capture = LiveCapture{};
    m_capture.profiles = std::move(profiles);
    m_capturedContacts = 0;
}

std::size_t GameLink::CapturedEvents() const {
    std::lock_guard lock{m_mutex};
    return m_capture.events.size();
}

std::size_t GameLink::CapturedContacts() const {
    std::lock_guard lock{m_mutex};
    return m_capturedContacts;
}

double GameLink::CapturedDurationMs() const {
    std::lock_guard lock{m_mutex};
    return m_capture.Empty() ? 0.0 : m_capture.DurationMs();
}

bool GameLink::GameClock(double& sessionMs) const {
    std::lock_guard lock{m_mutex};
    if (!m_haveClockOffset) {
        return false;
    }
    const double localMs =
        std::chrono::duration<double, std::milli>(Clock::now() - m_epoch).count();
    sessionMs = localMs - m_clockOffsetMs;
    return true;
}

// ── the socket thread ────────────────────────────────────────────────────────

void GameLink::OnEvents(const std::byte* data, std::size_t bytes) {
    const std::size_t count = bytes / sizeof(rds::FeedEvent);
    if (count == 0) {
        return;
    }
    std::vector<rds::FeedEvent> batch(count);
    std::memcpy(batch.data(), data, count * sizeof(rds::FeedEvent));

    std::lock_guard lock{m_mutex};
    m_snapshot.eventsReceived += count;
    for (const rds::FeedEvent& event : batch) {
        if (event.IsContact()) {
            ++m_snapshot.contactsReceived;
        }
        m_snapshot.lastEventMs = std::max(m_snapshot.lastEventMs, event.timeMs);
    }
    if (!m_capturing.load(std::memory_order_relaxed)) {
        return;
    }
    if (m_capture.events.empty()) {
        // The first event decides where the take starts, not the button press:
        // a record started during a quiet stretch would otherwise carry seconds
        // of nothing in front of the fall.
        m_capture.startMs = batch.front().timeMs;
    }
    for (const rds::FeedEvent& event : batch) {
        if (event.IsContact()) {
            ++m_capturedContacts;
        }
        m_capture.endMs = std::max(m_capture.endMs, event.timeMs);
    }
    m_capture.events.insert(m_capture.events.end(), batch.begin(), batch.end());
}

void GameLink::OnProfile(std::string_view text) {
    rds::link::ProfileMessage message;
    if (!rds::link::DecodeProfile(text, message)) {
        return;
    }
    std::lock_guard lock{m_mutex};
    // Kept whether or not a capture is running. The game sends one per actor per
    // attach, so a knockdown recorded five seconds after the actor was picked up
    // would have no limb table at all if these were only collected while
    // recording.
    m_capture.profiles[message.profile.actorId] = std::move(message);
}

void GameLink::Serve(rds::link::Socket& client) {
    std::vector<std::byte> payload;
    rds::link::Msg type{};

    if (rds::link::ReadMessage(client, type, payload, 3000) != 1 ||
        type != rds::link::Msg::kHello || payload.size() < sizeof(rds::link::HelloPacket)) {
        return;
    }
    rds::link::HelloPacket hello{};
    std::memcpy(&hello, payload.data(), sizeof(hello));

    const bool sameLayout = hello.feedEventSize == sizeof(rds::FeedEvent);

    rds::link::WelcomePacket welcome;
    welcome.acceptsEvents = sameLayout ? 1 : 0;
    std::snprintf(welcome.build, sizeof(welcome.build), "%s", __DATE__);
    if (!rds::link::WriteMessage(client, rds::link::Msg::kWelcome, &welcome, sizeof(welcome))) {
        return;
    }

    {
        std::lock_guard lock{m_mutex};
        m_snapshot.connected = true;
        m_snapshot.peerBuild = hello.build;
        m_snapshot.peerPid = hello.processId;
        m_snapshot.acceptsEvents = sameLayout;
        m_snapshot.eventsReceived = 0;
        m_snapshot.contactsReceived = 0;
        m_snapshot.lastEventMs = 0.0;
        m_snapshot.game = rds::link::StatusPacket{};
        // A new game is a new session clock, counting from its own zero again.
        m_haveClockOffset = false;
        m_clockOffsetMs = 0.0;
    }
    m_connected.store(true, std::memory_order_relaxed);

    spdlog::info("link: a game built {} connected (pid {}){}", hello.build, hello.processId,
                 sameLayout ? ""
                            : " - its FeedEvent layout differs from ours, so its event stream is "
                              "refused; rebuild both halves from the same tree");

    const auto opened = Clock::now();
    std::vector<std::byte> outgoing;
    while (!m_stop.load(std::memory_order_relaxed)) {
        {
            std::lock_guard lock{m_outMutex};
            outgoing.swap(m_staged);
            m_staged.clear();
        }
        if (!outgoing.empty()) {
            if (!client.SendAll(outgoing.data(), outgoing.size())) {
                break;
            }
            outgoing.clear();
        }

        const int got = rds::link::ReadMessage(client, type, payload, kPollMs);
        if (got < 0) {
            break;
        }
        if (got == 0) {
            continue;
        }
        switch (type) {
            case rds::link::Msg::kEvents:
                if (sameLayout) {
                    OnEvents(payload.data(), payload.size());
                }
                break;
            case rds::link::Msg::kProfile:
                OnProfile({reinterpret_cast<const char*>(payload.data()), payload.size()});
                break;
            case rds::link::Msg::kStatus: {
                if (payload.size() < sizeof(rds::link::StatusPacket)) {
                    break;
                }
                std::lock_guard lock{m_mutex};
                const double previousSessionMs = m_snapshot.game.sessionMs;
                std::memcpy(&m_snapshot.game, payload.data(), sizeof(rds::link::StatusPacket));
                m_lastStatusSec = SecondsSince(opened);
                m_snapshot.gameStatusAgeSec = 0.0;
                // And the epoch between the two clocks, kept at its minimum -
                // see GameClock() for why the smallest sample is the true one.
                //
                // A session clock that went backwards is a game that reset its
                // own epoch under us, and every sample before it now describes a
                // clock that no longer exists. Nothing does that today - the
                // reset happens at plugin init, before anything can connect - but
                // the failure it would cause is a take that lines up with nothing
                // and says nothing about why.
                {
                    if (m_snapshot.game.sessionMs < previousSessionMs) {
                        m_haveClockOffset = false;
                    }
                    const double localMs =
                        std::chrono::duration<double, std::milli>(Clock::now() - m_epoch).count();
                    const double offset = localMs - m_snapshot.game.sessionMs;
                    if (!m_haveClockOffset || offset < m_clockOffsetMs) {
                        m_clockOffsetMs = offset;
                        m_haveClockOffset = true;
                    }
                }
                break;
            }
            case rds::link::Msg::kPing:
                if (!rds::link::WriteMessage(client, rds::link::Msg::kPong)) {
                    return;
                }
                break;
            default:
                break;
        }
        {
            std::lock_guard lock{m_mutex};
            m_snapshot.gameStatusAgeSec = SecondsSince(opened) - m_lastStatusSec;
        }
    }
    spdlog::info("link: the game disconnected after {:.0f} s", SecondsSince(opened));
}

}  // namespace tb
