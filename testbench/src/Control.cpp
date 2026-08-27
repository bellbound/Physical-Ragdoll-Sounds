// The control socket, and the App half that answers it.
//
// Both here because they are one subject - see Control.h for what it is for and
// why the two threads are split the way they are. The server is the top half of
// the file; everything below the second banner runs on the UI thread.

#include "Control.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <format>

#include <windows.h>

#include "App.h"

#include "rds/ConfigManager.h"
#include "rds/ConfigSchema.h"

namespace fs = std::filesystem;

namespace tb {
namespace {

/// How long the accept loop waits before going back round to check for a stop.
constexpr int kAcceptPollMs = 200;

/// A client that connects and then says nothing is a port scan, not a request.
constexpr int kRequestTimeoutMs = 3000;

/// How long the socket thread will wait for the UI thread to answer. Generous:
/// a patch re-runs the whole take through the engine and re-mixes it, and on a
/// thirty second take with a stack of cues that is tens of milliseconds - but
/// the frame it lands on may also be the one loading a video.
constexpr int kApplyTimeoutMs = 10000;

[[nodiscard]] std::string_view Trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r'))
        text.remove_suffix(1);
    return text;
}

[[nodiscard]] bool EqualsNoCase(std::string_view a, std::string_view b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) {
               return std::tolower(x) == std::tolower(y);
           });
}

[[nodiscard]] bool ContainsNoCase(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) return true;
    if (needle.size() > haystack.size()) return false;
    const auto at = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
                                [](unsigned char x, unsigned char y) {
                                    return std::tolower(x) == std::tolower(y);
                                });
    return at != haystack.end();
}

/// `key=value` lines into a request. The first `=` splits, so a `set` line
/// keeps its own `=`: `set=Slide:fSlideMinDurationMs=120`.
[[nodiscard]] ControlRequest ParseRequest(std::string_view text) {
    ControlRequest request;
    while (!text.empty()) {
        const auto eol = text.find('\n');
        const std::string_view line = Trim(text.substr(0, eol));
        text = eol == std::string_view::npos ? std::string_view{} : text.substr(eol + 1);
        if (line.empty() || line.front() == '#') continue;

        const auto equals = line.find('=');
        if (equals == std::string_view::npos) continue;
        const std::string_view key = Trim(line.substr(0, equals));
        const std::string_view value = Trim(line.substr(equals + 1));

        if (key == "op") request.op = value;
        else if (key == "set") request.sets.emplace_back(value);
        else if (key == "note") request.note = value;
        else if (key == "base") request.base = value;
        else if (key == "name") request.name = value;
        else if (key == "filter") request.filter = value;
        else if (key == "side") request.side = value == "B" || value == "b" || value == "1" ? 1 : 0;
        else if (key == "save") request.save = !(value == "0" || value == "false");
    }
    return request;
}

}  // namespace

// ═════════════════════════════════════════════════════════════════════════════
// the server
// ═════════════════════════════════════════════════════════════════════════════

ControlServer::~ControlServer() { Stop(); }

bool ControlServer::Start(std::uint16_t port) {
    Stop();
    m_port = port;
    m_stop.store(false, std::memory_order_relaxed);

    // Opened here rather than on the thread, for the same reason GameLink does
    // it: a port clash is an answer this call can give.
    rds::link::Socket listener;
    std::string error;
    if (!listener.Listen(port, error)) {
        {
            std::lock_guard lock{m_errorMutex};
            m_error = error;
        }
        spdlog::warn("control: {}", error);
        return false;
    }
    {
        std::lock_guard lock{m_errorMutex};
        m_error.clear();
    }
    m_listening.store(true, std::memory_order_relaxed);
    m_thread = std::thread([this, listener = std::move(listener)]() mutable {
        Run(std::move(listener));
    });
    spdlog::info("control: listening on 127.0.0.1:{}", port);
    return true;
}

void ControlServer::Stop() {
    m_stop.store(true, std::memory_order_relaxed);
    // Anything blocked waiting for the UI thread has to be let go before the
    // join, or shutdown waits out the full apply timeout for a frame that is
    // never going to be drawn.
    m_jobCv.notify_all();
    if (m_thread.joinable()) m_thread.join();
    m_listening.store(false, std::memory_order_relaxed);
    {
        std::lock_guard lock{m_jobMutex};
        m_pending.clear();
        m_replies.clear();
    }
}

std::string ControlServer::Error() const {
    std::lock_guard lock{m_errorMutex};
    return m_error;
}

void ControlServer::Run(rds::link::Socket listener) {
    while (!m_stop.load(std::memory_order_relaxed)) {
        rds::link::Socket client = listener.Accept(kAcceptPollMs);
        if (!client.Valid()) continue;
        Serve(client);
    }
}

void ControlServer::Serve(rds::link::Socket& client) {
    rds::link::Msg type{};
    std::vector<std::byte> payload;
    if (rds::link::ReadMessage(client, type, payload, kRequestTimeoutMs) != 1) return;
    if (type != kControlRequest) {
        const std::string reply = "ok=0\nerror=not a control request\n";
        (void)rds::link::WriteMessage(client, kControlReply, reply);
        return;
    }

    ControlJob job;
    job.request = ParseRequest(
        std::string_view(reinterpret_cast<const char*>(payload.data()), payload.size()));

    std::uint64_t id = 0;
    {
        std::lock_guard lock{m_jobMutex};
        id = m_nextId++;
        job.id = id;
        m_pending.push_back(std::move(job));
    }

    std::string reply;
    {
        std::unique_lock lock{m_jobMutex};
        const bool answered = m_jobCv.wait_for(
            lock, std::chrono::milliseconds(kApplyTimeoutMs), [&] {
                return m_replies.contains(id) || m_stop.load(std::memory_order_relaxed);
            });
        if (const auto at = m_replies.find(id); at != m_replies.end()) {
            reply = std::move(at->second);
            m_replies.erase(at);
        } else {
            // Drop the job as well: a request the CLI has stopped waiting for
            // must not land on the config half a minute later.
            std::erase_if(m_pending, [id](const ControlJob& queued) { return queued.id == id; });
            reply = answered ? "ok=0\nerror=the testbench is shutting down\n"
                             : std::format("ok=0\nerror=the testbench did not answer inside {} ms - "
                                           "is its window drawing?\n",
                                           kApplyTimeoutMs);
        }
    }
    m_served.fetch_add(1, std::memory_order_relaxed);
    (void)rds::link::WriteMessage(client, kControlReply, reply);
}

bool ControlServer::Poll(ControlJob& job) {
    std::lock_guard lock{m_jobMutex};
    if (m_pending.empty()) return false;
    job = std::move(m_pending.front());
    m_pending.pop_front();
    return true;
}

void ControlServer::Complete(std::uint64_t id, std::string reply) {
    {
        std::lock_guard lock{m_jobMutex};
        m_replies.emplace(id, std::move(reply));
    }
    m_jobCv.notify_all();
}

std::uint64_t FileCreatedTicks(const fs::path& file) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (::GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &data) != 0) {
        ULARGE_INTEGER ticks;
        ticks.LowPart = data.ftCreationTime.dwLowDateTime;
        ticks.HighPart = data.ftCreationTime.dwHighDateTime;
        if (ticks.QuadPart != 0) return ticks.QuadPart;
    }
    std::error_code ec;
    const auto written = fs::last_write_time(file, ec);
    if (ec) return 0;
    return static_cast<std::uint64_t>(written.time_since_epoch().count());
}

// ═════════════════════════════════════════════════════════════════════════════
// the UI thread's half
// ═════════════════════════════════════════════════════════════════════════════

namespace {

/// Find a parameter by qualified key. Case insensitive, and a bare key matches
/// when exactly one section has it - `fRateCapMs` is unambiguous and typing
/// `Arbitration:` in front of it from memory is how a script gets a name wrong.
///
/// Also answers to the names a parameter used to have, because the schema
/// carries them for the ini reader and a script written last week is in exactly
/// the same position as a config file written last week.
[[nodiscard]] const rds::ParamDesc* FindParam(std::string_view qualified) {
    const auto params = rds::AlgorithmParams();
    for (const rds::ParamDesc& p : params) {
        if (EqualsNoCase(rds::QualifiedKey(p), qualified)) return &p;
    }
    for (const rds::ParamDesc& p : params) {
        if (!p.legacySection.empty() &&
            EqualsNoCase(std::format("{}:{}", p.legacySection, p.legacyKey), qualified)) {
            return &p;
        }
        if (!p.legacySection2.empty() &&
            EqualsNoCase(std::format("{}:{}", p.legacySection2, p.legacyKey2), qualified)) {
            return &p;
        }
    }
    if (qualified.find(':') == std::string_view::npos) {
        const rds::ParamDesc* only = nullptr;
        for (const rds::ParamDesc& p : params) {
            if (EqualsNoCase(p.key, qualified)) {
                if (only != nullptr) return nullptr;  // two sections have it; make them say which
                only = &p;
            }
        }
        return only;
    }
    return nullptr;
}

/// Up to five parameters whose key looks like what was asked for. A name that
/// is nearly right is the common failure, and a reply that only says "no such
/// parameter" makes the next attempt a guess as well.
[[nodiscard]] std::vector<std::string> NearMisses(std::string_view qualified) {
    std::string_view bare = qualified;
    if (const auto colon = bare.rfind(':'); colon != std::string_view::npos) {
        bare = bare.substr(colon + 1);
    }
    std::vector<std::string> out;
    if (bare.size() < 3) return out;
    for (const rds::ParamDesc& p : rds::AlgorithmParams()) {
        if (ContainsNoCase(p.key, bare) || ContainsNoCase(bare, p.key)) {
            out.push_back(rds::QualifiedKey(p));
            if (out.size() == 5) break;
        }
    }
    return out;
}

/// Parse a value the way the ini reader would, but say so when it could not.
///
/// ParseParam answers with the default for text that does not parse, which is
/// right for a hand-mangled file and wrong here: a script that typos a number
/// would silently get the default and be told the patch worked.
[[nodiscard]] bool ParseValue(const rds::ParamDesc& p, std::string_view text, double& out) {
    text = Trim(text);
    if (text.empty()) return false;
    if (p.type == rds::ParamType::kEnum) {
        for (std::size_t i = 0; i < p.enumNames.size(); ++i) {
            if (EqualsNoCase(text, p.enumNames[i])) {
                out = static_cast<double>(i);
                return true;
            }
        }
    }
    if (p.type == rds::ParamType::kBool) {
        if (EqualsNoCase(text, "true") || EqualsNoCase(text, "yes") || EqualsNoCase(text, "on")) {
            out = 1.0;
            return true;
        }
        if (EqualsNoCase(text, "false") || EqualsNoCase(text, "no") || EqualsNoCase(text, "off")) {
            out = 0.0;
            return true;
        }
    }
    // A leading '+' is not something from_chars accepts and is something a
    // person writes, so it is stepped over rather than refused.
    if (text.front() == '+') text.remove_prefix(1);
    double parsed{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) return false;
    out = parsed;
    return true;
}

/// Everything a stem cannot carry into a filename, flattened. A note is free
/// text and a name is a path.
[[nodiscard]] std::string SanitiseStem(std::string_view name) {
    std::string out;
    for (const char c : name) {
        const unsigned char u = static_cast<unsigned char>(c);
        out += (std::isalnum(u) != 0 || c == '_' || c == '-' || c == '.') ? c : '_';
    }
    while (!out.empty() && out.back() == '.') out.pop_back();
    return out;
}

/// One line, no semicolons: it is going into an ini comment.
[[nodiscard]] std::string SanitiseNote(std::string_view note) {
    std::string out;
    for (const char c : note) out += (c == '\n' || c == '\r' || c == ';') ? ' ' : c;
    return std::string(Trim(out));
}

}  // namespace

void App::StartControl() {
    // After StartLink, which is what read the port out of RagdollSounds.ini.
    const auto devbench = static_cast<std::uint16_t>(
        m_general.devbench.port > 0 ? m_general.devbench.port : rds::link::kDefaultPort);
    m_control.Start(ControlPortFor(devbench));
}

void App::PumpControl() {
    // One request per frame. A batch of them from one script arrives on
    // consecutive frames, which at sixty hertz is sixteen milliseconds apart and
    // keeps every patch a step the user can see happen and undo.
    ControlJob job;
    if (!m_control.Poll(job)) return;
    std::string reply = ApplyControl(job.request);
    m_control.Complete(job.id, std::move(reply));
}

const std::filesystem::path* App::FindConfigFile(std::string_view name) const {
    if (name.empty()) return nullptr;
    for (const fs::path& file : m_configFiles) {
        if (EqualsNoCase(file.stem().string(), name)) return &file;
    }
    // A substring, but only when it picks out one file. `config_24_08` names a
    // day, not a config, and loading the first of them would be an answer to a
    // question nobody asked.
    const fs::path* only = nullptr;
    for (const fs::path& file : m_configFiles) {
        if (ContainsNoCase(file.stem().string(), name)) {
            if (only != nullptr) return nullptr;
            only = &file;
        }
    }
    return only;
}

std::string App::ApplyControl(const ControlRequest& request) {
    std::string out;
    const auto line = [&out](std::string_view text) {
        out += text;
        out += '\n';
    };
    const auto fail = [&](std::string_view why) {
        return std::format("ok=0\nerror={}\n", why);
    };

    const int side = request.side < 0 ? m_focusSide : std::clamp(request.side, 0, 1);
    ConfigSide& s = m_side[side];

    // ── status ───────────────────────────────────────────────────────────────
    if (request.op.empty() || request.op == "status" || request.op == "ping") {
        const LinkSnapshot link = m_link.Snapshot();
        line("ok=1");
        line(std::format("side={}", side == 0 ? "A" : "B"));
        line(std::format("split={}", m_split ? 1 : 0));
        line(std::format("config={}", s.name));
        line(std::format("unsaved={}", s.unsaved ? 1 : 0));
        line(std::format("configs={}", m_configFiles.size()));
        line(std::format("take={}",
                         m_take >= 0 ? m_takes[static_cast<std::size_t>(m_take)].stem : ""));
        line(std::format("game={}", link.connected  ? "connected"
                                    : link.listening ? "waiting"
                                                     : "down"));
        line(std::format("push={}", m_pushToGame ? 1 : 0));
        if (!s.patchNote.empty()) line(std::format("note={}", s.patchNote));
        return out;
    }

    // ── list ─────────────────────────────────────────────────────────────────
    if (request.op == "list") {
        line("ok=1");
        line(std::format("selected={}", s.name));
        // In the order the picker draws them, which is newest first.
        for (const fs::path& file : m_configFiles) {
            line(std::format("config={}\t{}", file.stem().string(), FileCreatedTicks(file)));
        }
        return out;
    }

    // ── get ──────────────────────────────────────────────────────────────────
    if (request.op == "get") {
        line("ok=1");
        line(std::format("config={}", s.name));
        for (const rds::ParamDesc& p : rds::AlgorithmParams()) {
            const std::string key = rds::QualifiedKey(p);
            if (!request.filter.empty() && !ContainsNoCase(key, request.filter) &&
                !ContainsNoCase(p.group, request.filter)) {
                continue;
            }
            if (p.type == rds::ParamType::kString) {
                line(std::format("param={}={}", key, rds::GetParamString(&s.cfg, p)));
                continue;
            }
            const double value = rds::GetParam(&s.cfg, p);
            line(std::format("param={}={}\tdefault={}\trange={}..{}\t{}", key,
                             rds::FormatParam(p, value), rds::FormatParam(p, p.defaultValue),
                             rds::FormatParam(p, p.minValue), rds::FormatParam(p, p.maxValue),
                             p.label));
        }
        return out;
    }

    // ── load ─────────────────────────────────────────────────────────────────
    if (request.op == "load") {
        const fs::path* file = FindConfigFile(request.name.empty() ? request.base : request.name);
        if (file == nullptr) {
            return fail(std::format("no config named '{}' - one name, or a substring that picks "
                                    "out exactly one file",
                                    request.name.empty() ? request.base : request.name));
        }
        const fs::path chosen = *file;  // LoadConfigFile does not rescan, but be safe about it
        m_focusSide = side;
        LoadConfigFile(side, chosen);
        for (std::size_t i = 0; i < m_configFiles.size(); ++i) {
            if (m_configFiles[i] == chosen) m_configIndex = static_cast<int>(i);
        }
        line("ok=1");
        line(std::format("name={}", s.name));
        line(std::format("file={}", chosen.string()));
        spdlog::info("control: loaded {} onto side {}", s.name, side == 0 ? "A" : "B");
        return out;
    }

    // ── set ──────────────────────────────────────────────────────────────────
    if (request.op != "set") {
        return fail(std::format("unknown op '{}' - status, list, get, set, load", request.op));
    }
    if (request.sets.empty()) {
        return fail("nothing to set");
    }

    // The base first, so a patch against a named config is that config's values
    // and not this side's.
    rds::ConfigSet patched = s.cfg;
    std::string family = s.name;
    if (!request.base.empty()) {
        const fs::path* file = FindConfigFile(request.base);
        if (file == nullptr) {
            return fail(std::format("no config named '{}'", request.base));
        }
        patched = rds::ConfigSet{};
        App::LoadAlgorithmFile(*file, patched);
        family = file->stem().string();
    }
    patched.Base().slots.rngSeed = m_seed;

    // Every assignment is checked before any of them lands. Half a patch is a
    // config nobody asked for, and the file it would be saved under would carry
    // a name that describes the whole of it.
    struct Assignment {
        const rds::ParamDesc* param{};
        double value{};
        bool isString{};
        std::string text;
    };
    std::vector<Assignment> assignments;
    assignments.reserve(request.sets.size());
    for (const std::string& text : request.sets) {
        const auto equals = text.find('=');
        if (equals == std::string::npos) {
            return fail(std::format("'{}' is not Section:Key=value", text));
        }
        const std::string_view key = Trim(std::string_view(text).substr(0, equals));
        const std::string_view value = Trim(std::string_view(text).substr(equals + 1));
        const rds::ParamDesc* param = FindParam(key);
        if (param == nullptr) {
            std::string message = std::format("no such parameter: {}", key);
            const std::vector<std::string> candidates = NearMisses(key);
            if (!candidates.empty()) {
                message += " - did you mean";
                for (const std::string& candidate : candidates) message += " " + candidate;
            }
            return fail(message);
        }
        Assignment assignment;
        assignment.param = param;
        if (param->type == rds::ParamType::kString) {
            assignment.isString = true;
            assignment.text = value;
        } else if (!ParseValue(*param, value, assignment.value)) {
            return fail(std::format("{} is not a value {} takes", value, rds::QualifiedKey(*param)));
        }
        assignments.push_back(std::move(assignment));
    }

    std::vector<std::string> applied;
    for (const Assignment& assignment : assignments) {
        const rds::ParamDesc& p = *assignment.param;
        // Setting anything in a surface's block opens that surface. Writing to a
        // closed block would land in memory the next Resolve overwrites, so the
        // patch would report success and then evaporate - and `set
        // Surface.ice:fTrimDb=-3` plainly means "ice has its own trim now".
        if (const rds::SurfaceClass surface = rds::SurfaceClassOfParam(p);
            surface != rds::SurfaceClass::kCount) {
            patched.Base().surfaces.opened[static_cast<std::size_t>(surface)] = true;
        }
        if (assignment.isString) {
            const std::string before(rds::GetParamString(&patched, p));
            rds::SetParamString(&patched, p, assignment.text);
            applied.push_back(std::format("{}: {} -> {}", rds::QualifiedKey(p), before,
                                          rds::GetParamString(&patched, p)));
            continue;
        }
        const double before = rds::GetParam(&patched, p);
        rds::SetParam(&patched, p, assignment.value);
        const double after = rds::GetParam(&patched, p);
        std::string note;
        // Say so when the schema moved the number. A silently clamped value is
        // a tuning session spent wondering why the last change did nothing.
        if (rds::FormatParam(p, after) != rds::FormatParam(p, assignment.value)) {
            note = std::format(" (asked for {}, {} allows {}..{})",
                               rds::FormatParam(p, assignment.value), rds::QualifiedKey(p),
                               rds::FormatParam(p, p.minValue), rds::FormatParam(p, p.maxValue));
        }
        applied.push_back(std::format("{}: {} -> {}{}", rds::QualifiedKey(p),
                                      rds::FormatParam(p, before), rds::FormatParam(p, after),
                                      note));
    }

    // On the undo stack like any other edit, so Ctrl+Z takes back what a script
    // did exactly the way it takes back a slider. That is the whole safety net
    // here: nothing a command line can do to this program is one keypress away
    // from being undone.
    // After every assignment, so a patch that tunes an opened parent takes the
    // classes following it along in the same step.
    patched.Base().surfaces.Resolve();

    const rds::ConfigSet previous = s.cfg;
    s.cfg = patched;
    m_focusSide = side;
    PushEdit(side, previous, request.note.empty() ? std::string("patch")
                                                  : "patch: " + SanitiseNote(request.note));
    s.patchNote = SanitiseNote(request.note);
    s.dirty = true;
    Rerun(side);

    line("ok=1");
    for (const std::string& text : applied) line("applied=" + text);

    if (!request.save) {
        line(std::format("name={}", s.name));
        line("saved=0");
        spdlog::info("control: patched side {} in place, {} parameter(s), not saved",
                     side == 0 ? "A" : "B", applied.size());
        return out;
    }

    // ── the new save ─────────────────────────────────────────────────────────
    //
    // Always a new file, never over the one it came from: a script that
    // overwrote the config the user is listening to would destroy the one thing
    // in this program that is not reproducible.
    std::string name = SanitiseStem(request.name);
    if (name.empty()) {
        // The next number in the family the patch was built on, so the lineage
        // reads off the names: config_24_08_7 -> config_24_08_8.
        name = FindConfigFile(family) != nullptr ? NextIteration(family) : NextConfigName();
    } else if (fs::exists(m_paths.configs / (name + ".ini"))) {
        name = NextIteration(name);
    }

    const fs::path file = m_paths.configs / (name + ".ini");
    std::string header = "RagdollSounds_Algorithm.ini - written by the testbench";
    if (!family.empty() && family != "(unsaved)") header += std::format(", patched from {}", family);
    if (!s.patchNote.empty()) header += std::format(" - {}", s.patchNote);
    if (!rds::ConfigManager::SaveFrom(file, &s.cfg, App::AlgorithmAndOpenedSurfaces(s.cfg),
                                      header)) {
        return fail(std::format("could not write {}", file.string()));
    }

    s.name = name;
    std::snprintf(s.saveName, sizeof(s.saveName), "%s", name.c_str());
    s.unsaved = false;
    // The rescan is what puts it in the picker, and it lands at the top of it:
    // the list is newest-created first, and nothing here is newer.
    ScanConfigs();
    for (std::size_t i = 0; i < m_configFiles.size(); ++i) {
        if (m_configFiles[i] == file) m_configIndex = static_cast<int>(i);
    }

    line(std::format("name={}", name));
    line(std::format("file={}", file.string()));
    line("saved=1");
    line(std::format("side={}", side == 0 ? "A" : "B"));
    spdlog::info("control: {} written and selected on side {} ({} parameter(s)){}", name,
                 side == 0 ? "A" : "B", applied.size(),
                 s.patchNote.empty() ? std::string{} : " - " + s.patchNote);
    return out;
}

}  // namespace tb
