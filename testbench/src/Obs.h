#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

/// Drives OBS Studio's built-in WebSocket server (obs-websocket 5.x) over
/// loopback, so the testbench can start a video take, stop it, and be told where
/// OBS wrote the file.
///
/// A port of QuickModMenuNG's Obs.h/.cpp - see the top of Obs.cpp for exactly
/// what was replaced and why it is a copy rather than a shared file.
///
/// Every call here returns immediately. The socket lives on threads of its own
/// and the UI reads a snapshot; callbacks are handed back on the UI thread by
/// Pump(), which Draw() calls once a frame.
namespace tb::obs {

/// Whether OBS can pause this recording. There is no query for it — OBS greys
/// its own Pause button out whenever the recording shares the stream encoder —
/// so it starts Unknown, is guessed from the profile at connect, and is settled
/// for good by the first PauseRecord that comes back refused.
enum class Tri { Unknown, Yes, No };

struct Status {
    bool connected = false;
    bool recording = false;
    bool paused = false;
    /// Paused only because the menu is up, and so due to be resumed when it
    /// closes. Never true at the same time as a manual pause.
    bool autoPaused = false;
    Tri pauseSupported = Tri::Unknown;
    bool chapterSupported = false;
    std::uint64_t durationMs = 0;
    std::string scene;
};

/// Where OBS is and how to reach it. Everything except `obsExe` used to come
/// from QuickModMenuNG's ini; here it comes from `[Devbench]` in
/// RagdollSounds.ini, plus obs-websocket's own config for whatever is left
/// blank - which is nearly always the port and the password, because
/// obs-websocket generates one for itself and writes it down.
struct Config {
    bool enabled{true};
    std::string host;      ///< empty asks OBS's own config; loopback either way
    std::uint16_t port{};  ///< 0 asks OBS's own config
    std::string password;  ///< empty asks OBS's own config
    /// Scene to switch to before recording. Empty records whatever OBS is on.
    std::string scene;
    bool autoPauseInMenu{false};
    bool pauseImpactTakesInMenu{false};
    /// obs64.exe, so a testbench that cannot find a running OBS can start one.
    /// Read from `[Devbench] sObsPath`. Empty means "only drive one that is
    /// already up".
    std::filesystem::path obsExe;
};

/// Must be called before Connect(). The settings are read from the socket
/// threads without a lock, which is safe exactly once: at startup, before
/// anything has been spawned.
void Configure(const Config& config);

/// Start obs64.exe if `Config::obsExe` names one and nothing is listening on the
/// websocket port yet.
///
/// Returns false with a reason when there is no path, the file is not there, or
/// CreateProcess refused. A *successful* launch still means waiting: OBS takes
/// several seconds to come up and load obs-websocket, and Connect() keeps
/// retrying in the background for as long as that takes.
bool Launch(std::string& error);

/// Hand the UI thread whatever the socket threads have finished. Called once a
/// frame; without it no StartTake or StopTake callback ever runs.
void Pump();

/// The last thing worth showing a human. `seq` increments on each new one, so a
/// caller can tell "still the old message" from "it said that again".
[[nodiscard]] std::string LastNote(std::uint64_t& seq);

/// False until Configure() sets it. Nothing below connects while this is false.
[[nodiscard]] bool Configured();

/// Lock-free, safe from any thread, cheap enough for every page build.
[[nodiscard]] Status Now();

/// "04:12" — the running timecode, for a row label.
[[nodiscard]] std::string Clock();

/// "recording 04:12" / "paused 04:12" / "connected" / "not connected".
[[nodiscard]] std::string Summary();

/// Bring the socket up. A no-op while already connected or already trying.
void Connect();
/// Send a close frame and let the connection wind down. Nothing here calls it;
/// it exists for a debug row and for symmetry.
void Disconnect();

/// Switch to the configured scene and start recording. The scene and the start
/// are queued in order on the one socket, so the scene is live before the output
/// opens.
void StartRecording();
/// Start once the menu is down, so the list panel is not in the first seconds of
/// the take. Fired by Session::End.
void StartRecordingOnMenuClose();
void StopRecording();

/// The Pause / Resume row. Sticky: closing the menu does not undo it.
void SetManualPause(bool paused);

/// The auto-pause pair, called by Session::Begin and Session::End.
///
/// MenuOpened is a no-op on an already manually-paused take, and MenuClosed only
/// resumes what MenuOpened paused — so opening the menu on a paused recording and
/// closing it again cannot silently restart it.
void MenuOpened();
void MenuClosed();

/// A chapter marker in the running recording. Silently nothing on an OBS that
/// does not advertise CreateRecordChapter, or on a non-hybrid-MP4 output.
void Chapter(std::string name);

// ── the impact recorder's side door ──────────────────────────────────────────

/// True while an impact take owns the recording, which is what makes the
/// auto-pause defer to [OBS] PauseImpactTakesInMenu.
void SetTakeActive(bool active);

/// Point OBS's FilenameFormatting at `stem`, start recording, and call `armed`
/// on the game thread once OBS reports the output actually running — or after
/// `timeoutMs` with `started` false, which means "no video, take the data
/// anyway".
void StartTake(std::string stem, std::uint32_t timeoutMs, std::function<void(bool started)> armed);

/// Stop, restore the player's own FilenameFormatting, and report the file OBS
/// wrote. `done` runs on the game thread, with an empty path if nothing was
/// recorded.
void StopTake(std::function<void(std::string outputPath)> done);

/// One clock sample for the sync track: OBS's own output duration, and the round
/// trip it took to ask, so the analysis knows how much to trust the row. Runs on
/// the game thread. Dropped silently when nothing is recording.
void SampleClock(std::function<void(std::uint64_t obsMs, std::uint64_t rttMs)> sample);

/// What OBS said the last finished recording was written to. Empty until a stop
/// has been answered.
[[nodiscard]] std::string LastOutputPath();

/// Whatever OBS knew about itself at connect, for the take's metadata.
[[nodiscard]] std::string Version();
[[nodiscard]] std::string RecordDirectory();

void SetScene(std::string scene);
/// The cached scene list, refreshed at connect. Empty until the first answer
/// lands.
[[nodiscard]] std::vector<std::string> Scenes();

}  // namespace tb::obs
