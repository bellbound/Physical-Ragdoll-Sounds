#pragma once

// The whole testbench, as one object with one Draw().
//
// Throwaway code, so it is deliberately flat: the state is public-ish, the UI
// reads it directly, and there is no layer between "the user moved a slider"
// and "RunOffline again and swap the buffer". That last path is the point of
// the program and it is three lines long.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Capture.h"
#include "Export.h"
#include "Link.h"
#include "Mixer.h"
#include "Obs.h"
#include "Player.h"
#include "SfxBrowser.h"
#include "Video.h"

#include "rds/Config.h"
#include "rds/Offline.h"
#include "rds/Recording.h"
#include "rds/Sfx.h"
#include "rds/SlotManifest.h"

namespace tb {

/// One step of a side's undo history: the whole config as it was before an
/// edit, and what that edit touched, so the button can say what it will undo.
///
/// The whole config, rather than the one parameter, because a config is a few
/// hundred bytes and "restore this" cannot get out of step with the schema the
/// way a per-field patch can.
struct ConfigEdit {
    rds::AlgorithmConfig cfg{};
    std::string label;
    /// Where this step sits in the one history the user actually has. Config
    /// edits and loop-region changes live in separate stacks - they undo
    /// different kinds of thing - but Ctrl+Z has to mean "the last thing I did"
    /// regardless of which kind that was, and comparing these decides it.
    std::uint64_t seq{};
};

/// One step of the loop region's history. The region is a selection rather than
/// a value, but it is edited by hand, it is easy to lose by a stray click, and
/// re-drawing one you had zoomed to is fiddly - so it is undoable like anything
/// else you can type.
struct RegionEdit {
    double startMs{};
    double endMs{};
    std::string label;
    std::uint64_t seq{};
};

/// One step of the sfx assignment history.
///
/// The whole table rather than the one slot, for the same reason ConfigEdit
/// holds the whole config: it is a few hundred bytes and "put this back" cannot
/// get out of step with the manifest the way a per-slot patch can. Global
/// rather than per side, because which sfx a slot plays is a property of the
/// bank and both sides play through the same bank.
struct SfxEdit {
    rds::SfxAssignments state;
    std::string label;
    std::uint64_t seq{};
};

/// One half of the right-hand panel. In split mode there are two of these and
/// playback alternates between their buffers on each loop.
struct ConfigSide {
    rds::AlgorithmConfig cfg{};
    std::string name{"(unsaved)"};
    char saveName[64]{};

    rds::OfflineResult result;
    std::shared_ptr<MixedAudio> audio;
    double runMs{};  ///< how long RunOffline took, for the "it is instant" claim
    bool dirty{true};

    std::vector<rds::VerifyExpectation> verify;
    bool verifyRun{};

    /// True when this side carries edits that are not in any file. Separate
    /// from `dirty`, which means "the audio needs re-rendering" and is cleared
    /// a few milliseconds later - this one survives until something writes.
    bool unsaved{true};

    // ── undo of the unsaved edits ────────────────────────────────────────────
    // Only the slider/checkbox/combo values live here. Loading a named config
    // is a new baseline, not a step, so it clears both stacks.
    std::vector<ConfigEdit> undo;
    std::vector<ConfigEdit> redo;

    /// Open from the frame a widget first moves until the frame it is released:
    /// a drag across fifty frames has to be one undo step, not fifty.
    bool editOpen{};
    rds::AlgorithmConfig editBase{};
    std::string editLabel;
};

class App {
public:
    struct Paths {
        std::filesystem::path recordings;
        /// RagdollSounds.ini, beside the sfx ini in the deployed mod. Read, never
        /// written: the testbench needs `[Devbench]` out of it - the port to
        /// listen on and where OBS lives - and those are properties of the
        /// machine the game runs on, so there is nowhere else for them to be
        /// true. Empty means "look next to sfxIni".
        std::filesystem::path generalIni;
        std::filesystem::path configs;
        std::filesystem::path frameCache;
        std::filesystem::path sounds;  ///< the built pack, for slots with no assignment
        std::filesystem::path library;  ///< every sfx that exists, with its metadata
        /// RagdollSounds_SFX.ini. Written straight into the deployed mod rather
        /// than beside the algorithm configs: an algorithm config is one of many
        /// you A/B between, and the sfx assignment is the one thing the mod
        /// ships with. There is nothing to iterate over.
        std::filesystem::path sfxIni;
    };

    bool Init(const Paths& paths);
    void Draw();

    /// Dump everything currently in memory to a text file next to the configs.
    /// Both the button and `--export` land here, so what you read on disk is
    /// exactly what the running program is holding.
    std::filesystem::path ExportState(std::string& error);

    /// Open the take whose stem contains `match` instead of the first one.
    /// Command-line only, so a take with video can be opened without clicking.
    void SelectRecordingByName(const std::string& match);

    [[nodiscard]] Player& Transport() { return m_player; }
    [[nodiscard]] const std::string& StartupError() const { return m_startupError; }

    /// Close the link and stop anything recording. Called before the GL context
    /// goes away, because a capture that finishes into a dead window is a crash
    /// on the way out.
    void Shutdown();

private:
    // ── state ────────────────────────────────────────────────────────────────
    void ScanRecordings();
    void ScanConfigs();
    void SelectRecording(int index);
    void StepRecording(int delta);
    void StepConfig(int delta);
    void LoadConfigFile(int side, const std::filesystem::path& file);
    void SaveConfigFile(int side);
    /// The first unused `config_<dd>_<mm>_<n>` for today.
    [[nodiscard]] std::string NextConfigName() const;
    /// `config_22_08_4` to `config_22_08_5`, skipping any that exist. Anything
    /// that does not end in `_<number>` gets `_2`.
    [[nodiscard]] std::string NextIteration(std::string_view name) const;
    /// Everything unsaved, written. Ctrl+S, and the top bar's button.
    void SaveEverything();
    [[nodiscard]] bool AnythingUnsaved() const;
    void Rerun(int side);
    void RerunDirty();

    // ── undo ─────────────────────────────────────────────────────────────────
    void BeginEdit(int side, std::string_view label);
    void CommitEdit(int side);
    void PushEdit(int side, const rds::AlgorithmConfig& before, std::string_view label);
    void Undo(int side);
    void Redo(int side);
    void ClearHistory(int side);
    /// Ctrl+Z / Ctrl+Y. Undoes whichever happened last - a parameter edit or a
    /// change to the loop region - rather than always the parameter.
    void UndoLatest();
    void RedoLatest();
    void UndoRegion();
    void RedoRegion();
    /// The only way the region is allowed to change, so every change is a step.
    void SetRegion(double startMs, double endMs, std::string_view label);
    [[nodiscard]] static bool SameConfig(const rds::AlgorithmConfig& a, const rds::AlgorithmConfig& b);

    void SetSplit(bool on);
    void CollapseOnto(int side);

    // ── the switches that are not config ─────────────────────────────────────
    //
    // The limiter, the trace, the two auto-scrolls, snap-on-play, save-as-
    // iteration, push-to-game, the video mode, split, auto-loop. None of them
    // change what the mod does - they change what this program shows and how it
    // behaves while you work - and every one of them was back at its compiled-in
    // default at the next launch. The first minute of every session was putting
    // them back, and the one that mattered was the limiter: it is on by default
    // and it is the one thing the game cannot do, so a session that forgot to
    // turn it off was a session spent listening to a mix Skyrim will never play.
    //
    // Kept out of the algorithm inis on purpose. A config is something you A/B,
    // save under a dated name and ship; these are properties of this machine, and
    // a diff between two configs must never contain one.

    /// The persisted switches, as (ini key, member). One list, walked by both
    /// the load and the save, so remembering a new checkbox is one line here
    /// and nothing else.
    [[nodiscard]] std::vector<std::pair<std::string_view, bool*>> UiPrefFields();
    void LoadUiPrefs();
    /// Called once a frame. Writes only when something actually moved, so the
    /// cost of it being unconditional is one string build per frame.
    void SaveUiPrefs();

    // ── the sfx assignments ──────────────────────────────────────────────────
    //
    // Defined in AppSfx.cpp: the panel is a self-contained piece of UI over a
    // self-contained piece of state, and App.cpp is long enough.

    /// Rebuild the bank from the library and the current assignments, drop the
    /// decoded buffers, and re-run both sides. Every path that changes what a
    /// slot plays ends here, which is what makes the change audible at once.
    void ApplySfx();
    void LoadSfx();
    void SaveSfx();
    /// Snapshot before an assignment edit, then apply it. One call per gesture:
    /// picking a file, removing one, toggling looping.
    void PushSfxEdit(const rds::SfxAssignments& before, std::string_view label);
    void UndoSfx();
    void RedoSfx();
    void DrawSfxPanel(float height);
    void DrawSlotWidget(rds::SlotId slot);
    /// Slots in the order the panel lists them: the ones this take actually
    /// used first, then the order an impact arrives in.
    [[nodiscard]] std::vector<rds::SlotId> SlotOrder() const;
    /// True when the current side's cue list contains this slot.
    [[nodiscard]] bool SlotInTimeline(rds::SlotId slot) const;

    [[nodiscard]] double VideoTimeMs(double takeMs) const;

    // ── the devbench link ────────────────────────────────────────────────────
    //
    // Defined in AppLive.cpp. The link, OBS, the live recording and the two
    // windows that manage takes are one subject and App.cpp is long enough.

    /// Read `[Devbench]` out of RagdollSounds.ini, open the listener, and point
    /// the OBS driver at the exe the same file names.
    void StartLink();

    /// Push whatever changed to the game: the focused side's config, the sfx
    /// table, the library path. Called every frame; sends only on a difference,
    /// and sends all three on a fresh connection - which is what "the mod pulls
    /// its config from the devbench" actually is, from this end.
    void SyncToGame();

    /// The connection state, and the record buttons, right-aligned on the
    /// transport row.
    void DrawLinkRow();
    void DrawOptions();
    void DrawRecordingManager();

    // ── recording from the game ──────────────────────────────────────────────

    void StartLiveRecording();
    void StopLiveRecording();
    /// OBS has answered the start. `started` false means there is no video and
    /// the take is data only, which is a perfectly good take.
    void OnVideoArmed(bool started);
    /// Everything has landed: write the CSV, move the video beside it, rescan.
    void FinishLiveRecording(const std::string& videoPath);

    // ── acting on a take ─────────────────────────────────────────────────────

    /// Delete the current take, its sidecars, its video and its frame cache.
    void DeleteTakeAt(int index);
    /// Build the current take's frame cache and drop the mp4.
    void BuildFrameCacheNow();
    /// The timeline selection's right-click actions.
    void DeleteSelection();
    void CreateRecordingFromSelection();
    void ExtractSelectionVideo();
    void DeleteTakeVideo();

    /// Reload the take list and keep the same stem selected if it is still
    /// there. Every path that adds or removes a take ends here.
    void RescanKeepingSelection();

    /// Which video mode a take should open in, given the options.
    [[nodiscard]] VideoTake::Mode VideoMode() const;

    // ── ui ───────────────────────────────────────────────────────────────────
    void DrawTopBar();
    void DrawLeft(float width);
    void DrawVideo(float height);
    void DrawTransport();
    void DrawTimeline(float height);
    /// The timeline selection's right-click menu. Its own function because the
    /// popup body is a list of actions and the timeline is already the longest
    /// function here.
    void DrawSelectionMenu();
    void DrawStats();
    void DrawSelectedCue();
    /// Every impact the take reported, and every cue the engine emitted, side by
    /// side. Both sortable: the question is usually "what were the hardest hits"
    /// or "what were the loudest cues", and scrolling a time-ordered list to find
    /// that out is the wrong tool.
    void DrawImpactsTable();
    void DrawCuesTable();

    /// The stretch of the take everything below the timeline is talking about:
    /// the loop region when one is set, otherwise the whole thing.
    ///
    /// One definition, read by the counts and the tables alike, so "44 cues" and
    /// the list under it can never disagree about which cues they mean.
    void WindowMs(double& lo, double& hi) const;
    [[nodiscard]] bool WindowIsRegion() const;

    /// Scroll a table so the row nearest the playhead stays visible. Only while
    /// playing - doing it always would fight the scroll wheel.
    void FollowPlayhead(const std::vector<double>& times) const;
    void DrawRight(int side, float height, bool split);
    void DrawParams(int side);
    void HandleKeys();

    Paths m_paths;
    std::string m_startupError;

    std::vector<rds::RecordingInfo> m_takes;
    std::vector<std::filesystem::path> m_takeCsv;
    int m_take{-1};
    std::unique_ptr<rds::Recording> m_recording;
    std::string m_recordingError;

    std::vector<std::filesystem::path> m_configFiles;
    int m_configIndex{-1};

    rds::SoundBank m_bank;
    rds::SfxLibrary m_library;
    rds::SfxAssignments m_sfx;
    /// What was last written, so "unsaved" is a comparison and not a flag
    /// somebody has to remember to set.
    rds::SfxAssignments m_sfxSaved;
    SfxBrowser m_browser;
    std::vector<SfxEdit> m_sfxUndo;
    std::vector<SfxEdit> m_sfxRedo;
    /// Which slot's widget the mouse is over, for the timeline highlight, and
    /// the answer being built for the next frame. Two fields because the left
    /// column draws the timeline *before* the right column draws the widget
    /// that would set it - so the timeline reads last frame's answer, which on
    /// a hover highlight is not something an eye can see.
    int m_hoverSlot{-1};
    int m_hoverSlotPending{-1};
    char m_sfxFilter[64]{};
    SoundSource m_sources;
    Player m_player;
    VideoTake m_video;
    SyncModel m_sync;
    OffsetStore m_offsets;
    double m_videoOffsetMs{};
    /// False until the offset is either loaded from disk or centred once the
    /// clip's length is known - the length only arrives when ffmpeg finishes.
    bool m_videoOffsetKnown{};

    /// Layout, all draggable and all fractions of the window so they survive a
    /// resize. Persisted nowhere: cheap to set, and a wrong remembered split is
    /// more annoying than re-dragging one.
    float m_leftFrac{0.55f};    ///< left column against the config panel
    float m_videoFrac{0.42f};   ///< video against everything under it
    float m_tableFrac{0.5f};    ///< impacts against cues
    float m_configFrac{0.62f};  ///< the config panel against the sfx panel under it

    ConfigSide m_side[2];
    bool m_split{};
    int m_focusSide{};

    int m_selectedCue{-1};
    char m_filter[64]{};
    float m_monitorDb{-3.0f};
    /// The one thing the testbench does that the game cannot. On by default so a
    /// stacked burst does not wrap, but the flag and the pre-limiter peak are
    /// both on screen (00 section 13).
    bool m_limiter{true};
    bool m_showTrace{true};
    bool m_showContacts{true};
    /// Snap the timeline to the loop region when playback starts.
    ///
    /// It used to rescale the moment a region was drawn, which fought every
    /// other way of moving around the timeline: the wheel would zoom out and the
    /// next frame would put it back. Playback is the one moment where "show me
    /// the bit I selected" is unambiguous, so that is the only time it happens.
    bool m_zoomRegion{true};

    // ── the timeline view ────────────────────────────────────────────────────
    //
    // Free state rather than something derived from the loop region: the wheel
    // zooms it and the middle button pans it, and a view recomputed from the
    // region every frame cannot be moved by hand at all.
    double m_viewStartMs{};
    double m_viewSpanMs{};
    bool m_viewValid{};
    /// Edge-detects the transport so the snap above happens once, on the frame
    /// playback starts, and not for as long as it is playing.
    bool m_wasPlaying{};

    // ── the timeline selection ───────────────────────────────────────────────
    //
    // Deliberately not the loop region. The region says what to *listen to*; the
    // selection says what to *do something to* - cut a take out of, delete, pull
    // a clip from. Sharing one would mean every right-click menu was also a
    // statement about playback.
    bool m_hasSelection{};
    double m_selStartMs{};
    double m_selEndMs{};
    bool m_selecting{};
    double m_selAnchorMs{};
    /// True from the frame a left press lands on the timeline body until it is
    /// either dragged (a selection) or released (a seek).
    bool m_maybeSelecting{};
    float m_selPressX{};
    std::string m_selectionNote;
    /// True between mouse-down and mouse-up on the region strip. The zoom and the
    /// window filter both wait for it to clear: rescaling on every frame of the
    /// drag moved the timeline out from under the cursor, so the region you let
    /// go on was never the one you were aiming at.
    bool m_regionDragging{};
    double m_regionAnchorMs{};
    /// The window as it stood when the drag began, held for the whole gesture.
    /// Without it a drag inside an existing region would fall back to the whole
    /// take on the first frame - the timeline would jump out to full scale under
    /// the cursor, which is the opposite of narrowing down.
    double m_frozenLoMs{};
    double m_frozenHiMs{};
    bool m_frozenIsRegion{};
    /// The region before the drag, so the whole gesture is one undo step.
    double m_dragPrevStartMs{};
    double m_dragPrevEndMs{};

    /// Loop-region history, alongside each side's config history. Ctrl+Z reads
    /// both and takes the newer.
    std::vector<RegionEdit> m_regionUndo;
    std::vector<RegionEdit> m_regionRedo;
    std::uint64_t m_editSeq{};

    /// Per table, because you often want one pinned to a row you are reading
    /// while the other keeps up with the video.
    bool m_followImpacts{true};
    bool m_followCues{true};
    /// Mirrors Player::Loop(). The transport owns the flag, but the preference
    /// file needs somewhere to land before the player exists.
    bool m_autoLoop{true};
    std::uint32_t m_seed{1};
    std::string m_exportNote;  ///< what the last export did, shown beside the button
    std::string m_saveNote;    ///< what the last Ctrl+S wrote
    /// A save writes the next iteration of the loaded config rather than over
    /// it - `config_22_08_4` becomes `config_22_08_5`. On by default, because
    /// the reason to save mid-session is nearly always that the last one was
    /// worth keeping too. Off overwrites whatever the name box says.
    bool m_saveAsIteration{true};

    // ═════════════════════════════════════════════════════════════════════════
    // the devbench link
    // ═════════════════════════════════════════════════════════════════════════

    GameLink m_link;
    rds::GeneralConfig m_general{};
    std::filesystem::path m_generalIni;
    /// Which takes are in the Num4 / Num6 cycle.
    TakeFlags m_takeFlags;

    /// Off stops pushing config at the game without dropping the connection, so
    /// a take can be recorded against what the mod's own inis say rather than
    /// against whatever is on the sliders.
    bool m_pushToGame{true};
    /// What was last sent, so a push is a difference and not a flood. Invalid
    /// until the first push of a connection, which is what makes a reconnect
    /// send everything again.
    bool m_pushedValid{};
    bool m_wasConnected{};
    rds::AlgorithmConfig m_pushedConfig{};
    rds::SfxAssignments m_pushedSfx;
    std::string m_pushedLibrary;

    // ── recording from the game ──────────────────────────────────────────────

    enum class RecordState {
        kIdle,
        kArming,     ///< OBS has been asked to start and has not answered
        kRecording,
        kWriting,    ///< OBS has been asked to stop and has not answered
    };
    RecordState m_recordState{RecordState::kIdle};
    /// The stem the take will get. Decided at the start, because OBS is told to
    /// name its own output after it and the two have to agree.
    std::string m_recordStem;
    char m_recordNote[128]{};
    std::string m_recordError;
    std::chrono::steady_clock::time_point m_recordStarted{};
    /// True while OBS is recording for us, so the finish knows whether to expect
    /// a file.
    bool m_recordHasVideo{};

    // ── windows and notes ────────────────────────────────────────────────────

    bool m_showOptions{};
    bool m_showManager{};
    /// The experimental one: use the mp4 directly rather than building a frame
    /// cache from it. See VideoTake::Mode.
    bool m_videoSync{};
    std::filesystem::path m_uiPrefsFile;
    /// The text of the last write. A save that would produce this again does
    /// nothing, which is what makes calling it every frame free.
    std::string m_uiPrefsWritten;

    std::string m_videoNote;   ///< what the last cache build or clip cut did
    std::string m_manageNote;  ///< what the last recording-manager button did
    std::uint64_t m_obsNoteSeq{};
    std::string m_obsNote;
};

}  // namespace tb
