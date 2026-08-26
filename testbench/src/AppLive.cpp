// The devbench half of the testbench: the link to a running game, OBS, making a
// take out of the two, and the windows that manage the takes afterwards.
//
// Split out of App.cpp because it is one subject with one shape - something
// happens outside this process, and this file is where it is turned into a file
// on disk and a row on screen - and because App.cpp was already the longest file
// here.
//
// ── what the link is for ─────────────────────────────────────────────────────
//
// The testbench has always been able to answer "what would this config have done
// to that take". The link answers the two questions it could not:
//
//   *Is that what it actually sounds like?* The sliders push themselves at the
//   running game, so a change is heard in the headset on the next knockdown
//   rather than after a save, a relaunch and a walk back to the guard.
//
//   *Where do takes come from?* They used to come from QuickModMenuNG's recorder,
//   which meant reaching for a menu inside the headset. Now the game streams
//   every contact out and this program writes the same two files, with OBS
//   driven alongside so the video arrives with it.

#include "App.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>

#include "imgui.h"

#include "rds/ConfigManager.h"
#include "rds/ConfigSchema.h"

namespace fs = std::filesystem;

namespace tb {
namespace {

/// How long to give OBS to actually start recording before taking the take
/// without video. Generous: OBS opens a file, an encoder and sometimes a
/// hardware session, and a take that is only data is still a take.
constexpr std::uint32_t kObsArmTimeoutMs = 4000;

/// How often the take pairs OBS's output clock with the game's.
///
/// QuickModMenuNG sampled every ten seconds, because it was recording sessions
/// minutes long inside a headset. A devbench take is usually five to thirty
/// seconds, and a fit through two rows is a line through two points - so this is
/// a second, which costs one websocket round trip and gives the slope something
/// to be fitted through.
constexpr double kClockSampleMs = 1000.0;

void Tip(std::string_view text) {
    if (!text.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(420.0f);
        ImGui::TextUnformatted(text.data(), text.data() + text.size());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

/// A filled dot, for a state that is either on or off and wants to be readable
/// out of the corner of an eye.
void Dot(ImU32 colour) {
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float radius = ImGui::GetTextLineHeight() * 0.28f;
    ImGui::GetWindowDrawList()->AddCircleFilled(
        ImVec2(at.x + radius + 2.0f, at.y + ImGui::GetTextLineHeight() * 0.5f), radius, colour);
    ImGui::Dummy(ImVec2(radius * 2.0f + 6.0f, ImGui::GetTextLineHeight()));
}

}  // namespace

// ═════════════════════════════════════════════════════════════════════════════
// bringing the link up
// ═════════════════════════════════════════════════════════════════════════════

void App::StartLink() {
    // The port and the OBS path come out of the deployed mod's own
    // RagdollSounds.ini rather than out of a second file of our own. They are
    // properties of the machine the game runs on - which port is free, where OBS
    // was installed - and a testbench with its own copy of them is a testbench
    // that connects to a port the game is not using.
    m_generalIni = m_paths.generalIni;
    if (m_generalIni.empty() && !m_paths.sfxIni.empty()) {
        m_generalIni = m_paths.sfxIni.parent_path() / "RagdollSounds.ini";
    }

    m_general = rds::GeneralConfig{};
    const std::size_t keys =
        rds::ConfigManager::LoadInto(m_generalIni, &m_general, rds::GeneralParams());
    if (keys == 0) {
        spdlog::warn("link: no {} - falling back to the defaults, port {}", m_generalIni.string(),
                     m_general.devbench.port);
    }

    obs::Config obsConfig;
    obsConfig.enabled = true;
    obsConfig.obsExe = m_general.devbench.obsPath;
    obs::Configure(obsConfig);
    obs::Connect();

    const auto port = static_cast<std::uint16_t>(
        m_general.devbench.port > 0 ? m_general.devbench.port : rds::link::kDefaultPort);
    m_link.Start(port);

    if (!m_general.devbench.enabled) {
        spdlog::warn("link: [Devbench] bEnableDevbench is 0 in {} - this end is listening, but the "
                     "game will not look for it",
                     m_generalIni.string());
    }
}

void App::Shutdown() {
    if (m_recordState != RecordState::kIdle) {
        m_link.CancelCapture();
        obs::StopTake([](std::string) {});
        m_recordState = RecordState::kIdle;
    }
    m_link.Stop();
    m_control.Stop();
    obs::Disconnect();
}

VideoTake::Mode App::VideoMode() const {
    return m_videoSync ? VideoTake::Mode::kDirect : VideoTake::Mode::kFrameCache;
}

// ═════════════════════════════════════════════════════════════════════════════
// pushing config at the game
// ═════════════════════════════════════════════════════════════════════════════

void App::SyncToGame() {
    // Not config, but it belongs on the same once-a-frame call: a take that is
    // running needs its clocks paired whether or not anything on the sliders
    // moved, and this is the frame tick the devbench half already has.
    SampleTakeClock(false);

    const bool connected = m_link.Connected();
    if (connected != m_wasConnected) {
        // A fresh connection knows nothing about what is on the sliders, so
        // everything goes out again. This is what "the mod pulls its config from
        // the devbench" is, seen from this end: the game says hello and the
        // three things it needs arrive unasked.
        m_pushedValid = false;
        m_pushedAudioMode = false;
        m_wasConnected = connected;
        if (!connected) {
            return;
        }
    }
    if (!connected) {
        return;
    }

    // Before the push gate below, and not subject to it: which mix the game is
    // playing is not one of the overrides. Turning the sliders loose so the game
    // runs its own inis is exactly when the comparison is worth making, and the
    // switch would be dead in the one mode it matters most.
    //
    // Sent once on connect as well as on every change, because a game that has
    // just started knows nothing about a switch that was already flipped.
    if (!m_pushedAudioMode || m_pushedVanillaAudio != m_useVanillaAudio) {
        m_link.PushAudioMode(m_useVanillaAudio);
        m_pushedVanillaAudio = m_useVanillaAudio;
        m_pushedAudioMode = true;
    }

    if (!m_pushToGame) {
        return;
    }

    const std::string library = m_paths.library.string();
    if (!m_pushedValid || m_pushedLibrary != library) {
        m_link.PushLibraryPath(library);
        m_pushedLibrary = library;
    }

    const rds::AlgorithmConfig& config = m_side[m_focusSide].cfg;
    if (!m_pushedValid || !SameConfig(m_pushedConfig, config)) {
        m_link.PushAlgorithm(config);
        m_pushedConfig = config;
    }

    if (!m_pushedValid || !(m_pushedSfx == m_sfx)) {
        m_link.PushSfx(m_sfx);
        m_pushedSfx = m_sfx;
    }
    m_pushedValid = true;
}

// ═════════════════════════════════════════════════════════════════════════════
// the connection row
// ═════════════════════════════════════════════════════════════════════════════

void App::DrawLinkRow() {
    const LinkSnapshot link = m_link.Snapshot();
    const obs::Status video = obs::Now();

    std::uint64_t noteSeq = 0;
    if (std::string note = obs::LastNote(noteSeq); noteSeq != m_obsNoteSeq) {
        m_obsNoteSeq = noteSeq;
        m_obsNote = std::move(note);
    }

    // Laid out from the right edge, so it keeps the same place as the numbers to
    // its left grow and shrink. The width is the widest this row ever gets -
    // connected, with the audio switch, a clock and two counters - rather than
    // measured per frame, because a row that moves as it counts is unreadable.
    constexpr float kWidth = 580.0f;
    const float rightEdge = ImGui::GetWindowWidth() - 8.0f;
    if (ImGui::GetCursorPosX() < rightEdge - kWidth) {
        ImGui::SetCursorPosX(rightEdge - kWidth);
    }

    // The A/B switch, in this row rather than buried in Options because it is
    // clicked between one shove and the next: a comparison is only worth anything
    // while the same body is falling down the same stairs.
    //
    // Outside the connected branch, because it no longer needs a game. With one
    // attached it swaps the mix in Skyrim; with a take open it swaps that take's
    // playback to the vanilla track the take recorded. Usually both at once,
    // which is the point of it.
    {
        const bool haveTrack = m_recording != nullptr && !m_recording->VanillaTrack().empty();
        if (m_useVanillaAudio) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.78f, 0.35f, 1.0f));
        }
        ImGui::Checkbox("Use Vanilla Audio", &m_useVanillaAudio);
        if (m_useVanillaAudio) {
            ImGui::PopStyleColor();
        }

        // What it will do to *this* take, which is the half a tooltip about the
        // game cannot answer. "Silent because there is no track" and "silent
        // because the library is not set" sound identical and are not the same
        // problem, so both are named.
        std::string take;
        if (m_recording == nullptr) {
            take = "\n\nNo take is open, so this only affects the game.";
        } else if (!haveTrack) {
            take =
                "\n\nThis take has no vanilla track, so its playback is unchanged. Takes\n"
                "recorded before the hook existed have none, and neither does one recorded\n"
                "on a runtime where the hook could not be installed.";
        } else if (m_useVanillaAudio) {
            take = std::format(
                "\n\nThis take: {} row(s) played, {} with no file found.{}", m_vanillaPlayed,
                m_vanillaMisses,
                m_vanillaLibrary.FileCount() == 0
                    ? "\nThe vanilla sound library is empty - point --vanilla-sounds at an\n"
                      "extract of the game's sound/fx tree."
                    : "");
        } else {
            take = std::format("\n\nThis take has a vanilla track: {} row(s).",
                               m_recording->VanillaTrack().size());
        }

        Tip(std::string(
                "Listen to what this mod replaces, for as long as this is ticked.\n\n"
                "With a game connected: it stops dropping vanilla's body impacts and stops\n"
                "playing ours - every cue is still made and still recorded, it just never\n"
                "becomes a voice. So what you hear is a knockdown with the mod not installed.\n\n"
                "With a take open: the take plays its own vanilla track instead of our mix -\n"
                "the sounds Skyrim actually chose while that take was recorded, from the wavs\n"
                "in the vanilla sound library. Which variant of a descriptor was drawn is not\n"
                "knowable and is picked from the seed; the per-play dB roll is left flat.\n"
                "<take>_vanilla.csv says the same, at more length.") +
            take);
        ImGui::SameLine();
    }

    ImGui::BeginGroup();

    if (link.connected) {
        Dot(IM_COL32(110, 220, 130, 255));
    } else if (link.listening) {
        Dot(IM_COL32(150, 150, 90, 200));
    } else {
        Dot(IM_COL32(200, 80, 80, 220));
    }
    ImGui::SameLine();

    if (!link.listening) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "link down");
        Tip(link.error.empty() ? "The listener never opened." : link.error);
    } else if (!link.connected) {
        ImGui::TextDisabled("waiting for the game");
        Tip(std::format(
            "Listening on 127.0.0.1:{}.\n"
            "The game connects by itself once it is running with [Devbench] bEnableDevbench = 1\n"
            "in {}. It retries every couple of seconds for the whole session, so the testbench\n"
            "can be started at any point and will still be found.",
            m_general.devbench.port, m_generalIni.filename().string()));
    } else {
        ImGui::TextColored(ImVec4(0.55f, 0.9f, 0.6f, 1.0f), "game %u", link.peerPid);
        Tip(std::format("Connected to a RagdollSounds.dll built {}.\n"
                        "{} event(s) in, {} of them contacts.\n"
                        "The game reports {} tracked actor(s), {} cue(s) and {} engine voice(s)"
                        "{}\n"
                        "Overrides in force: {}{}{}",
                        link.peerBuild.empty() ? "?" : link.peerBuild, link.eventsReceived,
                        link.contactsReceived, link.game.trackedActors, link.game.cuesEmitted,
                        link.game.voicesOut,
                        link.game.droppedContacts != 0
                            ? std::format(", and {} contact(s) its ring had to throw away.",
                                          link.game.droppedContacts)
                            : std::string("."),
                        (link.game.overrideFlags & 1u) != 0 ? "algorithm " : "",
                        (link.game.overrideFlags & 2u) != 0 ? "sfx " : "",
                        (link.game.overrideFlags & 4u) != 0 ? "library" : ""));

        if (!link.acceptsEvents) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "(stream refused)");
            Tip("The game's FeedEvent layout is not ours, so its contact stream is being\n"
                "ignored rather than reinterpreted. Config still gets through. Rebuild both\n"
                "halves from the same tree.");
        }

        ImGui::SameLine();
        switch (m_recordState) {
            case RecordState::kIdle: {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.42f, 0.12f, 0.14f, 1.0f));
                const bool go = ImGui::Button("Record");
                ImGui::PopStyleColor();
                if (go) StartLiveRecording();
                Tip(video.connected
                        ? "Start a take. OBS records the picture, this end records the events,\n"
                          "and the two are written out together when you stop."
                        : "Start a take. OBS is not connected, so this will try to start it from\n"
                          "[Devbench] sObsPath and take the events either way - a take with no\n"
                          "video is still a take.");
                break;
            }
            case RecordState::kArming:
                ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.45f, 1.0f), "arming OBS...");
                break;
            case RecordState::kRecording: {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.16f, 0.18f, 1.0f));
                const bool stop = ImGui::Button("Stop");
                ImGui::PopStyleColor();
                if (stop) StopLiveRecording();
                ImGui::SameLine();
                const double seconds =
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - m_recordStarted)
                        .count();
                ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.55f, 1.0f), "%02d:%02d  %zu contacts%s",
                                   static_cast<int>(seconds) / 60, static_cast<int>(seconds) % 60,
                                   m_link.CapturedContacts(), m_recordHasVideo ? "  [video]" : "");
                Tip(m_recordHasVideo
                        ? "OBS is recording alongside. Stopping writes the take, moves the clip\n"
                          "beside it, and leaves it unbuilt until you generate its frames."
                        : "No video on this one - OBS never confirmed it was running. The events\n"
                          "are still being kept.");
                break;
            }
            case RecordState::kWriting:
                ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.45f, 1.0f), "writing...");
                break;
        }
    }

    ImGui::EndGroup();

    if (!m_recordError.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "record: %s", m_recordError.c_str());
    } else if (!m_obsNote.empty()) {
        ImGui::TextDisabled("obs: %s", m_obsNote.c_str());
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// recording a take out of the running game
// ═════════════════════════════════════════════════════════════════════════════

void App::StartLiveRecording() {
    m_recordError.clear();
    m_recordHasVideo = false;

    // The stem is decided now rather than at the end, because OBS is told to
    // name its own output after it and the two have to agree. Named after
    // nothing in particular until the take is written: the actor is not known
    // until the first profile arrives.
    m_recordStem = NextTakeStem(m_paths.recordings, "devbench_take");
    m_link.BeginCapture();
    m_recordStarted = std::chrono::steady_clock::now();
    m_recordSync.clear();

    const obs::Status video = obs::Now();
    if (!video.connected) {
        // Nothing is answering on the websocket. Try to start OBS, and take the
        // events regardless: a take that is only data is a perfectly good take,
        // and refusing to record because a video tool is not up would be the
        // tail wagging the dog.
        std::string error;
        if (!obs::Launch(error)) {
            spdlog::info("record: no video this time - {}", error);
        }
        obs::Connect();
        m_recordState = RecordState::kRecording;
        return;
    }

    m_recordState = RecordState::kArming;
    obs::SetTakeActive(true);
    obs::StartTake(m_recordStem, kObsArmTimeoutMs, [this](bool started) { OnVideoArmed(started); });
}

void App::OnVideoArmed(bool started) {
    // Runs on the UI thread, out of obs::Pump().
    if (m_recordState != RecordState::kArming) {
        return;  // stopped or cancelled while OBS was thinking about it
    }
    m_recordHasVideo = started;
    m_recordState = RecordState::kRecording;
    if (!started) {
        spdlog::info("record: OBS did not confirm it was running - taking the events only");
        return;
    }
    // The arm sample. OBS has just said the output is running, so this row is the
    // one nearest the head of the file and the one the intercept leans on most.
    SampleTakeClock(true);
}

void App::SampleTakeClock(bool force) {
    if (m_recordState != RecordState::kRecording || !m_recordHasVideo) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (!force &&
        std::chrono::duration<double, std::milli>(now - m_lastClockSample).count() <
            kClockSampleMs) {
        return;
    }
    // The game's clock at the moment the question is asked. Read here rather than
    // in the callback because the callback runs whenever the answer comes back,
    // and the round trip is the one interval this pairing has to account for.
    double gameMs = 0.0;
    if (!m_link.GameClock(gameMs)) {
        return;  // no heartbeat yet, so nothing to pair OBS against
    }
    m_lastClockSample = now;
    obs::SampleClock([this, gameMs](std::uint64_t obsMs, std::uint64_t rttMs) {
        // The midpoint of the send and the reply is the local instant the reply
        // describes - the estimator NTP uses, and the same one QuickModMenuNG's
        // recorder used from inside the game.
        m_recordSync.push_back({gameMs + static_cast<double>(rttMs) * 0.5,
                                static_cast<double>(obsMs), static_cast<double>(rttMs)});
    });
}

void App::StopLiveRecording() {
    if (m_recordState == RecordState::kIdle) {
        return;
    }
    // The closing row, before the state moves off kRecording and before the stop
    // is queued. Both requests go down the one socket in order, so this one is
    // answered - and pushed - before the stop is, and the fit gets a sample at
    // each end of the take rather than only at the head.
    SampleTakeClock(true);
    m_recordState = RecordState::kWriting;
    if (!m_recordHasVideo) {
        FinishLiveRecording({});
        return;
    }
    obs::StopTake([this](std::string outputPath) { FinishLiveRecording(outputPath); });
}

void App::FinishLiveRecording(const std::string& videoPath) {
    obs::SetTakeActive(false);
    const LiveCapture capture = m_link.EndCapture();
    m_recordState = RecordState::kIdle;
    // Cleared here rather than at the end, where it used to be: the end is past
    // the video move, and a clear there wiped the one message that says the clip
    // did not make it beside the take.
    m_recordError.clear();

    if (capture.Empty()) {
        m_recordError = "nothing arrived - was anybody knocked over?";
        // The clip is still on disk wherever OBS put it. Left alone rather than
        // deleted: it is the user's recording directory, and a tool that removes
        // files from it on a failed take is a tool nobody trusts with the
        // directory.
        return;
    }

    const rds::link::ProfileMessage* subject = capture.Subject();
    TakeSource source = SourceFromCapture(
        capture.events, subject,
        m_recordNote[0] != '\0' ? std::string(m_recordNote) : std::string("devbench live take"));

    // Renamed now that the actor is known. The stem OBS was given is kept as the
    // fallback, because the clip on disk already carries it.
    std::string stem = m_recordStem;
    if (!source.actorName.empty() && source.actorName != "unknown") {
        stem = NextTakeStem(m_paths.recordings, SafeStem(source.actorName) + "_devbench");
    }

    std::string error;
    TakeWindow window;
    const fs::path csv = WriteTake(m_paths.recordings, stem, source, capture.startMs,
                                   capture.endMs + 1.0, error, &window);
    if (csv.empty()) {
        m_recordError = error;
        return;
    }

    // The clip moves in beside the take and keeps its stem, which is the whole
    // convention Recording::Scan looks for. Moved rather than copied: OBS wrote
    // it into the user's recording folder and leaving a second copy there is how
    // a disk fills up without anybody deciding to.
    fs::path video;
    if (!videoPath.empty()) {
        std::error_code ec;
        const fs::path from{videoPath};
        const fs::path to = m_paths.recordings / (stem + from.extension().string());
        fs::rename(from, to, ec);
        if (ec) {
            // Across volumes, usually. The tidy-up afterwards is best effort on
            // its own error, because a copy that landed is a video beside the
            // take whether or not the original could be swept up.
            fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
            if (!ec) {
                std::error_code sweep;
                fs::remove(from, sweep);
            }
        }
        if (ec) {
            m_recordError = std::format("the take was written but the video stayed at {}",
                                        videoPath);
        } else {
            video = to;
        }
    }

    WriteTakeSync(stem, window, video);

    RescanKeepingSelection();
    // Straight to it. The reason to record is to hear it, and a take you have to
    // go and find in a combo box is one more step between the fall and the sound.
    for (int i = 0; i < static_cast<int>(m_takes.size()); ++i) {
        if (m_takes[static_cast<std::size_t>(i)].stem == stem) {
            SelectRecording(i);
            break;
        }
    }
    spdlog::info("record: {} written, {} event(s){}", stem, capture.events.size(),
                 videoPath.empty() ? " (no video)" : " with video");
}

void App::WriteTakeSync(const std::string& stem, const TakeWindow& window,
                        const std::filesystem::path& video) {
    if (video.empty()) {
        return;  // no video, so nothing to line anything up against
    }

    // Rebased onto the take's own zero, which is its first surviving row less the
    // lead-in - `window.originMs`, and not the moment Record was pressed. That is
    // the same clock t_ms is written on, so it is the only one the sync track may
    // be expressed in.
    ObsTakeInfo info;
    info.outputPath = video.generic_string();
    info.obsVersion = obs::Version();
    info.recordDirectory = obs::RecordDirectory();

    std::string error;
    if (WriteSyncTrack(m_paths.recordings, stem, m_recordSync, window.originMs, error)) {
        info.syncCsv = stem + "_sync.csv";
        const SyncModel fit = FitSync(m_paths.recordings / info.syncCsv);
        info.offsetMs = fit.valid ? fit.intercept : 0.0;
        spdlog::info("record: {} sync {}/{} rows, offset {:+.0f} ms, drift {:+.2f} ms/s", stem,
                     fit.rowsUsed, fit.rowsTotal, info.offsetMs, fit.driftMsPerSec);
    } else {
        // Worth saying out loud, but not worth failing the take over: the events
        // and the video are both on disk and the offset can still be nudged by
        // hand. The block below is written either way, because "OBS recorded this
        // take and nothing was cut off it" is true whether or not a clock was
        // ever sampled - and it is what keeps the two-second clip pad off it.
        spdlog::warn("record: {} has video but no sync track - {}", stem, error);
    }
    AppendObsBlock(m_paths.recordings / (stem + ".yaml"), info);
}

// ═════════════════════════════════════════════════════════════════════════════
// acting on a take
// ═════════════════════════════════════════════════════════════════════════════

void App::RescanKeepingSelection() {
    const std::string wanted =
        m_take >= 0 && m_take < static_cast<int>(m_takes.size())
            ? m_takes[static_cast<std::size_t>(m_take)].stem
            : std::string{};
    ScanRecordings();
    if (m_takes.empty()) {
        m_take = -1;
        m_recording.reset();
        return;
    }
    for (int i = 0; i < static_cast<int>(m_takes.size()); ++i) {
        if (m_takes[static_cast<std::size_t>(i)].stem == wanted) {
            SelectRecording(i);
            return;
        }
    }
    SelectRecording(std::clamp(m_take, 0, static_cast<int>(m_takes.size()) - 1));
}

void App::DeleteTakeAt(int index) {
    if (index < 0 || index >= static_cast<int>(m_takes.size())) {
        return;
    }
    const std::string stem = m_takes[static_cast<std::size_t>(index)].stem;
    const fs::path csv = m_takeCsv[static_cast<std::size_t>(index)];

    // Close the video first: the frame cache is about to be removed and the
    // decoder holds open handles into it.
    if (index == m_take) m_video.Close();

    std::string error;
    const std::size_t gone = DeleteTake(csv, m_paths.frameCache, error);
    m_takeFlags.Erase(stem);
    m_offsets.Erase(stem);
    m_manageNote = gone > 0 ? std::format("deleted {}", stem) : error;

    // Aim at the neighbour before the list is rebuilt, so deleting the take you
    // are listening to leaves you on the next one rather than back at the top.
    if (index == m_take) {
        m_take = std::max(0, index - 1);
        const std::string neighbour =
            m_take < static_cast<int>(m_takes.size()) && m_take != index
                ? m_takes[static_cast<std::size_t>(m_take)].stem
                : std::string{};
        ScanRecordings();
        int landOn = 0;
        for (int i = 0; i < static_cast<int>(m_takes.size()); ++i) {
            if (m_takes[static_cast<std::size_t>(i)].stem == neighbour) {
                landOn = i;
                break;
            }
        }
        if (m_takes.empty()) {
            m_take = -1;
            m_recording.reset();
        } else {
            SelectRecording(landOn);
        }
        return;
    }
    RescanKeepingSelection();
}

void App::BuildFrameCacheNow() {
    if (m_take < 0) {
        return;
    }
    std::string error;
    if (!m_video.BuildCacheAndDropVideo(m_paths.frameCache, error)) {
        m_videoNote = error;
        return;
    }
    m_videoNote = "frames built, video dropped";
    // The take no longer has an mp4, so its RecordingInfo is out of date.
    RescanKeepingSelection();
}

void App::DeleteSelection() {
    if (!m_hasSelection || m_take < 0) {
        return;
    }
    const fs::path csv = m_takeCsv[static_cast<std::size_t>(m_take)];
    std::size_t removed = 0;
    std::string error;
    if (!DeleteEventRange(csv, m_selStartMs, m_selEndMs, removed, error)) {
        m_selectionNote = error;
        return;
    }
    m_selectionNote = std::format("dropped {} row(s)", removed);
    m_hasSelection = false;
    RescanKeepingSelection();
}

void App::CreateRecordingFromSelection() {
    if (!m_hasSelection || m_take < 0 || !m_recording) {
        return;
    }
    const rds::RecordingInfo& info = m_takes[static_cast<std::size_t>(m_take)];
    const std::string stem =
        NextTakeStem(m_paths.recordings, SafeStem(info.stem) + "_cut");

    std::string error;
    TakeWindow window;
    const fs::path csv = SliceTake(*m_recording, info, m_paths.recordings, stem, m_selStartMs,
                                   m_selEndMs, error, &window);
    if (csv.empty()) {
        m_selectionNote = error;
        return;
    }

    // The matching stretch of video, cut and then decoded, so the new take is
    // usable rather than merely present. Two conversions, and both matter: the
    // *written take's* window rather than the selection, because SliceTake
    // re-bases onto the first row it kept and a selection dragged by eye starts
    // in dead air; then take time to video time, because the two clocks are a
    // fit apart. Cutting from the selection cost 623 ms on log_14_cut_1 - the
    // gap between where the drag started and where the ragdoll did - and the
    // picture ran half a second ahead of everything it was there to explain.
    std::string videoNote;
    if (!info.videoPath.empty()) {
        const fs::path clip = m_paths.recordings / (stem + ".mp4");
        const double zero = VideoTimeMs(window.originMs);
        const double from = std::max(0.0, zero - kLeadInMs);
        const double to = VideoTimeMs(window.originMs + window.durationMs) + kLeadInMs;
        std::string cutError;
        if (CutVideo(info.videoPath, clip, from, to, cutError)) {
            std::string buildError;
            if (m_videoSync) {
                // Video sync keeps the mp4 and reads frames out of it, so
                // building a cache here would be work the option exists to
                // avoid.
                videoNote = ", clip cut";
            } else if (BuildFrameCache(clip, m_paths.frameCache, stem, buildError)) {
                std::error_code ec;
                fs::remove(clip, ec);
                videoNote = ", frames built";
            } else {
                videoNote = ", clip cut but " + buildError;
            }
            // Where the take's zero actually fell inside the clip: the lead-in,
            // except at the very front of a recording where there was not a
            // lead-in's worth of video to take. Written down rather than left to
            // the guess, because the guess is about OBS's cut point and this cut
            // is ours.
            m_offsets.Set(stem, zero - from);
        } else {
            videoNote = ", no video (" + cutError + ")";
        }
    }

    m_selectionNote = "wrote " + stem + videoNote;
    m_hasSelection = false;
    ScanRecordings();
    for (int i = 0; i < static_cast<int>(m_takes.size()); ++i) {
        if (m_takes[static_cast<std::size_t>(i)].stem == stem) {
            SelectRecording(i);
            return;
        }
    }
}

void App::ExtractSelectionVideo() {
    if (!m_hasSelection || m_take < 0) {
        return;
    }
    const rds::RecordingInfo& info = m_takes[static_cast<std::size_t>(m_take)];
    if (info.videoPath.empty()) {
        m_selectionNote = "this take has no video";
        return;
    }
    // Named for the window it came from and put beside the recordings without a
    // csv, so Recording::Scan does not adopt it as a take.
    const fs::path clip =
        m_paths.recordings / std::format("{}_clip_{:.0f}-{:.0f}.mp4", info.stem, m_selStartMs,
                                         m_selEndMs);
    std::string error;
    if (!CutVideo(info.videoPath, clip, VideoTimeMs(m_selStartMs), VideoTimeMs(m_selEndMs),
                  error)) {
        m_selectionNote = error;
        return;
    }
    m_selectionNote = "wrote " + clip.filename().string();
}

void App::DeleteTakeVideo() {
    if (m_take < 0) {
        return;
    }
    const std::string stem = m_takes[static_cast<std::size_t>(m_take)].stem;
    const fs::path video = m_takes[static_cast<std::size_t>(m_take)].videoPath;

    m_video.Close();
    std::error_code ec;
    const bool gone = !video.empty() && fs::remove(video, ec);
    ClearFrameCache(m_paths.frameCache, stem);
    m_offsets.Erase(stem);

    m_selectionNote = gone ? "video and frames deleted" : "no video to delete";
    RescanKeepingSelection();
}

// ═════════════════════════════════════════════════════════════════════════════
// the options window
// ═════════════════════════════════════════════════════════════════════════════

void App::DrawOptions() {
    if (!m_showOptions) {
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(560.0f, 0.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Options", &m_showOptions, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    ImGui::SeparatorText("Video");

    const bool wasDirect = m_videoSync;
    ImGui::Checkbox("Enable Video Sync (Experimental)", &m_videoSync);
    Tip("Read frames straight out of the mp4 instead of decoding the whole clip to a\n"
        "frame cache first.\n\n"
        "The appeal is that nothing is pre-decoded: a take recorded thirty seconds ago\n"
        "is scrubbable at once, and the video stays the source of truth rather than a\n"
        "360p copy of it. The cost is a seek per frame, so scrubbing shows the last\n"
        "frame it managed until the new one lands.\n\n"
        "It never overrides a cache that already exists - there is nothing to be gained\n"
        "by seeking an mp4 when the frames are on disk - so this decides what happens to\n"
        "takes that have not been built yet, and stops anything building one by itself.");
    if (m_videoSync != wasDirect && m_take >= 0) {
        // Re-open the current take under the new rule, so the checkbox does
        // something visible rather than something that starts mattering next
        // time a take is loaded.
        const rds::RecordingInfo& info = m_takes[static_cast<std::size_t>(m_take)];
        m_video.Open(info.stem, info.videoPath, m_paths.frameCache, VideoMode());
    }

    ImGui::TextDisabled("Frame caches live in %s", m_paths.frameCache.string().c_str());
    if (ImGui::Button("Clear every frame cache")) {
        m_video.Close();
        const std::size_t gone = ClearAllFrameCaches(m_paths.frameCache);
        m_manageNote = std::format("cleared {} frame cache(s)", gone);
        RescanKeepingSelection();
    }
    Tip("Every take's decoded frames go. Takes that still have their mp4 rebuild on the\n"
        "next open; takes whose video was already dropped lose their picture for good.");
    if (!m_manageNote.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", m_manageNote.c_str());
    }

    ImGui::SeparatorText("The game");

    ImGui::Checkbox("Push config to the running game", &m_pushToGame);
    Tip("With this on, every slider move, every sfx assignment and the library path are\n"
        "sent to a connected game as they change, and the mod runs them instead of its\n"
        "own inis. Turn it off to hear the game on what its inis actually say - which is\n"
        "what a user would hear - without dropping the connection.");
    if (!m_pushToGame && m_link.Connected()) {
        ImGui::SameLine();
        if (ImGui::SmallButton("hand it back")) {
            m_link.PushClear();
            m_pushedValid = false;
        }
        Tip("Tell the mod to drop every override and go back to its own inis. Without this\n"
            "the last config pushed stays in force for the rest of the session.");
    }

    ImGui::TextDisabled("Settings are read from %s", m_generalIni.string().c_str());
    ImGui::TextDisabled("  [Devbench] iDevbenchPort = %d", m_general.devbench.port);
    ImGui::TextDisabled("  [Devbench] bEnableDevbench = %d", m_general.devbench.enabled ? 1 : 0);
    ImGui::TextDisabled("  [Devbench] sObsPath = %s",
                        m_general.devbench.obsPath[0] != '\0' ? m_general.devbench.obsPath
                                                              : "(unset)");
    // The control socket has no key of its own - it is the port next door - so
    // this row is the only place its number is written down.
    if (m_control.Listening()) {
        ImGui::TextDisabled("  control socket on 127.0.0.1:%u, %llu request(s) this session",
                            static_cast<unsigned>(m_control.Port()),
                            static_cast<unsigned long long>(m_control.Served()));
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.5f, 1.0f), "  control socket down");
    }
    Tip(m_control.Listening()
            ? "The command line's way into this session: tools/tune.py patches the config the\n"
              "focused side is playing, saves it as a new file and selects it, without a restart.\n"
              "The port is the devbench port plus one."
            : std::string("The control listener never opened, so tools/tune.py cannot reach this\n"
                          "session. Usually a second testbench already has the port.\n\n") +
                  m_control.Error());
    if (ImGui::Button("Reload that file")) {
        m_link.Stop();
        m_control.Stop();
        StartLink();
        StartControl();
    }
    Tip("Re-reads RagdollSounds.ini and re-opens the listener on whatever port it now\n"
        "names. For changing the port without restarting both halves.");

    ImGui::SeparatorText("Recording");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##note", "note for the next take - its intent, not a description",
                             m_recordNote, sizeof(m_recordNote));
    Tip("Goes into the sidecar as recording.note. The takes that turned out to be worth\n"
        "keeping are the ones where this said what was being tried.");

    ImGui::End();
}

// ═════════════════════════════════════════════════════════════════════════════
// the recording manager
// ═════════════════════════════════════════════════════════════════════════════

void App::DrawRecordingManager() {
    if (!m_showManager) {
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(880.0f, 560.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Recordings", &m_showManager, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    // What each take is made of, computed once for the whole window: three of
    // the buttons below select on it and the table shows it, and asking the file
    // system per row per frame would be a directory walk at 60 Hz.
    struct Row {
        bool hasVideo{};
        bool hasCache{};
        bool unbuilt{};  ///< video with no cache: nothing has lined the two up
        bool empty{};    ///< no video and no impact - nothing to look at either way
    };
    std::vector<Row> rows(m_takes.size());
    std::size_t unbuiltCount = 0;
    std::size_t emptyCount = 0;
    for (std::size_t i = 0; i < m_takes.size(); ++i) {
        Row& row = rows[i];
        row.hasVideo = !m_takes[i].videoPath.empty();
        row.hasCache = HasFrameCache(m_paths.frameCache, m_takes[i].stem);
        row.unbuilt = row.hasVideo && !row.hasCache;
        row.empty = !row.hasVideo && !row.hasCache && m_takes[i].impacts == 0;
        unbuiltCount += row.unbuilt ? 1 : 0;
        emptyCount += row.empty ? 1 : 0;
    }

    ImGui::TextDisabled("%zu take(s) in %s", m_takes.size(), m_paths.recordings.string().c_str());

    if (ImGui::Button("Enable all")) {
        for (const rds::RecordingInfo& take : m_takes) m_takeFlags.SetEnabled(take.stem, true);
        m_manageNote = "everything is in the cycle";
    }
    ImGui::SameLine();
    if (ImGui::Button("Disable all")) {
        for (const rds::RecordingInfo& take : m_takes) m_takeFlags.SetEnabled(take.stem, false);
        m_manageNote = "nothing is in the cycle";
    }
    Tip("The Num4 / Num6 cycle only. Nothing is deleted and every take is still\n"
        "selectable from the combo box.");

    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();

    if (ImGui::Button("Clear frame cache")) {
        m_video.Close();
        const std::size_t gone = ClearAllFrameCaches(m_paths.frameCache);
        m_manageNote = std::format("cleared {} frame cache(s)", gone);
        RescanKeepingSelection();
    }
    Tip("Every take's decoded frames. Takes that still have their mp4 rebuild on the\n"
        "next open; takes whose video was already dropped lose their picture for good.");

    ImGui::SameLine();
    ImGui::BeginDisabled(unbuiltCount == 0);
    if (ImGui::Button(std::format("Delete unbuilt recordings ({})", unbuiltCount).c_str())) {
        ImGui::OpenPopup("##delunbuilt");
    }
    ImGui::EndDisabled();
    Tip("Takes that have a video and no frames built from it - the ones recorded and\n"
        "never looked at. Recording is cheap and most takes are one bad shove, so this\n"
        "is the pile that grows.");

    ImGui::SameLine();
    ImGui::BeginDisabled(emptyCount == 0);
    if (ImGui::Button(std::format("Delete recordings without video or impacts ({})", emptyCount)
                          .c_str())) {
        ImGui::OpenPopup("##delempty");
    }
    ImGui::EndDisabled();
    Tip("Takes with no picture and no contact in them. Nothing to hear and nothing to\n"
        "watch - a record button pressed and nobody knocked over.");

    // Both bulk deletions ask first. They are the two buttons in this program
    // that can remove an afternoon of takes, and there is no undo for a file.
    const auto confirm = [&](const char* id, const char* what, auto&& predicate) {
        if (!ImGui::BeginPopup(id)) {
            return;
        }
        ImGui::Text("Delete every take %s?", what);
        ImGui::TextDisabled("Their csv, sidecar, sync track, video and frame cache all go.");
        ImGui::Separator();
        if (ImGui::Button("Delete them")) {
            m_video.Close();
            std::size_t deleted = 0;
            for (std::size_t i = 0; i < m_takes.size(); ++i) {
                if (!predicate(rows[i])) continue;
                std::string error;
                if (DeleteTake(m_takeCsv[i], m_paths.frameCache, error) > 0) {
                    m_takeFlags.Erase(m_takes[i].stem);
                    m_offsets.Erase(m_takes[i].stem);
                    ++deleted;
                }
            }
            m_manageNote = std::format("deleted {} take(s)", deleted);
            RescanKeepingSelection();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    };
    confirm("##delunbuilt", "with a video and no frames", [](const Row& r) { return r.unbuilt; });
    confirm("##delempty", "with no video and no impacts", [](const Row& r) { return r.empty; });

    if (!m_manageNote.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", m_manageNote.c_str());
    }

    ImGui::Separator();

    constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                       ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("takes", 7, kFlags)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("in cycle", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("take", ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableSetupColumn("ms", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("impacts", ImGuiTableColumnFlags_WidthFixed, 56.0f);
        ImGui::TableSetupColumn("video", ImGuiTableColumnFlags_WidthFixed, 76.0f);
        ImGui::TableSetupColumn("note", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 58.0f);
        ImGui::TableHeadersRow();

        int deleteRequest = -1;
        for (int i = 0; i < static_cast<int>(m_takes.size()); ++i) {
            const rds::RecordingInfo& take = m_takes[static_cast<std::size_t>(i)];
            const Row& row = rows[static_cast<std::size_t>(i)];
            ImGui::PushID(i);
            ImGui::TableNextRow();

            ImGui::TableNextColumn();
            bool enabled = m_takeFlags.Enabled(take.stem);
            if (ImGui::Checkbox("##enabled", &enabled)) m_takeFlags.SetEnabled(take.stem, enabled);

            ImGui::TableNextColumn();
            if (ImGui::Selectable(take.stem.c_str(), i == m_take,
                                  ImGuiSelectableFlags_SpanAllColumns |
                                      ImGuiSelectableFlags_AllowOverlap)) {
                SelectRecording(i);
            }

            ImGui::TableNextColumn();
            ImGui::Text("%.0f", take.durationMs);

            ImGui::TableNextColumn();
            if (take.impacts == 0) {
                ImGui::TextDisabled("0");
            } else {
                ImGui::Text("%u", take.impacts);
            }

            ImGui::TableNextColumn();
            if (row.unbuilt) {
                ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.45f, 1.0f), "unbuilt");
                Tip("An mp4 with no frames built from it. Open the take and press Generate\n"
                    "frames, or let this window's bulk button clear the pile.");
            } else if (row.hasCache) {
                ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.6f, 1.0f), "frames");
            } else {
                ImGui::TextDisabled("-");
            }

            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", take.note.c_str());
            Tip(take.note);

            ImGui::TableNextColumn();
            if (ImGui::SmallButton("delete")) deleteRequest = i;

            ImGui::PopID();
        }
        ImGui::EndTable();

        // Outside the loop: deleting rebuilds m_takes, and doing that half way
        // through iterating it is the table reading a vector that has moved.
        if (deleteRequest >= 0) DeleteTakeAt(deleteRequest);
    }

    ImGui::End();
}

}  // namespace tb
