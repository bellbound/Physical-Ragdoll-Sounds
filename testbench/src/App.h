#pragma once

// The whole testbench, as one object with one Draw().
//
// Throwaway code, so it is deliberately flat: the state is public-ish, the UI
// reads it directly, and there is no layer between "the user moved a slider"
// and "RunOffline again and swap the buffer". That last path is the point of
// the program and it is three lines long.

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Bench.h"
#include "Capture.h"
#include "Control.h"
#include "Export.h"
#include "Link.h"
#include "Mixer.h"
#include "VanillaLibrary.h"
#include "Obs.h"
#include "Player.h"
#include "SfxBrowser.h"
#include "Video.h"

#include "rds/Config.h"
#include "rds/ConfigSchema.h"
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

/// Which file a cue is actually playing.
///
/// A cue carries a slot and a variant index, and neither of those is the answer
/// to "what am I hearing": the variant is a position in a list the assignment
/// panel re-orders, the slot may be pinned to one file for the session, and a
/// slot with nothing assigned plays its declared fallback's files, and one with
/// no fallback either does not sound at all. Resolving all four in one place
/// means the timeline, the cue table and the export cannot give three different
/// answers.
struct CueSound {
    std::string label;  ///< what to show: the library name, or "(no recording)"
    std::string file;   ///< the library filename, empty when nothing sounds
    int variant{};
    int variantCount{};  ///< how many files the slot has to pick between
    /// The slot whose files this is, when that is not the slot the cue named:
    /// `scrape_limb_wood` with nothing recorded plays `scrape_limb`. Equal to
    /// the cue's own slot in the ordinary case.
    rds::SlotId plays{};
    bool fellBack{};
    /// Nothing to play: no recording on this slot and none on anything it falls
    /// back to, so the cue is in the list and silent in the mix.
    bool silent{};
    /// The slot is pinned, so this file was not picked - it was the only one
    /// allowed. Worth saying, because a pin is invisible from anywhere but the
    /// slot's own widget and it is what makes the variant column stop moving.
    bool forced{};
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

    // ── the config this one is being read against ────────────────────────────
    //
    // A second named config, loaded but never played and never edited. It
    // exists so the panel can say which parameters this side actually moved:
    // "config_22_08_17 with the ramp pulled in" is a sentence about two files,
    // and until this was here the only way to get it was two windows and a
    // scroll. Not a third side - nothing renders it, nothing saves it, and
    // switching it costs no re-run.
    std::string compareName;  ///< empty means no comparison is running
    rds::AlgorithmConfig compareCfg{};

    /// What the last patch off the control socket said it was for, if one has
    /// landed on this side. Shown on the picker's tooltip: a config that
    /// appeared while you were listening to something else should be able to
    /// say where it came from. Cleared by loading a file, which is a new
    /// baseline and not that patch any more.
    std::string patchNote;
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
        /// An extract of the game's `sound/fx` tree, for playing a take's vanilla
        /// track back. Dev-only and never deployed - Bethesda's files, exactly
        /// like `assets/sfx/skyrim/`, and kept outside everything that ships for
        /// the three reasons that folder's README sets out. Empty is fine: the
        /// vanilla side is then silent and says why.
        std::filesystem::path vanillaSounds;
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

    /// Answer one request off the control socket, if one is waiting.
    ///
    /// Called from the frame loop rather than from Draw, and *before* the check
    /// that skips drawing while the window is minimised: patching a testbench
    /// that is sitting in the taskbar behind the game is the ordinary case for
    /// this, not the edge one. Defined in Control.cpp.
    void PumpControl();

private:
    // ── state ────────────────────────────────────────────────────────────────
    void ScanRecordings();
    void ScanConfigs();
    void SelectRecording(int index);
    void StepRecording(int delta);
    void StepConfig(int delta);
    void LoadConfigFile(int side, const std::filesystem::path& file);
    void SaveConfigFile(int side);

    // ── comparing this side against another named config ─────────────────────

    /// Load a config into this side's compare slot. Nothing re-runs and nothing
    /// this side plays changes - it only changes what the panel marks.
    void LoadCompareFile(int side, const std::filesystem::path& file);
    void ClearCompare(int side);
    [[nodiscard]] bool CompareOn(int side) const { return !m_side[side].compareName.empty(); }
    /// True when this parameter would be written differently in the two files.
    ///
    /// Compared as the text the ini carries rather than as doubles: the file is
    /// four decimals, a slider lands on the nearest float, and two configs that
    /// write the same line are the same config. Bit-for-bit would report half
    /// the table as changed the moment a value made a round trip through disk.
    [[nodiscard]] static bool ParamDiffers(const rds::AlgorithmConfig& a,
                                           const rds::AlgorithmConfig& b, const rds::ParamDesc& p);
    /// How many parameters differ between this side and its compare config, and
    /// how many are identical. Walked fresh each frame - it is ~200 doubles and
    /// a format, and a cached count that went stale after an undo would be worse
    /// than useless.
    void CompareCounts(int side, int& differing, int& same) const;
    /// The differing rows, in schema order, for the report window.
    [[nodiscard]] std::vector<const rds::ParamDesc*> CompareDeltas(int side) const;
    void DrawCompareReport(int side);
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

    /// Pause, then run the loaded take through the backend as fast as it will
    /// go and record what that cost. Both sides while split, so the answer is a
    /// comparison rather than a number - which is the only form the question
    /// "does this config cost more" has an answer in.
    void RunBenchmark();

    /// Point the mixer at whatever the picker is highlighting, and say whether
    /// that changed. The re-mix is the caller's, because on the frame a pick is
    /// committed the assignment triggers one anyway and two would be a wasted
    /// pass over the whole take.
    bool SyncSfxAudition();

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
    /// Owned keys rather than views: the per-slot timeline switches build their
    /// names from the slot's own, so there is nothing static to point at.
    [[nodiscard]] std::vector<std::pair<std::string, bool*>> UiPrefFields();

    /// One remembered splitter: the ini key, the fraction, and the range its own
    /// bar drags between.
    struct FracPref {
        std::string_view key;
        float* member;
        float lo;
        float hi;
    };

    /// The same idea as UiPrefFields for the layout, in a second list because a
    /// fraction read off disk needs clamping and a bool does not.
    [[nodiscard]] std::vector<FracPref> UiPrefFracFields();
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
    /// Push the session's forces and mutes onto the freshly loaded bank, and
    /// drop the ones whose file is no longer on the slot. Called by ApplySfx,
    /// which is the only thing that rebuilds the bank.
    void ApplySfxSession();
    [[nodiscard]] bool SfxForced(rds::SlotId slot, std::string_view file) const;
    [[nodiscard]] bool SfxMuted(rds::SlotId slot, std::string_view file) const;
    /// Pin this file on this slot, or unpin it when it is already pinned. One
    /// slot has at most one pin, so pinning a second file moves it.
    void ToggleSfxForce(rds::SlotId slot, std::string_view file);
    void ToggleSfxMute(rds::SlotId slot, std::string_view file);
    /// Move placement `index` of `from` onto `to` under `condition`, or copy it
    /// there when `copy`. What dropping a dragged row does, and one undo step.
    ///
    /// Within one slot a move is a re-tag, because the placement's position is
    /// the variant index a recorded cue carries: sending the row to the end of
    /// the list to put a condition on it would change which sound every take in
    /// the corpus plays. A drop back where the file already is does nothing.
    void DropSfxPlacement(rds::SlotId from, int index, rds::SlotId to,
                          rds::VariantCondition condition, bool copy);
    [[nodiscard]] int SfxForceCount() const;
    [[nodiscard]] int SfxMuteCount() const;
    void ClearSfxForces();
    void ClearSfxMutes();
    void DrawSfxPanel(float height);
    void DrawSlotWidget(rds::SlotId slot);
    /// Slots in the order the panel lists them: the ones this take actually
    /// used first, then the order an impact arrives in.
    [[nodiscard]] std::vector<rds::SlotId> SlotOrder() const;
    /// True when the current side's cue list contains this slot.
    [[nodiscard]] bool SlotInTimeline(rds::SlotId slot) const;
    /// Which file this cue plays. A pure lookup through the bank - never
    /// `Resolve`, which would advance the shuffle bag and answer with a
    /// different file than the one the cue list was built from.
    [[nodiscard]] CueSound SoundOf(const rds::Cue& cue) const;

    [[nodiscard]] double VideoTimeMs(double takeMs) const;

    // ── the devbench link ────────────────────────────────────────────────────
    //
    // Defined in AppLive.cpp. The link, OBS, the live recording and the two
    // windows that manage takes are one subject and App.cpp is long enough.

    /// Read `[Devbench]` out of RagdollSounds.ini, open the listener, and point
    /// the OBS driver at the exe the same file names.
    void StartLink();

    // ── the control socket ───────────────────────────────────────────────────
    //
    // Defined in Control.cpp, which also holds the server itself.

    /// Open the control listener on the devbench port plus one. After
    /// StartLink, which is what read that port.
    void StartControl();

    /// Do what one request asks and render the reply. UI thread only.
    [[nodiscard]] std::string ApplyControl(const ControlRequest& request);

    /// The config file a name picks out: an exact stem, or a substring when it
    /// picks out exactly one. Null when it names none or several.
    [[nodiscard]] const std::filesystem::path* FindConfigFile(std::string_view name) const;

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
    /// Pair OBS's output clock with the game's and keep the row, so the take can
    /// be lined up with its video afterwards. See the sync notes in Capture.h.
    ///
    /// `force` takes one now; without it the interval decides. Nothing happens
    /// when there is no video on this take or no heartbeat to read a game clock
    /// off, which is a take that simply has no sync track - not an error.
    void SampleTakeClock(bool force);
    /// Write the finished take's `_sync.csv` and the `obs:` block of its sidecar,
    /// rebased onto the clock `WriteTake` landed the take on. A no-op without
    /// video, which is the only case where neither means anything.
    void WriteTakeSync(const std::string& stem, const TakeWindow& window,
                       const std::filesystem::path& video);

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
    /// True when this take's offset came out of video-offsets.ini rather than
    /// out of the clip-pad guess. Not the same question as m_videoOffsetKnown,
    /// which the guess also answers yes.
    [[nodiscard]] bool OffsetMeasured() const;

    // ── ui ───────────────────────────────────────────────────────────────────
    void DrawTopBar();
    void DrawLeft(float width);
    void DrawVideo(float height);
    void DrawTransport();
    /// The benchmark button and its answer. Its own function rather than four
    /// more inches of DrawTransport, which is already the second longest thing
    /// in this file.
    void DrawBenchmarkRow();
    void DrawSimulateRow();
    void MarkPretendDirty();
    /// Push the two cue-window switches at the transport, once per frame.
    ///
    /// Not part of drawing them: the skip has to happen whether or not the
    /// benchmark row is on screen, and a transport that only advanced while
    /// its own checkbox was visible would be a transport with a hiding place.
    void UpdateCueWindow();
    void DrawTimeline(float height);
    /// The measured body at the playhead, under the timeline. One line,
    /// because the question it answers - "how fast was the body going when
    /// that fired" - is asked while looking at something else.
    void DrawBodyReadout(const ConfigSide& side);
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

public:
    /// Read a whole testbench config: the algorithm rows, then which surfaces
    /// the file has blocks for, then those blocks, then Resolve.
    ///
    /// Public, with the writer's half below, because a testbench config is one
    /// file where the game keeps two - so anything that reads or writes one has
    /// to splice the surfaces list in the same way, and Export and Control both
    /// do. Two spellings of "which rows belong in this file" is how the export
    /// would quietly start disagreeing with the file it claims to be a copy of.
    static void LoadAlgorithmFile(const std::filesystem::path& file, rds::AlgorithmConfig& cfg);

    /// The rows a testbench config is written from: everything, plus the
    /// surfaces that are opened. Closed classes contribute nothing.
    static std::vector<rds::ParamDesc> AlgorithmAndOpenedSurfaces(const rds::AlgorithmConfig& cfg);

private:
    void DrawParams(int side);

    /// The `+` at the bottom of the parameter panel, and the popup that lists
    /// every surface without a block of its own. Not a schema row, because it
    /// decides whether a set of schema rows is shown at all.
    void DrawSurfaceAdd(int side);

    /// What a surface inherits from, for the button and the popup labels:
    /// the parent class's name, or `[Surfaces]` at a root.
    static std::string SurfaceParentName(rds::SurfaceClass surface);
    /// One parameter: its name, its widget, and the edit history around them.
    ///
    /// `startX` and `width` are the column it draws into, both measured from the
    /// panel's left edge, because a row the schema paired with the one before it
    /// shares that row's line at the halfway mark. A full-width row is
    /// `startX == 0`.
    void DrawParam(int side, const rds::ParamDesc& p, float startX, float width);
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
    /// The audition volume, in dB on the player's preview voice. Here rather
    /// than in the browser because it is a ui preference like every other
    /// switch in ui-state.ini, and those are written from one list of pointers
    /// that has to outlive any one window.
    float m_sfxPreviewGainDb{0.0f};
    std::vector<SfxEdit> m_sfxUndo;
    std::vector<SfxEdit> m_sfxRedo;
    /// One slot's pin: a file every cue for the slot plays, whatever the
    /// shuffle bag would have said.
    ///
    /// Held here rather than only in the bank because ApplySfx rebuilds the bank
    /// whenever anything about an assignment changes, and held as a filename
    /// because a variant index is a position in a list that the same edit can
    /// renumber. Never saved and never pushed over the link: a pin is a way to
    /// listen, not a decision about the pack, and it lasts exactly as long as
    /// the process.
    ///
    /// Muting used to live here too and does not any more - a mute is a decision
    /// about the pack, so it is in `m_sfx` beside the file list and saves with
    /// it.
    struct SlotSession {
        std::string forced;
    };
    std::array<SlotSession, static_cast<std::size_t>(rds::SlotId::kCount)> m_sfxSession;
    /// Which slot's widget the mouse is over, for the timeline highlight, and
    /// the answer being built for the next frame. Two fields because the left
    /// column draws the timeline *before* the right column draws the widget
    /// that would set it - so the timeline reads last frame's answer, which on
    /// a hover highlight is not something an eye can see.
    int m_hoverSlot{-1};
    int m_hoverSlotPending{-1};
    /// What the browser's highlight is currently being heard as, so the take is
    /// re-mixed on the frame the highlight moves and on no other frame. -1 and
    /// empty when nothing is being auditioned.
    int m_auditionSlot{-1};
    std::string m_auditionFile;
    /// The condition the file about to be chosen in the library will land with.
    ///
    /// `+ variant` asks what the new one is for and then opens the picker, so
    /// the answer is given a window and a frame before the file it belongs to
    /// arrives - which is why this is state rather than an argument. -1 when
    /// the picker was opened by `+ add` or `change`, both of which mean a plain
    /// variant and both of which clear it on the way past, so a popover that
    /// was opened and abandoned cannot tag the next file somebody assigns.
    int m_pendingConditionSlot{-1};
    rds::VariantCondition m_pendingCondition{};
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
    /// resize - and remembered between launches through UiPrefFracFields. A
    /// session is spent in one arrangement, so dragging four bars back into place
    /// at every launch cost more than a wrong remembered split ever did.
    float m_leftFrac{0.55f};    ///< left column against the config panel
    float m_videoFrac{0.42f};   ///< video against everything under it
    float m_tableFrac{0.5f};    ///< impacts against cues
    float m_configFrac{0.62f};  ///< the config panel against the sfx panel under it

    ConfigSide m_side[2];
    bool m_split{};
    int m_focusSide{};

    int m_selectedCue{-1};
    char m_filter[64]{};
    /// While a compare config is loaded, hide every parameter the two agree on.
    /// The panel is ~200 rows and a tuning session moves a dozen; this is the
    /// difference between "what did I change" and scrolling for it.
    bool m_diffOnly{};
    /// The compare report window, and whether it lists the identical rows too.
    bool m_showCompareReport{};
    bool m_compareShowSame{};
    /// Which side the report window is reading. Only meaningful while split.
    int m_compareSide{};
    float m_monitorDb{-3.0f};
    /// The one thing the testbench does that the game cannot. On by default so a
    /// stacked burst does not wrap, but the flag and the pre-limiter peak are
    /// both on screen (00 section 13).
    bool m_limiter{true};

    /// Pretend the take happened somewhere else, in something else.
    ///
    /// Session-only and deliberately not in `m_prefs`: coming back tomorrow to a
    /// testbench that is quietly pretending, and tuning under it, is the worst
    /// thing this control could do. It also belongs to the *take* rather than to
    /// a config, so both A/B sides share it - an A/B compares two configs in one
    /// pretend world, not two worlds.
    rds::OfflineOptions m_pretend;
    bool m_showTrace{true};
    bool m_showContacts{true};
    /// Snap the timeline to the loop region when playback starts.
    ///
    /// It used to rescale the moment a region was drawn, which fought every
    /// other way of moving around the timeline: the wheel would zoom out and the
    /// next frame would put it back. Playback is the one moment where "show me
    /// the bit I selected" is unambiguous, so that is the only time it happens.
    bool m_zoomRegion{true};

    /// Play from just before the first cue to just after the last one.
    ///
    /// Most takes are mostly silence - the ragdoll starts when the recording
    /// does and the body lies still long after the last sound - and listening
    /// to a mix change means hearing the change, not waiting for it. A drawn
    /// region still wins, being the more specific answer to the same question.
    /// Which slots this take has produced a cue for, since it was loaded.
    ///
    /// Sticky, and that is the point. The sfx panel sorts the slots this take
    /// uses above the ones it does not, and asking the live cue list which
    /// those are made the list rearrange itself under the cursor: muting a
    /// slot removes its cues, so the widget just clicked would drop below the
    /// "not in this take" divider and the next click landed on its neighbour.
    ///
    /// A muted slot is still in the take - the cues were chosen and paid for
    /// and then silenced at render, which is the whole reason a mute is an
    /// honest A/B. So presence is remembered per take rather than recomputed
    /// per frame, and it is cleared when a different take is loaded, where it
    /// would be a claim about the wrong recording.
    std::array<bool, static_cast<std::size_t>(rds::SlotId::kCount)> m_slotSeen{};

    /// Slots kept off the timeline, by index. Set from each slot's `timeline`
    /// checkbox in the sfx panel and remembered between launches.
    ///
    /// Hidden rather than shown, so the zero value is the honest one: a fresh
    /// install, a deleted prefs file and a slot added after this was written all
    /// mean "draw it", and none of them need an entry to say so.
    ///
    /// A drawing switch and nothing else. Unlike a mute, which silences a layer
    /// the arbitrator still chose and paid for, this does not reach the engine
    /// at all - the cue list, the table, the export and the sound are identical
    /// with a slot hidden. It exists because the lane is 150 pixels tall and a
    /// take with a long slide spends most of them on scrape envelopes that are
    /// drawn over the impacts they are the context for.
    std::array<bool, static_cast<std::size_t>(rds::SlotId::kCount)> m_slotHidden{};

    bool m_limitToCues{};

    /// Jump the gaps inside a take as well, on the same reasoning.
    ///
    /// A knockdown that lands, goes quiet for four seconds and then slides is
    /// two things worth hearing with a wait in between.
    bool m_skipCueless{};

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
    /// The fixed end of the drag: the point the region grows from. A fresh drag
    /// anchors where the press landed, dragging an edge anchors on the *other*
    /// edge, which is what makes grabbing an edge adjust the region rather than
    /// start a new one.
    double m_regionAnchorMs{};
    /// How the drag on the strip started, decided once on the press.
    ///
    /// The strip carries a region that is usually the thing being adjusted, not
    /// the thing being replaced: you draw one roughly, listen, and then want its
    /// left edge fifty milliseconds earlier. Deciding this from where the press
    /// landed is what turns that into a drag rather than into drawing the whole
    /// region again.
    enum class RegionDrag { kNew, kEdge, kMove };
    RegionDrag m_regionDragKind{RegionDrag::kNew};
    /// For a move: where in the region the grab was, so it slides under the
    /// cursor instead of jumping its start to it, and how long it is - the
    /// length is held for the gesture so clamping at either end cannot shrink it.
    double m_regionGrabOffsetMs{};
    double m_regionGrabSpanMs{};
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
    /// Drop limb-on-limb contacts from the impacts table.
    ///
    /// A ragdoll hits itself constantly - an arm against a thigh, a shin against
    /// the other shin - and those rows are the great majority of a fall while
    /// being almost none of what anybody is listening for. On by default: the
    /// question the table is nearly always being asked is "what did this body
    /// hit the world with", and reading that off a list where four rows in five
    /// are the body hitting itself is the reason the box exists.
    bool m_hideSelfImpacts{true};
    /// Mirrors Player::Loop(). The transport owns the flag, but the preference
    /// file needs somewhere to land before the player exists.
    bool m_autoLoop{true};
    std::uint32_t m_seed{1};

    // ── the benchmark ────────────────────────────────────────────────────────
    //
    // Per side, and kept until the next click rather than cleared when a slider
    // moves: a stale number next to a changed config is exactly what you want
    // on screen while you make the change, because the comparison being made is
    // against what it used to cost.
    BenchResult m_bench[2];
    /// How long the run is allowed to take, per side. The window is frozen for
    /// it (Bench.h says why), so it is on screen and adjustable rather than a
    /// constant somebody has to guess at.
    float m_benchBudgetSec{0.5f};

    std::string m_exportNote;  ///< what the last export did, shown beside the button
    /// Write the whole config, with every description, into the export.
    ///
    /// On, because the failure it prevents is silent: a report that names
    /// `config_22_08_5` and nothing else is unreadable the moment that file is
    /// saved over, and the whole point of an export is that it can be read
    /// later by somebody who was not there. Off when the report is going into a
    /// message and four hundred lines of ini are in the way.
    bool m_exportConfigs{true};
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
    /// The command line's way in - see Control.h. Started alongside the game
    /// link because it needs the same port out of the same file.
    ControlServer m_control;
    rds::GeneralConfig m_general{};
    std::filesystem::path m_generalIni;
    /// Which takes are in the Num4 / Num6 cycle.
    TakeFlags m_takeFlags;

    /// Off stops pushing config at the game without dropping the connection, so
    /// a take can be recorded against what the mod's own inis say rather than
    /// against whatever is on the sliders.
    bool m_pushToGame{true};
    /// Where the wavs behind a take's vanilla track are found.
    VanillaLibrary m_vanillaLibrary;
    /// What the last vanilla mix managed. Shown beside the switch, because "the
    /// vanilla side is silent" has two causes and they need telling apart.
    std::size_t m_vanillaPlayed{};
    std::size_t m_vanillaMisses{};
    /// The value of m_useVanillaAudio the current buffers were mixed for, so a
    /// click on the switch re-renders instead of being noticed only by the game.
    bool m_vanillaAudioApplied{};

    /// On, the connected game puts vanilla's own body impacts back and plays
    /// nothing of ours, and a take plays its recorded vanilla track instead of
    /// our mix - the A/B switch for judging the mix against what it replaces,
    /// inside one session rather than across two launches.
    ///
    /// Deliberately not remembered between launches: it is a thing done for the
    /// length of a comparison, and a testbench that started with the mod muted
    /// would be a morning spent wondering why the game went quiet.
    bool m_useVanillaAudio{};
    /// Whether the game has been told the above since it connected. Its own flag
    /// rather than m_pushedValid's, because the switch travels even when the
    /// sliders are not being pushed.
    bool m_pushedAudioMode{};
    bool m_pushedVanillaAudio{};
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
    /// The take's sync track as it accumulates, on the game's clock. Rebased onto
    /// the take's own clock and written out beside the CSV when it is written.
    std::vector<SyncSample> m_recordSync;
    std::chrono::steady_clock::time_point m_lastClockSample{};

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
