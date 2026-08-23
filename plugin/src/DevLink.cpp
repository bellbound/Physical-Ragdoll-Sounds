#include "DevLink.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <cstring>

namespace rds::game {
namespace {

/// How long to wait between attempts to find the testbench.
///
/// Flat rather than backing off: the whole point is that you can start the
/// testbench at any moment in a three-hour session and have the game find it,
/// and a backoff that reached thirty seconds would turn that into "restart
/// Skyrim". A connect attempt to a dead loopback port is a few microseconds.
constexpr auto kReconnectDelay = std::chrono::milliseconds(2000);

/// How long the read waits before going back round to flush the outbound queue.
/// Short, because it is also the worst case latency on a config push.
constexpr int kPollMs = 25;

}  // namespace

DevLink::~DevLink() { Stop(); }

void DevLink::Start(const DevbenchConfig& config) {
    Stop();
    m_config = config;
    if (!m_config.enabled) {
        return;
    }
    m_stop.store(false, std::memory_order_relaxed);
    m_running.store(true, std::memory_order_relaxed);
    m_thread = std::thread([this] { Run(); });
    spdlog::info("devbench: looking for a testbench on 127.0.0.1:{}", m_config.port);
}

void DevLink::Stop() {
    m_stop.store(true, std::memory_order_relaxed);
    if (m_thread.joinable()) {
        m_thread.join();
    }
    m_running.store(false, std::memory_order_relaxed);
    m_connected.store(false, std::memory_order_relaxed);
    std::lock_guard lock{m_outMutex};
    m_staged.clear();
}

// ═════════════════════════════════════════════════════════════════════════════
// the game thread's side
// ═════════════════════════════════════════════════════════════════════════════

void DevLink::Queue(link::Msg type, const void* payload, std::size_t bytes) {
    if (!m_connected.load(std::memory_order_relaxed)) {
        // Nothing is listening. Dropped here rather than staged, so a session
        // played without the testbench does not build a backlog that all arrives
        // at once the moment it is started.
        return;
    }
    link::Header header;
    header.type = static_cast<std::uint16_t>(type);
    header.size = static_cast<std::uint32_t>(bytes);

    std::lock_guard lock{m_outMutex};
    if (m_staged.size() + sizeof(header) + bytes > kMaxStagedBytes) {
        m_droppedFrames.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const std::size_t at = m_staged.size();
    m_staged.resize(at + sizeof(header) + bytes);
    std::memcpy(m_staged.data() + at, &header, sizeof(header));
    if (bytes > 0) {
        std::memcpy(m_staged.data() + at + sizeof(header), payload, bytes);
    }
}

void DevLink::PushEvents(const FeedEvent* events, std::size_t count) {
    if (count == 0 || !m_acceptsEvents.load(std::memory_order_relaxed)) {
        return;
    }
    Queue(link::Msg::kEvents, events, count * sizeof(FeedEvent));
}

void DevLink::PushProfile(const link::ProfileMessage& message) {
    const std::string text = link::EncodeProfile(message);
    Queue(link::Msg::kProfile, text.data(), text.size());
    std::lock_guard lock{m_stateMutex};
    m_profilesSent.insert(message.profile.actorId);
}

void DevLink::PushStatus(const link::StatusPacket& status) {
    Queue(link::Msg::kStatus, &status, sizeof(status));
}

void DevLink::SetCell(std::string cell) {
    std::lock_guard lock{m_stateMutex};
    m_cell = std::move(cell);
}

std::string DevLink::Cell() const {
    std::lock_guard lock{m_stateMutex};
    return m_cell;
}

bool DevLink::ProfileSent(ActorId actor) const {
    std::lock_guard lock{m_stateMutex};
    return m_profilesSent.contains(actor);
}

void DevLink::ForgetProfile(ActorId actor) {
    std::lock_guard lock{m_stateMutex};
    m_profilesSent.erase(actor);
}

bool DevLink::Take(Pending& out) {
    std::lock_guard lock{m_inMutex};
    if (!m_pending.Any()) {
        return false;
    }
    out = std::move(m_pending);
    m_pending = Pending{};
    return true;
}

// ═════════════════════════════════════════════════════════════════════════════
// the socket thread
// ═════════════════════════════════════════════════════════════════════════════

void DevLink::Run() {
    while (!m_stop.load(std::memory_order_relaxed)) {
        link::Socket socket;
        if (!socket.Connect(static_cast<std::uint16_t>(m_config.port), 300)) {
            // The ordinary case. No log line: this runs every two seconds for the
            // whole session, and a warning here is how a useful log becomes one
            // nobody opens.
            std::this_thread::sleep_for(kReconnectDelay);
            continue;
        }
        Session(socket);
        m_connected.store(false, std::memory_order_relaxed);
        m_overrideFlags.store(0, std::memory_order_relaxed);
        {
            // Everything staged for a peer that is gone. Keeping it would send a
            // dead session's contacts to the next testbench that connects.
            std::lock_guard lock{m_outMutex};
            m_staged.clear();
        }
        {
            std::lock_guard lock{m_stateMutex};
            m_profilesSent.clear();
        }
        if (!m_stop.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(kReconnectDelay);
        }
    }
}

void DevLink::Session(link::Socket& socket) {
    link::HelloPacket hello;
    hello.processId = static_cast<std::uint32_t>(::GetCurrentProcessId());
    std::snprintf(hello.build, sizeof(hello.build), "%s", __DATE__);
    if (!link::WriteMessage(socket, link::Msg::kHello, &hello, sizeof(hello))) {
        return;
    }

    std::vector<std::byte> payload;
    link::Msg type{};
    if (link::ReadMessage(socket, type, payload, 3000) != 1 || type != link::Msg::kWelcome ||
        payload.size() < sizeof(link::WelcomePacket)) {
        return;
    }
    link::WelcomePacket welcome{};
    std::memcpy(&welcome, payload.data(), sizeof(welcome));
    m_acceptsEvents.store(welcome.acceptsEvents != 0, std::memory_order_relaxed);
    m_connected.store(true, std::memory_order_relaxed);

    spdlog::info("devbench: connected to a testbench built {} (protocol {}){}", welcome.build,
                 welcome.version,
                 welcome.acceptsEvents != 0
                     ? ""
                     : " - it will not read our event stream, the FeedEvent layouts differ");

    std::vector<std::byte> outgoing;
    while (!m_stop.load(std::memory_order_relaxed)) {
        // Flush first: a config push answered by three seconds of silence looks
        // like a broken link, and the reply to anything we are asked is whatever
        // is already queued.
        {
            std::lock_guard lock{m_outMutex};
            outgoing.swap(m_staged);
            m_staged.clear();
        }
        if (!outgoing.empty()) {
            if (!socket.SendAll(outgoing.data(), outgoing.size())) {
                break;
            }
            outgoing.clear();
        }

        const int got = link::ReadMessage(socket, type, payload, kPollMs);
        if (got < 0) {
            break;
        }
        if (got == 0) {
            continue;
        }

        const std::string_view text{reinterpret_cast<const char*>(payload.data()), payload.size()};
        switch (type) {
            case link::Msg::kAlgorithm: {
                // Decoded onto a default config rather than onto the ini's, so
                // what the game runs is exactly what the testbench is showing -
                // a key the testbench does not know about goes back to its
                // default rather than silently keeping the ini's value.
                AlgorithmConfig config{};
                link::DecodeAlgorithm(text, config);
                std::lock_guard lock{m_inMutex};
                m_pending.algorithm = true;
                m_pending.config = config;
                m_overrideFlags.fetch_or(1u, std::memory_order_relaxed);
                break;
            }
            case link::Msg::kSfxTable: {
                SfxAssignments table;
                link::DecodeSfx(text, table);
                std::lock_guard lock{m_inMutex};
                m_pending.sfx = true;
                m_pending.sfxTable = std::move(table);
                m_overrideFlags.fetch_or(2u, std::memory_order_relaxed);
                break;
            }
            case link::Msg::kLibraryPath: {
                std::lock_guard lock{m_inMutex};
                m_pending.library = true;
                m_pending.libraryPath.assign(text);
                m_overrideFlags.fetch_or(4u, std::memory_order_relaxed);
                break;
            }
            case link::Msg::kClearOverrides: {
                std::lock_guard lock{m_inMutex};
                m_pending.clear = true;
                m_overrideFlags.store(0, std::memory_order_relaxed);
                break;
            }
            case link::Msg::kPing:
                if (!link::WriteMessage(socket, link::Msg::kPong)) {
                    return;
                }
                break;
            default:
                break;
        }
    }
    spdlog::info("devbench: the testbench went away; listening for it again");
}

// ═════════════════════════════════════════════════════════════════════════════
// the tee
// ═════════════════════════════════════════════════════════════════════════════

bool LinkTap::Drain(TimeMs untilMs, std::vector<FeedEvent>& out) {
    const std::size_t before = out.size();
    const bool more = m_inner.Drain(untilMs, out);
    const std::size_t count = out.size() - before;
    if (count == 0 || !m_link.Connected()) {
        return more;
    }

    const FeedEvent* batch = out.data() + before;

    // A profile per actor, before the contacts that reference it. The testbench
    // needs the limb table to name a bone or resolve coverage, and a batch whose
    // profile arrives afterwards would replay its first frame as "unknown".
    //
    // `ragdoll_start` re-sends: the ragdoll is rebuilt on cell change, on 3D
    // reload, and repeatedly on a disturbed standing actor, and the limb table
    // is rebuilt with it.
    for (std::size_t i = 0; i < count; ++i) {
        const FeedEvent& event = batch[i];
        const bool restart =
            event.kind == EventKind::kState && std::strcmp(event.text, "ragdoll_start") == 0;
        if (restart) {
            m_link.ForgetProfile(event.actorId);
        }
        if (m_link.ProfileSent(event.actorId)) {
            continue;
        }
        const ActorProfile* profile = m_inner.Profile(event.actorId);
        if (profile == nullptr) {
            continue;
        }
        link::ProfileMessage message;
        message.profile = *profile;
        message.cell = m_link.Cell();
        message.formId = std::format("{:08X}", static_cast<std::uint32_t>(event.actorId));
        message.sessionMs = event.timeMs;
        m_link.PushProfile(message);
    }

    m_link.PushEvents(batch, count);
    return more;
}

}  // namespace rds::game
