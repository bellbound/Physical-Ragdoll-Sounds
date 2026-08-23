#include "App.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>

#include <spdlog/spdlog.h>

#include "imgui.h"

#include "rds/ConfigManager.h"
#include "rds/ConfigSchema.h"
#include "rds/Engine.h"
#include "rds/Ini.h"

namespace fs = std::filesystem;

namespace tb {
namespace {

/// A draggable divider that edits `frac` in place.
///
/// ImGui has no splitter widget: the idiom is an invisible button you read the
/// drag delta from, which is all this is. `vertical` describes the *bar*, so a
/// vertical bar moves a left/right split.
bool Splitter(const char* id, bool vertical, float& frac, float span, float minFrac,
              float maxFrac) {
    constexpr float kThickness = 6.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    ImGui::InvisibleButton(id, vertical ? ImVec2(kThickness, ImGui::GetContentRegionAvail().y)
                                        : ImVec2(ImGui::GetContentRegionAvail().x, kThickness));
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    if (hovered || active) {
        ImGui::SetMouseCursor(vertical ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
    }
    bool moved = false;
    if (active && span > 1.0f) {
        const float delta = vertical ? ImGui::GetIO().MouseDelta.x : ImGui::GetIO().MouseDelta.y;
        if (delta != 0.0f) {
            frac = std::clamp(frac + delta / span, minFrac, maxFrac);
            moved = true;
        }
    }
    // Drawn rather than left blank: an invisible handle is one nobody finds.
    const ImVec2 a = ImGui::GetItemRectMin();
    const ImVec2 b = ImGui::GetItemRectMax();
    ImGui::GetWindowDrawList()->AddRectFilled(
        a, b, ImGui::GetColorU32(active    ? ImGuiCol_SeparatorActive
                                 : hovered ? ImGuiCol_SeparatorHovered
                                           : ImGuiCol_Separator));
    ImGui::PopStyleVar();
    return moved;
}

/// Order `index` by whatever the table's header was last clicked on.
///
/// `key` returns the sortable value of one row for one column, as a double -
/// every column here is a number, a time or a short enum, and collapsing them to
/// one comparable type keeps the call sites to a single line each.
template <typename KeyFn>
void ApplySort(const ImGuiTableSortSpecs* specs, std::vector<int>& index, KeyFn key) {
    if (specs == nullptr || specs->SpecsCount == 0) {
        return;
    }
    const ImGuiTableColumnSortSpecs& spec = specs->Specs[0];
    const bool ascending = spec.SortDirection == ImGuiSortDirection_Ascending;
    std::stable_sort(index.begin(), index.end(), [&](int a, int b) {
        const double ka = key(a, spec.ColumnUserID);
        const double kb = key(b, spec.ColumnUserID);
        return ascending ? ka < kb : kb < ka;
    });
}

ImU32 ReasonColour(rds::CueReason r) {
    switch (r) {
        case rds::CueReason::kImpactComposite: return IM_COL32(255, 176, 64, 255);
        case rds::CueReason::kSurfaceSkin:     return IM_COL32(196, 150, 96, 255);
        case rds::CueReason::kHeadImpact:      return IM_COL32(255, 96, 96, 255);
        case rds::CueReason::kCrunch:          return IM_COL32(236, 88, 176, 255);
        case rds::CueReason::kGore:            return IM_COL32(190, 40, 60, 255);
        case rds::CueReason::kLimbTap:         return IM_COL32(150, 205, 255, 255);
        case rds::CueReason::kScrape:          return IM_COL32(120, 220, 170, 255);
        case rds::CueReason::kFoleyBed:        return IM_COL32(120, 140, 180, 255);
        case rds::CueReason::kAirborneRise:    return IM_COL32(150, 120, 220, 255);
        case rds::CueReason::kSettleClose:     return IM_COL32(230, 230, 120, 255);
    }
    return IM_COL32(200, 200, 200, 255);
}

/// dB to a 0..1 bar height. The references' usable span is about 35 dB, so the
/// timeline shows that span rather than a full 60 - otherwise every cue in a
/// well-tuned mix draws at the same height.
float BarHeight(float gainDb) {
    return std::clamp((gainDb + 40.0f) / 40.0f, 0.03f, 1.0f);
}

void Tip(std::string_view text) {
    if (!text.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(420.0f);
        ImGui::TextUnformatted(text.data(), text.data() + text.size());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

}  // namespace

// ═════════════════════════════════════════════════════════════════════════════
// setup
// ═════════════════════════════════════════════════════════════════════════════

bool App::Init(const Paths& paths) {
    m_paths = paths;

    std::error_code ec;
    fs::create_directories(m_paths.configs, ec);
    fs::create_directories(m_paths.frameCache, ec);

    // The bank is filled from the library and the assignments, so this has to
    // come before anything asks it what a slot plays. LoadSfx ends in
    // ApplySfx, which loads the bank and re-runs both sides.
    m_sources.SetBank(&m_bank, 48000);

    m_offsets.Load(m_paths.frameCache / "video-offsets.ini");
    m_takeFlags.Load(m_paths.frameCache / "recording-flags.ini");

    // Before anything is drawn and before the link starts: the video mode and
    // the push-to-game switch are both read on the way up.
    LoadUiPrefs();

    // The link, OBS and the port they use all come out of the deployed mod's own
    // RagdollSounds.ini - see StartLink.
    StartLink();

    ScanRecordings();
    ScanConfigs();

    if (!m_player.Start(48000)) m_startupError = m_player.LastError();
    m_player.SetLoop(m_autoLoop);

    for (ConfigSide& s : m_side) {
        s.cfg = rds::AlgorithmConfig{};
        s.cfg.slots.rngSeed = m_seed;  // fixed, so an A/B compares configs
        std::snprintf(s.saveName, sizeof(s.saveName), "%s", NextConfigName().c_str());
    }

    LoadSfx();

    if (!m_takes.empty()) SelectRecording(0);

    // Start on the most recently written config rather than on defaults. The
    // last one saved is nearly always the one being worked on, and starting
    // from defaults meant loading it by hand at every launch - and hearing the
    // first take through the wrong settings until you remembered to.
    //
    // By write time, not by name: the names carry a date but they are also
    // hand-edited, and the file system knows which was touched last.
    if (!m_configFiles.empty()) {
        int newest = 0;
        fs::file_time_type best{};
        for (std::size_t i = 0; i < m_configFiles.size(); ++i) {
            std::error_code te;
            const auto when = fs::last_write_time(m_configFiles[i], te);
            if (te) continue;
            if (i == 0 || when > best) {
                best = when;
                newest = static_cast<int>(i);
            }
        }
        m_configIndex = newest;
        LoadConfigFile(0, m_configFiles[static_cast<std::size_t>(newest)]);
    }

    // Last, and through SetSplit rather than by assignment: side B is built as a
    // copy of side A, so restoring the split before there is a config on A would
    // remember the mode and lose what it was meant to be comparing.
    if (m_split) {
        m_split = false;
        SetSplit(true);
    }
    return true;
}

// ══════════════════════════════════════════════════════════════════════════════
// the switches that are not config
// ══════════════════════════════════════════════════════════════════════════════

std::vector<std::pair<std::string_view, bool*>> App::UiPrefFields() {
    return {
        {"bLimiter", &m_limiter},
        {"bShowTrace", &m_showTrace},
        {"bShowContacts", &m_showContacts},
        {"bSnapOnPlay", &m_zoomRegion},
        {"bFollowImpacts", &m_followImpacts},
        {"bFollowCues", &m_followCues},
        {"bSaveAsIteration", &m_saveAsIteration},
        {"bPushToGame", &m_pushToGame},
        {"bVideoSync", &m_videoSync},
        {"bSplitAB", &m_split},
        {"bAutoLoop", &m_autoLoop},
    };
}

void App::LoadUiPrefs() {
    m_uiPrefsFile = m_paths.frameCache / "ui-state.ini";

    const auto fields = UiPrefFields();
    for (const std::string& line : rds::ini::ReadLines(m_uiPrefsFile)) {
        std::string_view key;
        std::string_view value;
        if (!rds::ini::SplitAssignment(line, key, value)) continue;
        for (const auto& [name, member] : fields) {
            if (rds::ini::EqualsIgnoreCase(name, key)) {
                *member = value != "0";
                break;
            }
        }
    }

    // Empty rather than the text just read, so the first frame writes the file
    // whether or not anything was clicked. That is what creates it on a machine
    // that has never had one, and what adds a switch to a file written before
    // that switch existed.
    m_uiPrefsWritten.clear();
}

void App::SaveUiPrefs() {
    if (m_uiPrefsFile.empty()) return;

    std::string text =
        "; Physical Ragdoll Sounds testbench - the UI switches, remembered between launches.\n"
        "; Nothing here changes what the mod does. Delete the file to go back to defaults.\n";
    for (const auto& [key, member] : UiPrefFields()) {
        text += std::format("{}={}\n", key, *member ? 1 : 0);
    }
    if (text == m_uiPrefsWritten) return;

    // Recorded whether or not the write landed. A disk that will not take this
    // file will not take it next frame either, and retrying sixty times a second
    // turns one unwritable preference into an unreadable log.
    m_uiPrefsWritten = text;
    rds::ini::WriteFile(m_uiPrefsFile, text);
}

void App::ScanRecordings() {
    m_takes.clear();
    m_takeCsv.clear();
    if (!fs::exists(m_paths.recordings)) {
        m_recordingError = "recordings folder not found: " + m_paths.recordings.string();
        return;
    }
    m_takes = rds::Recording::Scan(m_paths.recordings);
    for (const rds::RecordingInfo& info : m_takes)
        m_takeCsv.push_back(m_paths.recordings / (info.stem + ".csv"));
}

void App::ScanConfigs() {
    m_configFiles.clear();
    std::error_code ec;
    if (!fs::exists(m_paths.configs, ec)) return;
    for (const fs::directory_entry& e : fs::directory_iterator(m_paths.configs, ec))
        if (e.path().extension() == ".ini") m_configFiles.push_back(e.path());
    std::sort(m_configFiles.begin(), m_configFiles.end());
}

std::string App::NextConfigName() const {
    // Today's date, not a hardcoded one. The names are `config_<dd>_<mm>_<n>`
    // and the date half is there to say when a session happened; a fixed 22_08
    // said it happened on the day this was written.
    const auto now = std::chrono::system_clock::now();
    const auto today = std::chrono::year_month_day{std::chrono::floor<std::chrono::days>(now)};
    const std::string stem =
        std::format("config_{:02}_{:02}", static_cast<unsigned>(today.day()),
                    static_cast<unsigned>(today.month()));
    for (int n = 1; n < 999; ++n) {
        const std::string name = std::format("{}_{}", stem, n);
        if (!fs::exists(m_paths.configs / (name + ".ini"))) return name;
    }
    return stem + "_x";
}

std::string App::NextIteration(std::string_view name) const {
    // `config_22_08_4` -> `config_22_08_5`, and keep counting past anything that
    // already exists so a bumped name never lands on a file somebody else in
    // this session wrote.
    std::string stem(name);
    int number = 1;
    if (const auto underscore = stem.find_last_of('_'); underscore != std::string::npos) {
        const std::string tail = stem.substr(underscore + 1);
        if (!tail.empty() && std::ranges::all_of(tail, [](unsigned char c) {
                return std::isdigit(c) != 0;
            })) {
            number = std::atoi(tail.c_str());
            stem = stem.substr(0, underscore);
        }
    }
    for (int n = number + 1; n < number + 999; ++n) {
        const std::string candidate = std::format("{}_{}", stem, n);
        if (!fs::exists(m_paths.configs / (candidate + ".ini"))) return candidate;
    }
    return NextConfigName();
}

void App::SelectRecording(int index) {
    if (m_takes.empty()) return;
    index = std::clamp(index, 0, static_cast<int>(m_takes.size()) - 1);
    m_take = index;
    m_selectedCue = -1;
    m_recordingError.clear();

    m_recording = std::make_unique<rds::Recording>();
    std::string error;
    if (!m_recording->Load(m_takeCsv[static_cast<std::size_t>(index)], error)) {
        m_recordingError = error;
        m_recording.reset();
        return;
    }

    const rds::RecordingInfo& info = m_recording->Info();
    m_sync = FitSync(m_paths.recordings / (info.stem + "_sync.csv"));
    m_video.Open(info.stem, info.videoPath, m_paths.frameCache, VideoMode());

    // The clip's cut point is recorded nowhere, so centre the take in the clip
    // and let the user nudge from there. The fit gives us the drift (the slope);
    // the intercept it produces is against the *uncut* OBS recording and is
    // useless against a cut file.
    m_videoOffsetKnown = m_offsets.Has(info.stem);
    m_videoOffsetMs = m_offsets.Get(info.stem, 0.0);

    m_player.SeekMs(0.0);
    // A different take is a new baseline, not an edit: the old take's regions
    // mean nothing here, so they leave rather than sitting on the undo stack.
    m_player.SetLoopRegion(0.0, 0.0);
    m_regionDragging = false;
    m_regionUndo.clear();
    m_regionRedo.clear();
    // And neither the view nor the selection means anything against a different
    // set of events.
    m_viewValid = false;
    m_hasSelection = false;
    m_selecting = false;
    m_maybeSelecting = false;
    m_selectionNote.clear();
    m_videoNote.clear();
    for (int side = 0; side < 2; ++side) {
        m_side[side].dirty = true;
        m_side[side].verifyRun = false;
    }
    RerunDirty();
}

void App::SelectRecordingByName(const std::string& match) {
    for (int i = 0; i < static_cast<int>(m_takes.size()); ++i)
        if (m_takes[static_cast<std::size_t>(i)].stem.find(match) != std::string::npos) {
            SelectRecording(i);
            return;
        }
}

void App::StepRecording(int delta) {
    if (m_takes.empty()) return;
    const int n = static_cast<int>(m_takes.size());
    // Skip whatever the recording manager has switched off. A folder collects
    // dozens of takes and most of them are one bad shove; walking past them
    // every time is what the checkbox exists to stop.
    //
    // Bounded by n so a folder with everything disabled steps once and stops
    // rather than spinning.
    for (int step = 1; step <= n; ++step) {
        const int candidate = ((m_take + delta * step) % n + n) % n;
        if (m_takeFlags.Enabled(m_takes[static_cast<std::size_t>(candidate)].stem)) {
            SelectRecording(candidate);
            return;
        }
    }
}

void App::StepConfig(int delta) {
    if (m_configFiles.empty()) return;
    const int n = static_cast<int>(m_configFiles.size());
    m_configIndex = ((m_configIndex + delta) % n + n) % n;
    LoadConfigFile(m_focusSide, m_configFiles[static_cast<std::size_t>(m_configIndex)]);
}

void App::LoadConfigFile(int side, const fs::path& file) {
    ConfigSide& s = m_side[side];
    s.cfg = rds::AlgorithmConfig{};
    rds::ConfigManager::LoadInto(file, &s.cfg, rds::AlgorithmParams());
    s.cfg.slots.rngSeed = m_seed;
    s.name = file.stem().string();
    std::snprintf(s.saveName, sizeof(s.saveName), "%s", s.name.c_str());
    s.dirty = true;
    s.unsaved = false;
    s.verifyRun = false;
    ClearHistory(side);  // a loaded file is a new baseline, not an edit
    Rerun(side);
}

void App::SaveConfigFile(int side) {
    ConfigSide& s = m_side[side];
    std::string name = s.saveName;
    if (name.empty()) name = NextConfigName();
    // A save is a new iteration by default: the reason to save mid-session is
    // nearly always that the *last* one was worth keeping too, and overwriting
    // it is the one mistake this program cannot undo. Off in one click when
    // what you meant was "fix that one".
    if (m_saveAsIteration && fs::exists(m_paths.configs / (name + ".ini"))) {
        name = NextIteration(name);
        std::snprintf(s.saveName, sizeof(s.saveName), "%s", name.c_str());
    }
    const fs::path file = m_paths.configs / (name + ".ini");
    rds::ConfigManager::SaveFrom(file, &s.cfg, rds::AlgorithmParams(),
                                 "RagdollSounds_Algorithm.ini - written by the testbench");
    s.name = name;
    s.unsaved = false;
    ScanConfigs();
    for (std::size_t i = 0; i < m_configFiles.size(); ++i)
        if (m_configFiles[i] == file) m_configIndex = static_cast<int>(i);
}

bool App::AnythingUnsaved() const {
    if (!(m_sfx == m_sfxSaved)) return true;
    if (m_browser.Dirty()) return true;
    if (m_side[0].unsaved) return true;
    return m_split && m_side[1].unsaved;
}

void App::SaveEverything() {
    // Everything that has a file behind it and differs from what is in it. One
    // key, because "which of the four things I changed is unsaved" is not a
    // question anybody should have to hold in their head.
    std::vector<std::string> wrote;
    for (int side = 0; side < (m_split ? 2 : 1); ++side) {
        if (!m_side[side].unsaved) continue;
        SaveConfigFile(side);
        wrote.push_back(m_side[side].name);
    }
    if (!(m_sfx == m_sfxSaved)) {
        SaveSfx();
        wrote.emplace_back(m_paths.sfxIni.filename().string());
    }
    if (m_browser.Dirty()) {
        const std::size_t count = m_browser.DirtyCount();
        m_browser.Save();
        wrote.push_back(std::format("{} sfx metadata", count));
    }

    if (wrote.empty()) {
        m_saveNote = "nothing to save";
        return;
    }
    m_saveNote = "saved " + wrote.front();
    for (std::size_t i = 1; i < wrote.size(); ++i) m_saveNote += ", " + wrote[i];
    spdlog::info("{}", m_saveNote);
}

// ═════════════════════════════════════════════════════════════════════════════
// the loop that makes a slider audible
// ═════════════════════════════════════════════════════════════════════════════

void App::Rerun(int side) {
    ConfigSide& s = m_side[side];
    s.dirty = false;
    if (!m_recording) {
        s.audio.reset();
        return;
    }

    const auto t0 = std::chrono::steady_clock::now();

    rds::OfflineOptions opt;
    opt.seed = m_seed;
    opt.trace = true;

    m_bank.Seed(m_seed);
    m_recording->Rewind();
    s.result = rds::RunOffline(*m_recording, s.cfg, m_bank, opt);

    s.audio = std::make_shared<MixedAudio>(MixCues(s.result.cues, s.result.audioDurationMs,
                                                   m_recording->Listener(), m_sources, 48000, m_monitorDb,
                                                   m_limiter));
    s.runMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

    // The swap is at the current play position: no restart, no gap.
    m_player.SetBuffer(side, s.audio);
}

void App::RerunDirty() {
    for (int side = 0; side < 2; ++side)
        if (m_side[side].dirty) Rerun(side);
}

// ══════════════════════════════════════════════════════════════════════════════
// undo of the unsaved edits
//
// Tuning by ear means moving a slider until it is wrong and then wanting the
// last value back, which is a value nobody wrote down. One step is one gesture:
// the config is snapshotted when a widget first moves and the step is pushed
// when the widget is released, so a drag is one entry rather than one per frame.
// ══════════════════════════════════════════════════════════════════════════════

namespace {
/// How many gestures a side remembers. Long enough for a tuning session, short
/// enough that the stacks stay a rounding error next to the mixed audio.
constexpr std::size_t kUndoDepth = 128;
}  // namespace

bool App::SameConfig(const rds::AlgorithmConfig& a, const rds::AlgorithmConfig& b) {
    // Field by field through the schema rather than memcmp: the structs have
    // padding, and two copies of "the same config" must not differ because of it.
    for (const rds::ParamDesc& p : rds::AlgorithmParams())
        if (rds::GetParam(&a, p) != rds::GetParam(&b, p)) return false;
    return true;
}

void App::BeginEdit(int side, std::string_view label) {
    ConfigSide& s = m_side[side];
    if (s.editOpen) return;  // same gesture, still running
    s.editBase = s.cfg;
    s.editLabel = std::string(label);
    s.editOpen = true;
}

void App::CommitEdit(int side) {
    ConfigSide& s = m_side[side];
    if (!s.editOpen) return;
    s.editOpen = false;
    // A drag that ends where it started is not a step.
    if (SameConfig(s.editBase, s.cfg)) return;
    PushEdit(side, s.editBase, s.editLabel);
}

void App::PushEdit(int side, const rds::AlgorithmConfig& before, std::string_view label) {
    ConfigSide& s = m_side[side];
    if (SameConfig(before, s.cfg)) return;
    s.undo.push_back({before, std::string(label), ++m_editSeq});
    if (s.undo.size() > kUndoDepth) s.undo.erase(s.undo.begin());
    s.unsaved = true;
    s.redo.clear();  // a new edit is a new branch
    m_regionRedo.clear();
    m_sfxRedo.clear();
}

void App::Undo(int side) {
    ConfigSide& s = m_side[side];
    CommitEdit(side);  // an in-flight drag becomes a step before it is undone
    if (s.undo.empty()) return;
    const ConfigEdit step = s.undo.back();
    s.undo.pop_back();
    s.redo.push_back({s.cfg, step.label, ++m_editSeq});
    s.cfg = step.cfg;
    s.cfg.slots.rngSeed = m_seed;  // the seed is the testbench's, not the config's
    s.dirty = true;
    s.verifyRun = false;
}

void App::Redo(int side) {
    ConfigSide& s = m_side[side];
    CommitEdit(side);
    if (s.redo.empty()) return;
    const ConfigEdit step = s.redo.back();
    s.redo.pop_back();
    s.undo.push_back({s.cfg, step.label, ++m_editSeq});
    s.cfg = step.cfg;
    s.cfg.slots.rngSeed = m_seed;
    s.dirty = true;
    s.verifyRun = false;
}

void App::SetRegion(double startMs, double endMs, std::string_view label) {
    const double prevStart = m_player.LoopStartMs();
    const double prevEnd = m_player.LoopEndMs();
    if (std::fabs(prevStart - startMs) < 0.5 && std::fabs(prevEnd - endMs) < 0.5) return;

    m_regionUndo.push_back({prevStart, prevEnd, std::string(label), ++m_editSeq});
    if (m_regionUndo.size() > kUndoDepth) m_regionUndo.erase(m_regionUndo.begin());
    // One history: a new step of any kind ends the branch you could redo.
    m_regionRedo.clear();
    m_sfxRedo.clear();
    m_side[0].redo.clear();
    m_side[1].redo.clear();
    m_player.SetLoopRegion(startMs, endMs);
}

void App::UndoRegion() {
    if (m_regionUndo.empty()) return;
    const RegionEdit step = m_regionUndo.back();
    m_regionUndo.pop_back();
    m_regionRedo.push_back(
        {m_player.LoopStartMs(), m_player.LoopEndMs(), step.label, ++m_editSeq});
    m_player.SetLoopRegion(step.startMs, step.endMs);
}

void App::RedoRegion() {
    if (m_regionRedo.empty()) return;
    const RegionEdit step = m_regionRedo.back();
    m_regionRedo.pop_back();
    m_regionUndo.push_back(
        {m_player.LoopStartMs(), m_player.LoopEndMs(), step.label, ++m_editSeq});
    m_player.SetLoopRegion(step.startMs, step.endMs);
}

void App::UndoLatest() {
    CommitEdit(m_focusSide);  // an in-flight drag becomes a step before it is undone
    const ConfigSide& s = m_side[m_focusSide];
    const std::uint64_t cfgSeq = s.undo.empty() ? 0 : s.undo.back().seq;
    const std::uint64_t regSeq = m_regionUndo.empty() ? 0 : m_regionUndo.back().seq;
    const std::uint64_t sfxSeq = m_sfxUndo.empty() ? 0 : m_sfxUndo.back().seq;
    const std::uint64_t newest = std::max({cfgSeq, regSeq, sfxSeq});
    if (newest == 0) return;
    if (newest == sfxSeq) {
        UndoSfx();
    } else if (newest == regSeq) {
        UndoRegion();
    } else {
        Undo(m_focusSide);
    }
}

void App::RedoLatest() {
    CommitEdit(m_focusSide);
    const ConfigSide& s = m_side[m_focusSide];
    // The stacks carry the sequence of the *undo* that pushed them, so the
    // newer top is the thing most recently undone - the one to put back first.
    const std::uint64_t cfgSeq = s.redo.empty() ? 0 : s.redo.back().seq;
    const std::uint64_t regSeq = m_regionRedo.empty() ? 0 : m_regionRedo.back().seq;
    const std::uint64_t sfxSeq = m_sfxRedo.empty() ? 0 : m_sfxRedo.back().seq;
    const std::uint64_t newest = std::max({cfgSeq, regSeq, sfxSeq});
    if (newest == 0) return;
    if (newest == sfxSeq) {
        RedoSfx();
    } else if (newest == regSeq) {
        RedoRegion();
    } else {
        Redo(m_focusSide);
    }
}

void App::ClearHistory(int side) {
    ConfigSide& s = m_side[side];
    s.undo.clear();
    s.redo.clear();
    s.editOpen = false;
    s.editLabel.clear();
}

void App::SetSplit(bool on) {
    if (on == m_split) return;
    m_split = on;
    if (on) {
        m_side[1].cfg = m_side[0].cfg;
        m_side[1].name = m_side[0].name + " (copy)";
        std::snprintf(m_side[1].saveName, sizeof(m_side[1].saveName), "%s", NextConfigName().c_str());
        m_side[1].dirty = true;
        m_side[1].verifyRun = false;
        ClearHistory(1);
        Rerun(1);
    }
    m_player.SetSplit(on);
    if (!on) m_focusSide = 0;
}

void App::CollapseOnto(int side) {
    if (side == 1) {
        m_side[0] = m_side[1];
        m_player.SetBuffer(0, m_side[0].audio);
    }
    SetSplit(false);
    m_player.SetActiveSide(0);
}

double App::VideoTimeMs(double takeMs) const {
    // The slope is the fit's contribution - the two clocks drift over a take and
    // a single number cannot follow that. The offset is the cut point, which is
    // the user's to nudge because nothing recorded it.
    return (m_sync.valid ? m_sync.slope : 1.0) * takeMs + m_videoOffsetMs;
}

// ═════════════════════════════════════════════════════════════════════════════
// ui
// ═════════════════════════════════════════════════════════════════════════════

void App::Draw() {
    HandleKeys();
    RerunDirty();

    // OBS answers on its own threads; this is where those answers land on ours.
    // Before anything reads the recording state, so a take that finished while
    // the last frame was drawing is finished by the time the button is drawn.
    obs::Pump();
    SyncToGame();
    // Last frame's answer, and a fresh slate for this one. Recomputed rather
    // than cleared on mouse-out: a widget that stops being drawn - filtered
    // away, scrolled off - would otherwise leave the timeline lit up for a slot
    // nobody is pointing at.
    m_hoverSlot = m_hoverSlotPending;
    m_hoverSlotPending = -1;

    // The clip's length only exists once ffmpeg has finished, so the first
    // guess at the cut point can only be made here.
    //
    // QuickModMenuNG cuts each take out of the continuous OBS recording with
    // kClipPadMs = 2000 either side (CaptureBatch.cpp), so in theory this is a
    // flat 2000 on every take. In practice it is not: the cut uses `-ss` before
    // `-i`, which seeks to the preceding keyframe, and OBS writes one about
    // every 2 s - so each clip carries up to one extra keyframe interval of
    // lead-in, quantised differently per take. Measured, the real offsets run
    // 3044-3683. The clip *length* is unaffected, which is why this hid.
    //
    // So: the intended value is the fallback, and framecache/video-offsets.ini
    // carries the measured per-take numbers that actually line up. Anything in
    // that file wins over this.
    //
    // All of which assumes the take *is* a cut clip. Vayne_impacts_log_2 is not:
    // it is 103 s of uncut OBS output carrying seventeen knockdowns, and the pad
    // guess put it two seconds out. A clip is longer than its take by twice the
    // pad; an uncut recording is not, so the video being no longer than the take
    // is the tell. There the sync CSV means what it says and its own intercept is
    // the right starting point - measured against the first knockdown, that lands
    // within ~200 ms where the pad guess is 2000 ms out.
    constexpr double kClipPadMs = 2000.0;
    if (!m_videoOffsetKnown && m_video.HasVideo() && m_video.Ready() && m_video.FrameCount() > 0 &&
        m_take >= 0) {
        const double takeMs = m_recording ? m_recording->Info().durationMs : 0.0;
        const bool looksCut = m_video.DurationMs() > takeMs + kClipPadMs;
        m_videoOffsetMs = looksCut || !m_sync.valid ? kClipPadMs : m_sync.intercept;
        spdlog::info("{}: video {:.0f} ms against take {:.0f} ms - treating as {}, offset {:+.0f} ms",
                     m_take >= 0 ? m_takes[static_cast<std::size_t>(m_take)].stem : std::string(),
                     m_video.DurationMs(), takeMs, looksCut ? "a cut clip" : "uncut",
                     m_videoOffsetMs);
        m_videoOffsetKnown = true;  // not persisted: the user's first nudge is what saves it
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("Ragdoll Sounds Testbench", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus);

    DrawTopBar();
    ImGui::Separator();

    const float avail = ImGui::GetContentRegionAvail().x;
    const float leftWidth = std::max(360.0f, avail * m_leftFrac);

    ImGui::BeginChild("left", ImVec2(leftWidth, 0), ImGuiChildFlags_None);
    DrawLeft(leftWidth);
    ImGui::EndChild();

    ImGui::SameLine();
    Splitter("##vsplit", true, m_leftFrac, avail, 0.25f, 0.8f);
    ImGui::SameLine();

    // Read once: "continue with this config" collapses the split, and a panel
    // that vanishes half way down a frame is an ImGui stack mismatch.
    const bool split = m_split;
    ImGui::BeginChild("right", ImVec2(0, 0), ImGuiChildFlags_None);
    {
        // The column is the algorithm config over the sfx assignments, split by
        // a bar you can drag. Two panels rather than one scrolling list because
        // they are answers to different questions - "how loud, how often" above,
        // "which sound" below - and you read one while changing the other.
        const float total = ImGui::GetContentRegionAvail().y;
        const float configH = std::clamp(total * m_configFrac, 140.0f, std::max(160.0f, total - 120.0f));

        ImGui::BeginChild("configarea", ImVec2(0, configH), ImGuiChildFlags_None);
        if (split) {
            const float h = ImGui::GetContentRegionAvail().y * 0.5f - 4.0f;
            DrawRight(0, h, true);
            DrawRight(1, 0, true);
        } else {
            DrawRight(0, 0, false);
        }
        ImGui::EndChild();

        Splitter("##sfxsplit", false, m_configFrac, total, 0.2f, 0.85f);
        DrawSfxPanel(0.0f);
    }
    ImGui::EndChild();

    ImGui::End();

    DrawOptions();
    DrawRecordingManager();

    // Outside the main window: it is a real floating window, so it can be moved
    // off the app and left open beside it while a slot is auditioned.
    const SfxBrowser::Pick pick = m_browser.Draw();
    if (m_browser.TakeLibraryChanged()) {
        // New files in the library. If nothing is assigned yet this is the
        // adopt, and the names it just brought in are the assignment - so seed
        // from them rather than leaving a full library and an empty panel.
        if (m_sfx.Empty()) {
            m_sfx.SeedFromNames(m_library);
            m_sfxSaved = rds::SfxAssignments{};  // seeded is not saved
        }
        ApplySfx();
    }
    if (pick.made) {
        const rds::SfxAssignments before = m_sfx;
        rds::SlotAssignment& assignment = m_sfx.For(pick.slot);
        const std::string_view name = rds::Slot(pick.slot).name;
        if (pick.variant >= 0 && pick.variant < static_cast<int>(assignment.files.size())) {
            assignment.files[static_cast<std::size_t>(pick.variant)] = pick.file;
            PushSfxEdit(before, std::string(name) + " / change");
        } else {
            assignment.files.push_back(pick.file);
            PushSfxEdit(before, std::string(name) + " / add");
        }
    }

    // Last thing in the frame, so it sees every switch after every widget that
    // could have moved one. Cheaper than wiring a save into eleven call sites,
    // and it cannot be the thing somebody forgets when they add the twelfth.
    SaveUiPrefs();
}

void App::DrawTopBar() {
    if (ImGui::Button("Recordings...")) m_showManager = true;
    Tip("Every take in the folder: what it carries, whether it is in the Num4 / Num6\n"
        "cycle, and the four bulk clean-ups - clear the frame caches, drop the takes\n"
        "whose video was never built, and drop the ones that carry neither video nor\n"
        "an impact.");
    ImGui::SameLine();

    ImGui::SetNextItemWidth(420.0f);
    std::string label = m_take >= 0 ? m_takes[static_cast<std::size_t>(m_take)].stem : "(no recordings)";
    if (ImGui::BeginCombo("##take", label.c_str())) {
        for (int i = 0; i < static_cast<int>(m_takes.size()); ++i) {
            const rds::RecordingInfo& t = m_takes[static_cast<std::size_t>(i)];
            std::string item = t.stem;
            if (!t.videoPath.empty()) item += "  [video]";
            if (!t.complete) item += "  [incomplete]";
            if (ImGui::Selectable(item.c_str(), i == m_take)) SelectRecording(i);
            Tip(t.note);
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("< Num4")) StepRecording(-1);
    ImGui::SameLine();
    if (ImGui::Button("Num6 >")) StepRecording(1);

    if (m_take >= 0) {
        const std::string& stem = m_takes[static_cast<std::size_t>(m_take)].stem;
        ImGui::SameLine();
        bool enabled = m_takeFlags.Enabled(stem);
        if (ImGui::Checkbox("in cycle", &enabled)) m_takeFlags.SetEnabled(stem, enabled);
        Tip("Off takes this one out of the Num4 / Num6 cycle without deleting it. A folder\n"
            "collects dozens of takes and most of them are one bad shove; this is how the\n"
            "handful worth listening to stay one keypress apart.");

        ImGui::SameLine();
        if (ImGui::SmallButton("delete")) ImGui::OpenPopup("##deletetake");
        if (ImGui::BeginPopup("##deletetake")) {
            ImGui::TextUnformatted("Delete this take?");
            ImGui::TextDisabled("%s", stem.c_str());
            ImGui::TextDisabled("Its csv, sidecar, sync track, video and frame cache all go.");
            ImGui::Separator();
            if (ImGui::Button("Delete")) {
                DeleteTakeAt(m_take);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }

    if (m_take >= 0) {
        const rds::RecordingInfo& t = m_takes[static_cast<std::size_t>(m_take)];
        ImGui::SameLine();
        ImGui::TextDisabled("| %s | %s | %.0f ms | %u impacts%s", t.actorName.c_str(), t.cell.c_str(),
                            t.durationMs, t.impacts, t.dropped ? "  DROPPED EVENTS" : "");
        Tip(t.note.empty() ? "no note in the sidecar"
                           : "recording.note - the take's intent, not a description of it:\n" + t.note);
    }

    ImGui::SameLine();
    if (ImGui::Button("SFX library")) m_browser.Open();
    Tip("Everything in the library, what each file measures, and where importing happens.\n"
        "The same window a slot's change button opens, minus the comparison widgets.");

    // The unsaved marker, on the one bar that is always on screen. Nothing here
    // writes behind your back, so this is the only thing standing between an
    // afternoon of tuning and closing the window.
    ImGui::SameLine();
    if (AnythingUnsaved()) {
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.45f, 1.0f), "* unsaved");
        std::string what = "Ctrl+S writes all of it:";
        for (int side = 0; side < (m_split ? 2 : 1); ++side)
            if (m_side[side].unsaved)
                what += std::format("\n  config {} - {}", static_cast<char>('A' + side),
                                    m_side[side].name);
        if (!(m_sfx == m_sfxSaved)) what += "\n  " + m_paths.sfxIni.filename().string();
        if (m_browser.Dirty())
            what += std::format("\n  {} sfx name(s) or note(s)", m_browser.DirtyCount());
        Tip(what);
        ImGui::SameLine();
        if (ImGui::Button("Save all")) SaveEverything();
    } else {
        ImGui::TextDisabled("all saved");
    }
    if (!m_saveNote.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", m_saveNote.c_str());
    }

    ImGui::SameLine();
    const bool exportRegion = WindowIsRegion();
    if (ImGui::Button(exportRegion ? "Export region" : "Export state")) {
        std::string err;
        const auto written = ExportState(err);
        m_exportNote = written.empty() ? ("export failed: " + err)
                                       : ("wrote " + written.filename().string());
    }
    Tip(exportRegion
            ? "Dump the loop region only: its contacts, its cues, every arbitration\n"
              "decision inside it, the phase it was already in, and the video times\n"
              "to look at - plus the config and the whole-take funnel for context.\n"
              "Clear the region to export the whole take."
            : "Dump the take, the config, the sound bank, the funnel, every cue and every\n"
              "arbitration decision to a text file - state changes, contacts and cues\n"
              "merged into one timeline, so \"why is there a sound here\" is answered\n"
              "by reading the line at that time. Written next to the saved configs.\n"
              "Drag a loop region on the timeline and this exports just that region.");
    if (!m_exportNote.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", m_exportNote.c_str());
    }

    // Right-hand end of the bar. Placed by cursor rather than by SameLine so it
    // stays put as the row above it grows and shrinks.
    ImGui::SameLine();
    const float optionsX = ImGui::GetWindowWidth() - 100.0f;
    if (ImGui::GetCursorPosX() < optionsX) ImGui::SetCursorPosX(optionsX);
    if (ImGui::Button("Options", ImVec2(84.0f, 0.0f))) m_showOptions = true;
    Tip("Settings for the whole app: the experimental video sync, and whether the\n"
        "sliders push themselves at a connected game.");

    if (!m_recordingError.empty()) {
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "load failed: %s", m_recordingError.c_str());
    }
    if (!m_startupError.empty()) {
        ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "audio device: %s", m_startupError.c_str());
    }
}

std::filesystem::path App::ExportState(std::string& error) {
    if (m_take < 0 || !m_recording) {
        error = "no recording loaded";
        return {};
    }
    const ConfigSide& side = m_side[m_focusSide];

    ExportRequest req;
    req.recording = m_recording.get();
    req.info = &m_takes[static_cast<std::size_t>(m_take)];
    req.config = &side.cfg;
    req.result = &side.result;
    req.bank = &m_bank;
    req.audio = side.audio.get();
    req.sync = &m_sync;
    req.video = &m_video;
    req.configName = side.name;
    req.videoOffsetMs = m_videoOffsetMs;
    req.playheadMs = m_player.PositionMs();
    req.seed = m_seed;
    req.limiter = m_limiter;
    req.side = m_focusSide;
    req.offsetMeasured = m_videoOffsetKnown;
    // Whatever is on screen is what gets written. With a loop region set the
    // export is that region - its contacts, its cues, its arbitration decisions
    // and the video times to look at - because when a region is set, that
    // region is the thing being asked about, and a whole-take dump buries it.
    WindowMs(req.windowLoMs, req.windowHiMs);
    req.windowIsRegion = WindowIsRegion();

    return WriteExport(req, m_paths.configs.parent_path() / "exports", error);
}

void App::DrawLeft(float width) {
    const float total = ImGui::GetContentRegionAvail().y;
    const float videoH = std::max(120.0f, total * m_videoFrac);
    DrawVideo(videoH);
    DrawTransport();
    Splitter("##hsplit", false, m_videoFrac, total, 0.12f, 0.75f);
    DrawTimeline(150.0f);
    ImGui::Separator();
    DrawStats();
    ImGui::Separator();
    DrawSelectedCue();

    // The two listings, side by side, filling whatever is left. Impacts on the
    // left because that is the input and cues the output, and the question asked
    // here is nearly always "this contact - what became of it".
    const float row = ImGui::GetContentRegionAvail().y;
    if (row > 60.0f) {
        const float rowW = ImGui::GetContentRegionAvail().x;
        const float impactsW = std::max(200.0f, rowW * m_tableFrac);
        ImGui::BeginChild("impacts", ImVec2(impactsW, row), ImGuiChildFlags_Borders);
        DrawImpactsTable();
        ImGui::EndChild();
        ImGui::SameLine();
        Splitter("##tsplit", true, m_tableFrac, rowW, 0.2f, 0.8f);
        ImGui::SameLine();
        ImGui::BeginChild("cuelist", ImVec2(0, row), ImGuiChildFlags_Borders);
        DrawCuesTable();
        ImGui::EndChild();
    }
    (void)width;
}

bool App::WindowIsRegion() const {
    // Mid-drag the answer is whatever it was when the drag began. The region
    // itself follows the mouse so you can see the selection grow, but nothing
    // that *scales* anything is allowed to move until the button comes up:
    // rescaling per frame moved the timeline out from under the cursor, and
    // dragging inside an existing region would first snap the view back out to
    // the whole take - the opposite of narrowing down.
    if (m_regionDragging) return m_frozenIsRegion;
    return m_player.HasRegion();
}

void App::WindowMs(double& lo, double& hi) const {
    if (m_regionDragging) {
        lo = m_frozenLoMs;
        hi = m_frozenHiMs;
        return;
    }
    if (m_player.HasRegion()) {
        lo = m_player.LoopStartMs();
        hi = m_player.LoopEndMs();
        return;
    }
    lo = 0.0;
    hi = std::max(1.0, m_player.DurationMs());
}

void App::FollowPlayhead(const std::vector<double>& times) const {
    if (!m_player.Playing() || times.size() < 2) {
        return;
    }
    // `times` is the sorted-order row times; sorting by anything else makes
    // "the row nearest now" a meaningless place to scroll to, so the callers
    // only pass a list when the table is in time order.
    const double now = m_player.PositionMs();
    std::size_t best = 0;
    double bestDx = 1e18;
    for (std::size_t i = 0; i < times.size(); ++i) {
        const double dx = std::fabs(times[i] - now);
        if (dx < bestDx) {
            bestDx = dx;
            best = i;
        }
    }
    // Rows are uniform height under the clipper, so this is exact without
    // needing the row to have been drawn.
    ImGui::SetScrollY(static_cast<float>(best) * ImGui::GetTextLineHeightWithSpacing() -
                      ImGui::GetContentRegionAvail().y * 0.5f);
}

void App::DrawImpactsTable() {
    if (!m_recording) {
        ImGui::TextDisabled("no take loaded");
        return;
    }
    const auto& events = m_recording->Events();
    double lo = 0.0;
    double hi = 0.0;
    WindowMs(lo, hi);
    std::vector<int> index;
    index.reserve(events.size());
    for (int i = 0; i < static_cast<int>(events.size()); ++i) {
        const rds::FeedEvent& e = events[static_cast<std::size_t>(i)];
        if (e.kind == rds::EventKind::kImpact && e.timeMs >= lo && e.timeMs <= hi) {
            index.push_back(i);
        }
    }
    ImGui::Text("impacts  %zu", index.size());
    ImGui::SameLine();
    if (WindowIsRegion()) {
        ImGui::TextColored(ImVec4(0.55f, 0.75f, 1.0f, 1.0f), "in %.0f-%.0f ms", lo, hi);
    } else {
        ImGui::TextDisabled("(click a header to sort, a row to seek)");
    }
    ImGui::SameLine();
    ImGui::Checkbox("auto scroll##impacts", &m_followImpacts);
    Tip("Scroll to the row nearest the playhead while it is playing. Only in ascending\n"
        "time order: in any other order \"the row nearest now\" is not a place, so\n"
        "sorting by another column parks it until you sort by t again.");

    constexpr auto kFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY |
                            ImGuiTableFlags_Sortable | ImGuiTableFlags_Resizable;
    if (!ImGui::BeginTable("impacttbl", 8, kFlags)) {
        return;
    }
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("t", ImGuiTableColumnFlags_DefaultSort, 0, 0);
    ImGui::TableSetupColumn("limb", ImGuiTableColumnFlags_None, 0, 1);
    ImGui::TableSetupColumn("closing", ImGuiTableColumnFlags_PreferSortDescending, 0, 2);
    ImGui::TableSetupColumn("tangent", ImGuiTableColumnFlags_PreferSortDescending, 0, 3);
    ImGui::TableSetupColumn("body", ImGuiTableColumnFlags_PreferSortDescending, 0, 4);
    ImGui::TableSetupColumn("ang", ImGuiTableColumnFlags_PreferSortDescending, 0, 5);
    ImGui::TableSetupColumn("hit", ImGuiTableColumnFlags_None, 0, 6);
    ImGui::TableSetupColumn("seq", ImGuiTableColumnFlags_None, 0, 7);
    ImGui::TableHeadersRow();

    ApplySort(ImGui::TableGetSortSpecs(), index, [&](int i, ImGuiID col) -> double {
        const rds::FeedEvent& e = events[static_cast<std::size_t>(i)];
        switch (col) {
            case 0: return e.timeMs;
            case 1: return static_cast<double>(e.limbIndex);
            case 2: return e.impactSpeed;
            case 3: return e.tangentSpeed;
            case 4: return e.bodySpeed;
            case 5: return e.angularSpeed;
            case 6: return e.otherLayer == rds::ColLayer::kDeadBip ? 1.0 : 0.0;
            default: return static_cast<double>(e.sourceSeq);
        }
    });

    // Following only makes sense in time order, which is also the only order
    // where "the row nearest now" means anything.
    const ImGuiTableSortSpecs* impactSort = ImGui::TableGetSortSpecs();
    if (m_followImpacts && impactSort && impactSort->SpecsCount > 0 &&
        impactSort->Specs[0].ColumnUserID == 0 &&
        impactSort->Specs[0].SortDirection == ImGuiSortDirection_Ascending) {
        std::vector<double> times;
        times.reserve(index.size());
        for (const int i : index) {
            times.push_back(events[static_cast<std::size_t>(i)].timeMs);
        }
        FollowPlayhead(times);
    }

    const double nowMs = m_player.PositionMs();
    const rds::ActorProfile* profile = nullptr;
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(index.size()));
    while (clipper.Step()) {
        for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
            const int i = index[static_cast<std::size_t>(r)];
            const rds::FeedEvent& e = events[static_cast<std::size_t>(i)];
            if (!profile) profile = m_recording->Profile(e.actorId);
            const rds::LimbInfo* limb = profile ? profile->Limb(e.limbIndex) : nullptr;

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            // Seeking on click is the point of the table: sort by closing speed,
            // click the top row, and the video is at the hardest hit of the take.
            char label[32];
            std::snprintf(label, sizeof(label), "%.0f##i%d", e.timeMs, i);
            // Lit while the playhead is over it, so the table and the video
            // agree about where you are without having to read timestamps.
            const bool atNow = std::fabs(e.timeMs - nowMs) < 40.0;
            if (atNow) {
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(60, 90, 140, 140));
            }
            if (ImGui::Selectable(label, atNow, ImGuiSelectableFlags_SpanAllColumns)) {
                m_player.SeekMs(e.timeMs);
            }
            ImGui::TableNextColumn();
            ImGui::Text("%s", limb ? limb->boneName.c_str() : "?");
            ImGui::TableNextColumn();
            ImGui::Text("%.1f", e.impactSpeed);
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%.1f", e.tangentSpeed);
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%.1f", e.bodySpeed);
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%.1f", e.angularSpeed);
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", e.otherLayer == rds::ColLayer::kDeadBip ? "self" : "world");
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%u", e.sourceSeq);
        }
    }
    ImGui::EndTable();
}

void App::DrawCuesTable() {
    const ConfigSide& s = m_side[m_player.ActiveSide()];
    const auto& cues = s.result.cues;
    double lo = 0.0;
    double hi = 0.0;
    WindowMs(lo, hi);
    std::vector<int> index;
    index.reserve(cues.size());
    for (int i = 0; i < static_cast<int>(cues.size()); ++i) {
        const double t = cues[static_cast<std::size_t>(i)].timeMs;
        if (t >= lo && t <= hi) {
            index.push_back(i);
        }
    }
    ImGui::Text("cues  %zu", index.size());
    ImGui::SameLine();
    ImGui::TextDisabled("(side %c)", 'A' + m_player.ActiveSide());
    if (WindowIsRegion()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.55f, 0.75f, 1.0f, 1.0f), "in %.0f-%.0f ms", lo, hi);
    }
    ImGui::SameLine();
    ImGui::Checkbox("auto scroll##cues", &m_followCues);
    Tip("Scroll to the row nearest the playhead while it is playing. Only in ascending\n"
        "time order: in any other order \"the row nearest now\" is not a place, so\n"
        "sorting by another column parks it until you sort by t again.");

    constexpr auto kFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY |
                            ImGuiTableFlags_Sortable | ImGuiTableFlags_Resizable;
    if (!ImGui::BeginTable("cuetbl", 8, kFlags)) {
        return;
    }
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("t", ImGuiTableColumnFlags_DefaultSort, 0, 0);
    ImGui::TableSetupColumn("slot", ImGuiTableColumnFlags_None, 0, 1);
    ImGui::TableSetupColumn("gain", ImGuiTableColumnFlags_PreferSortDescending, 0, 2);
    ImGui::TableSetupColumn("pitch", ImGuiTableColumnFlags_None, 0, 3);
    ImGui::TableSetupColumn("int", ImGuiTableColumnFlags_PreferSortDescending, 0, 4);
    ImGui::TableSetupColumn("reason", ImGuiTableColumnFlags_None, 0, 5);
    ImGui::TableSetupColumn("phase", ImGuiTableColumnFlags_None, 0, 6);
    ImGui::TableSetupColumn("seq", ImGuiTableColumnFlags_None, 0, 7);
    ImGui::TableHeadersRow();

    ApplySort(ImGui::TableGetSortSpecs(), index, [&](int i, ImGuiID col) -> double {
        const rds::Cue& c = cues[static_cast<std::size_t>(i)];
        switch (col) {
            case 0: return c.timeMs;
            case 1: return static_cast<double>(c.slot);
            case 2: return c.gainDb;
            case 3: return c.pitch;
            case 4: return c.intensity;
            case 5: return static_cast<double>(c.reason);
            case 6: return static_cast<double>(c.phase);
            default: return static_cast<double>(c.sourceSeq);
        }
    });

    const ImGuiTableSortSpecs* cueSort = ImGui::TableGetSortSpecs();
    if (m_followCues && cueSort && cueSort->SpecsCount > 0 &&
        cueSort->Specs[0].ColumnUserID == 0 &&
        cueSort->Specs[0].SortDirection == ImGuiSortDirection_Ascending) {
        std::vector<double> times;
        times.reserve(index.size());
        for (const int i : index) {
            times.push_back(cues[static_cast<std::size_t>(i)].timeMs);
        }
        FollowPlayhead(times);
    }

    const double nowMs = m_player.PositionMs();
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(index.size()));
    while (clipper.Step()) {
        for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
            const int i = index[static_cast<std::size_t>(r)];
            const rds::Cue& c = cues[static_cast<std::size_t>(i)];
            const std::string_view slot = rds::ToString(c.slot);
            const std::string_view reason = rds::ToString(c.reason);
            const std::string_view phase = rds::ToString(c.phase);

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            char label[32];
            std::snprintf(label, sizeof(label), "%.0f##c%d", c.timeMs, i);
            // The same selection the timeline makes, so the provenance block
            // above follows a click here.
            if (std::fabs(c.timeMs - nowMs) < 40.0) {
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(60, 90, 140, 140));
            }
            if (ImGui::Selectable(label, m_selectedCue == i,
                                  ImGuiSelectableFlags_SpanAllColumns)) {
                m_selectedCue = i;
                m_player.SeekMs(c.timeMs);
            }
            ImGui::TableNextColumn();
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ReasonColour(c.reason)), "%.*s",
                               static_cast<int>(slot.size()), slot.data());
            ImGui::TableNextColumn();
            ImGui::Text("%.1f", c.gainDb);
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%.2f", c.pitch);
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%.2f", c.intensity);
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%.*s", static_cast<int>(reason.size()), reason.data());
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%.*s", static_cast<int>(phase.size()), phase.data());
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%u", c.sourceSeq);
        }
    }
    ImGui::EndTable();
}

void App::DrawVideo(float height) {
    ImGui::BeginChild("video", ImVec2(0, height), ImGuiChildFlags_Borders);

    // An unbuilt take - an mp4 with no frame cache behind it - gets a header
    // saying so and the button that fixes it. The picture underneath is real,
    // but it is *not synced*: the sync fit and the per-take nudge are both
    // measured against a built cache, and pretending otherwise would put a hit
    // on the wrong side of the picture and look like a bug in the engine.
    if (m_video.Unbuilt()) {
        ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.45f, 1.0f), "unbuilt take - preview is NOT synced");
        Tip("The video is here and the events are here, but nothing has lined the two up\n"
            "yet. Build the frame cache and the offset, the drift fit and every export\n"
            "start meaning something.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Generate frames")) BuildFrameCacheNow();
        Tip("Decode the clip to frames, then delete the mp4 - the whole point of the cache\n"
            "is that a 200 MB file stops being needed for a 360p timeline. Extract the\n"
            "video first if you want to keep it.");
        if (m_videoSync) {
            ImGui::SameLine();
            ImGui::TextDisabled("(video sync is on, so nothing builds one by itself)");
        }
        if (!m_videoNote.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", m_videoNote.c_str());
        }
    }

    if (!m_video.HasVideo() && m_video.FrameCount() == 0) {
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        ImGui::SetCursorPos(ImVec2(avail.x * 0.5f - 90.0f, avail.y * 0.5f - 8.0f));
        ImGui::TextDisabled("no video for this take");
    } else if (!m_video.Ready()) {
        ImGui::TextDisabled("%s", m_video.Status());
    } else {
        // An unbuilt take is scrubbed by its own clock: there is no measured
        // offset to apply, and applying the guess would be a claim this take
        // cannot support.
        const double vt = m_video.Unbuilt() ? m_player.PositionMs() : VideoTimeMs(m_player.PositionMs());
        const unsigned tex = m_video.TextureAt(vt);
        if (tex) {
            const ImVec2 avail = ImGui::GetContentRegionAvail();
            const float aspect = m_video.Height() > 0
                                     ? static_cast<float>(m_video.Width()) / static_cast<float>(m_video.Height())
                                     : 16.0f / 9.0f;
            float w = avail.x, h = w / aspect;
            if (h > avail.y) {
                h = avail.y;
                w = h * aspect;
            }
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail.x - w) * 0.5f);
            ImGui::Image(static_cast<ImTextureID>(static_cast<std::uintptr_t>(tex)), ImVec2(w, h));
        } else {
            ImGui::TextDisabled("frame out of range (%.0f ms)", vt);
        }
    }
    ImGui::EndChild();
}

void App::DrawTransport() {
    if (ImGui::Button(m_player.Playing() ? "Pause" : "Play (Num5)", ImVec2(110, 0))) m_player.TogglePlay();
    ImGui::SameLine();
    bool loop = m_player.Loop();
    if (ImGui::Checkbox("auto-loop", &loop)) {
        m_autoLoop = loop;
        m_player.SetLoop(loop);
    }
    ImGui::SameLine();
    bool split = m_split;
    if (ImGui::Checkbox("split A/B", &split)) SetSplit(split);
    Tip("Splits the config panel and alternates A -> B -> A on each loop.");
    ImGui::SameLine();
    if (m_player.HasRegion()) {
        if (ImGui::Button("clear region")) SetRegion(0.0, 0.0, "clear region");
        Tip("Also: a plain click on the timeline's bottom strip, or a right double-click\n"
            "anywhere on it. Ctrl+Z puts it back.");
        ImGui::SameLine();
    }
    const float level = m_player.Level();
    ImGui::TextDisabled("%.0f / %.0f ms   side %c   loops %u   out %.1f dBFS", m_player.PositionMs(),
                        m_player.DurationMs(), 'A' + m_player.ActiveSide(), m_player.LoopCount(),
                        20.0f * std::log10(std::max(level, 1e-5f)));

    // The link, on the same row and hard right. It belongs beside the transport
    // rather than in the top bar: what it says is "is the game feeding this, and
    // am I recording it", which is the same question the numbers to its left
    // answer about the take already loaded.
    ImGui::SameLine();
    DrawLinkRow();

    // scrub
    float pos = static_cast<float>(m_player.PositionMs());
    const float dur = static_cast<float>(std::max(1.0, m_player.DurationMs()));
    ImGui::SetNextItemWidth(-260.0f);
    if (ImGui::SliderFloat("##scrub", &pos, 0.0f, dur, "%.0f ms")) m_player.SeekMs(pos);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    if (ImGui::SliderFloat("monitor dB", &m_monitorDb, -30.0f, 12.0f, "%.1f")) {
        m_side[0].dirty = true;
        m_side[1].dirty = true;
    }
    Tip("Audition level only. This is not mix.masterGainDb - it never leaves the testbench.");
    ImGui::SameLine();
    if (ImGui::Checkbox("limiter", &m_limiter)) {
        m_side[0].dirty = true;
        m_side[1].dirty = true;
    }
    Tip("NOT SHIPPABLE. The game has no bus and no limiter (00 section 13), so this soft clip is "
        "the one thing the testbench does that Skyrim cannot. Turn it off to hear what the mix "
        "really does when a burst stacks.");

    if (m_video.HasVideo()) {
        float nudge = static_cast<float>(m_videoOffsetMs);
        ImGui::SetNextItemWidth(260.0f);
        if (ImGui::SliderFloat("video offset ms", &nudge, -6000.0f, 6000.0f, "%.0f")) {
            m_videoOffsetMs = nudge;
            if (m_take >= 0) m_offsets.Set(m_takes[static_cast<std::size_t>(m_take)].stem, m_videoOffsetMs);
        }
        Tip("video_time_ms = slope * t_ms + offset.\n"
            "The slope comes from fitting the low-rtt rows of the take's _sync.csv.\n"
            "The offset is the clip's cut point inside the longer OBS recording, which\n"
            "is recorded nowhere - so it is yours to set, and it persists per take.");
        ImGui::SameLine();
        if (ImGui::SmallButton("centre") && m_video.Ready() && m_take >= 0) {
            // Assume the clip brackets the take evenly. A starting point, not a
            // measurement - nothing in the capture records where the cut fell.
            m_videoOffsetMs =
                (m_video.DurationMs() - m_takes[static_cast<std::size_t>(m_take)].durationMs) * 0.5;
            m_offsets.Set(m_takes[static_cast<std::size_t>(m_take)].stem, m_videoOffsetMs);
        }
        Tip("Assume the clip brackets the take evenly. A starting point, not a measurement.");
        ImGui::SameLine();
        if (m_sync.valid)
            ImGui::TextDisabled("sync: %d/%d rows, min rtt %.0f ms, drift %+.2f ms/s | clip %.0f ms | %s",
                                m_sync.rowsUsed, m_sync.rowsTotal, m_sync.minRttMs, m_sync.driftMsPerSec,
                                m_video.Ready() ? m_video.DurationMs() : 0.0, m_video.Status());
        else
            ImGui::TextDisabled("no _sync.csv - the offset is the whole story here | %s", m_video.Status());
    }
}

void App::DrawTimeline(float height) {
    const ConfigSide& s = m_side[m_player.ActiveSide()];
    const double dur = std::max(1.0, m_player.DurationMs());

    // ── the view ─────────────────────────────────────────────────────────────
    //
    // Free state, not something derived from the loop region. The wheel zooms it
    // and the middle button pans it, and the region only ever moves it at one
    // moment: the frame playback starts. Deriving it from the region every frame
    // is what made it impossible to look anywhere else - you would zoom out, and
    // the next frame would put it straight back.
    if (!m_viewValid || m_viewSpanMs <= 0.0) {
        m_viewStartMs = 0.0;
        m_viewSpanMs = dur;
        m_viewValid = true;
    }
    const bool playing = m_player.Playing();
    if (playing && !m_wasPlaying && m_zoomRegion && m_player.HasRegion()) {
        const double lo = m_player.LoopStartMs();
        const double hi = m_player.LoopEndMs();
        const double pad = (hi - lo) * 0.08;
        m_viewStartMs = std::max(0.0, lo - pad);
        m_viewSpanMs = std::min(dur, hi + pad) - m_viewStartMs;
    }
    m_wasPlaying = playing;

    m_viewSpanMs = std::clamp(m_viewSpanMs, std::min(10.0, dur), dur);
    m_viewStartMs = std::clamp(m_viewStartMs, 0.0, dur - m_viewSpanMs);
    const double viewStart = m_viewStartMs;
    const double viewSpan = m_viewSpanMs;
    const double viewEnd = viewStart + viewSpan;
    const bool zoomed = viewSpan < dur - 0.5;

    ImGui::TextDisabled(
        "timeline - colour is CueReason, height is gain. Wheel zooms, middle drags, left drags a "
        "selection, bottom strip drags a loop region");
    ImGui::SameLine();
    ImGui::Checkbox("trace", &m_showTrace);
    ImGui::SameLine();
    ImGui::Checkbox("contacts", &m_showContacts);
    ImGui::SameLine();
    ImGui::Checkbox("snap on play", &m_zoomRegion);
    Tip("Rescale the timeline to the loop region when playback starts, and only then.\n"
        "It used to happen the moment a region was drawn, which fought the wheel: you\n"
        "would zoom out to see the context and the next frame would put it back.");
    if (zoomed) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.55f, 0.75f, 1.0f, 1.0f), "%.0f - %.0f ms  (%.0f ms of %.0f)",
                           viewStart, viewEnd, viewSpan, dur);
        ImGui::SameLine();
        if (ImGui::SmallButton("fit")) {
            m_viewStartMs = 0.0;
            m_viewSpanMs = dur;
        }
    }
    if (m_hasSelection) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.45f, 1.0f), "| selection %.0f - %.0f ms",
                           m_selStartMs, m_selEndMs);
        Tip("Right-click inside it for delete, for a new take cut out of it, and for what\n"
            "can be done with the video over that stretch.");
    }
    if (!m_selectionNote.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", m_selectionNote.c_str());
    }

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Everything below maps times through X(), which can now land off either
    // end, so the lane has to clip rather than paint over its neighbours.
    dl->PushClipRect(origin, ImVec2(origin.x + width, origin.y + height), true);

    const float regionStripH = 14.0f;
    const float laneContactsH = m_showContacts ? 12.0f : 0.0f;
    const float laneTraceH = m_showTrace ? 14.0f : 0.0f;
    const float cueH = height - regionStripH - laneContactsH - laneTraceH - 4.0f;

    const float cueTop = origin.y;
    const float cueBot = cueTop + cueH;
    const float traceTop = cueBot + 2.0f;
    const float contactTop = traceTop + laneTraceH;
    const float stripTop = origin.y + height - regionStripH;

    dl->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height), IM_COL32(22, 24, 30, 255));

    auto X = [&](double ms) {
        return origin.x + static_cast<float>((ms - viewStart) / viewSpan) * width;
    };

    // the loop region
    if (m_player.HasRegion()) {
        dl->AddRectFilled(ImVec2(X(m_player.LoopStartMs()), cueTop), ImVec2(X(m_player.LoopEndMs()), stripTop + regionStripH),
                          IM_COL32(90, 130, 200, 40));
    }

    // the selection, over the region and under the cues: it is the thing being
    // pointed at, and it has to be legible on top of whatever is playing.
    if (m_hasSelection || m_selecting) {
        const float a = X(std::min(m_selStartMs, m_selEndMs));
        const float b = X(std::max(m_selStartMs, m_selEndMs));
        dl->AddRectFilled(ImVec2(a, cueTop), ImVec2(b, stripTop), IM_COL32(230, 180, 80, 34));
        dl->AddLine(ImVec2(a, cueTop), ImVec2(a, stripTop), IM_COL32(240, 200, 110, 200), 1.5f);
        dl->AddLine(ImVec2(b, cueTop), ImVec2(b, stripTop), IM_COL32(240, 200, 110, 200), 1.5f);
    }

    // burst brackets: cues separated by less than the config's burst gap are one
    // burst, which is the rhythm the whole design is aiming at.
    const double burstGap = s.cfg.arb.burstMinGapMs;
    double burstStart = -1.0, burstEnd = -1.0;
    int burstCount = 0;
    auto flushBurst = [&] {
        if (burstStart >= 0.0 && burstCount > 0) {
            dl->AddRectFilled(ImVec2(X(burstStart) - 2.0f, cueBot - 3.0f), ImVec2(X(burstEnd) + 2.0f, cueBot),
                              IM_COL32(120, 200, 255, 110));
        }
        burstStart = burstEnd = -1.0;
        burstCount = 0;
    };
    for (const rds::Cue& c : s.result.cues) {
        if (c.reason == rds::CueReason::kFoleyBed || c.reason == rds::CueReason::kScrape) continue;
        if (burstStart < 0.0 || c.timeMs - burstEnd > burstGap) {
            flushBurst();
            burstStart = c.timeMs;
        }
        burstEnd = c.timeMs;
        ++burstCount;
    }
    flushBurst();

    // the cues
    //
    // With a slot's widget hovered in the sfx panel, that slot's cues are drawn
    // full height and everything else is dimmed - which answers "what does this
    // slot actually do in this take" by pointing at it rather than by reading a
    // count.
    const bool highlighting = m_hoverSlot >= 0;
    for (int i = 0; i < static_cast<int>(s.result.cues.size()); ++i) {
        const rds::Cue& c = s.result.cues[static_cast<std::size_t>(i)];
        const float x = X(c.timeMs);
        const float h = BarHeight(c.gainDb) * (cueH - 6.0f);
        const bool lit = highlighting && static_cast<int>(c.slot) == m_hoverSlot;
        ImU32 col = ReasonColour(c.reason);
        if (highlighting && !lit) {
            // Dimmed in place rather than hidden: the rhythm of the take is the
            // context that makes one slot's placement mean anything.
            col = (col & 0x00FFFFFFu) | (static_cast<ImU32>(45) << IM_COL32_A_SHIFT);
        }
        const bool sel = i == m_selectedCue;
        const float halfWidth = lit ? 2.5f : (sel ? 2.0f : 1.0f);
        dl->AddRectFilled(ImVec2(x - halfWidth, cueBot - h), ImVec2(x + halfWidth, cueBot), col);
        if (lit) {
            dl->AddLine(ImVec2(x, cueTop), ImVec2(x, cueBot), col, 1.0f);
            dl->AddCircleFilled(ImVec2(x, cueTop + 4.0f), 2.5f, col);
        }
        if (c.op == rds::CueOp::kStartLoop)
            dl->AddCircleFilled(ImVec2(x, cueBot - h - 4.0f), 3.0f, col);
        if (sel) dl->AddLine(ImVec2(x, cueTop), ImVec2(x, cueBot), IM_COL32(255, 255, 255, 160));
    }
    if (highlighting) {
        const std::string_view slotName = rds::ToString(static_cast<rds::SlotId>(m_hoverSlot));
        dl->AddText(ImVec2(origin.x + 6.0f, cueTop + 4.0f), IM_COL32(230, 235, 245, 210),
                    slotName.data(), slotName.data() + slotName.size());
    }

    // the trace: what arbitration threw away, which is where the 10:1 lives
    if (m_showTrace) {
        for (const rds::TraceRecord& t : s.result.trace) {
            const float x = X(t.timeMs);
            const bool emitted = std::strstr(t.outcome, "emit") != nullptr;
            dl->AddLine(ImVec2(x, traceTop + 2.0f), ImVec2(x, traceTop + laneTraceH - 2.0f),
                        emitted ? IM_COL32(110, 220, 130, 200) : IM_COL32(150, 70, 70, 170));
        }
    }

    // every contact that entered, so the reduction is a picture and not a number
    if (m_showContacts && m_recording) {
        for (const rds::FeedEvent& e : m_recording->Events()) {
            if (e.kind != rds::EventKind::kImpact) continue;
            const float x = X(e.timeMs);
            dl->AddLine(ImVec2(x, contactTop + 2.0f), ImVec2(x, contactTop + laneContactsH - 2.0f),
                        IM_COL32(90, 100, 120, 190));
        }
    }

    // the region strip
    dl->AddRectFilled(ImVec2(origin.x, stripTop), ImVec2(origin.x + width, stripTop + regionStripH),
                      IM_COL32(40, 44, 54, 255));
    if (m_player.HasRegion()) {
        dl->AddRectFilled(ImVec2(X(m_player.LoopStartMs()), stripTop), ImVec2(X(m_player.LoopEndMs()), stripTop + regionStripH),
                          IM_COL32(90, 140, 220, 190));
    }
    // A scale bar under the strip when zoomed, so "how far in am I" is a picture
    // rather than two numbers in the header.
    if (zoomed) {
        const float a = origin.x + static_cast<float>(viewStart / dur) * width;
        const float b = origin.x + static_cast<float>(viewEnd / dur) * width;
        dl->AddRectFilled(ImVec2(a, stripTop + regionStripH - 3.0f),
                          ImVec2(std::max(b, a + 2.0f), stripTop + regionStripH),
                          IM_COL32(150, 190, 255, 160));
    }

    // the playhead
    const float px = X(m_player.PositionMs());
    dl->AddLine(ImVec2(px, origin.y), ImVec2(px, origin.y + height), IM_COL32(255, 255, 255, 220), 1.5f);

    // ── interaction ──────────────────────────────────────────────────────────
    dl->PopClipRect();
    ImGui::InvisibleButton("##timeline", ImVec2(width, height),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                               ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mouse = io.MousePos;
    // The inverse of X(), so a click means the same thing zoomed or not.
    const double mouseMs =
        std::clamp(viewStart + static_cast<double>(mouse.x - origin.x) / width * viewSpan, 0.0, dur);

    // Wheel zooms about the cursor. About the cursor and not about the centre,
    // because the thing you point at is the thing you want to keep looking at -
    // centre-anchored zoom means chasing the target across the lane.
    if (hovered && io.MouseWheel != 0.0f) {
        const double factor = std::pow(0.82, static_cast<double>(io.MouseWheel));
        const double span = std::clamp(viewSpan * factor, std::min(10.0, dur), dur);
        m_viewStartMs = std::clamp(mouseMs - (mouseMs - viewStart) * (span / viewSpan), 0.0,
                                   dur - span);
        m_viewSpanMs = span;
    }
    // Middle drags. Dragging the *content*, so the timeline follows the mouse
    // rather than running away from it.
    if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Middle) && io.MouseDelta.x != 0.0f) {
        m_viewStartMs = std::clamp(
            m_viewStartMs - static_cast<double>(io.MouseDelta.x) / width * viewSpan, 0.0,
            dur - viewSpan);
    }

    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (mouse.y >= stripTop) {
            // Freeze the window for the gesture, and remember the region we are
            // replacing so the whole drag lands on the undo stack as one step.
            WindowMs(m_frozenLoMs, m_frozenHiMs);
            m_frozenIsRegion = WindowIsRegion();
            m_dragPrevStartMs = m_player.LoopStartMs();
            m_dragPrevEndMs = m_player.LoopEndMs();
            m_regionDragging = true;
            m_regionAnchorMs = mouseMs;
            m_player.SetLoopRegion(mouseMs, mouseMs);
        } else {
            // Not a seek yet. A press on the body is either a click - seek and
            // pick the nearest cue - or the start of a selection drag, and which
            // one it is only becomes clear once the mouse has moved. Doing both
            // at once would mean every selection also threw the playhead across
            // the take.
            m_maybeSelecting = true;
            m_selPressX = mouse.x;
            m_selAnchorMs = mouseMs;
        }
    }

    if (m_maybeSelecting && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        if (!m_selecting && std::fabs(mouse.x - m_selPressX) > 4.0f) {
            m_selecting = true;
            m_selectionNote.clear();
        }
        if (m_selecting) {
            m_selStartMs = std::min(m_selAnchorMs, mouseMs);
            m_selEndMs = std::max(m_selAnchorMs, mouseMs);
        }
    } else if (m_maybeSelecting) {
        m_maybeSelecting = false;
        if (m_selecting) {
            m_selecting = false;
            m_hasSelection = (m_selEndMs - m_selStartMs) > 1.0;
        } else {
            // It was a click after all.
            m_player.SeekMs(m_selAnchorMs);
            int best = -1;
            double bestDx = 1e18;
            for (int i = 0; i < static_cast<int>(s.result.cues.size()); ++i) {
                const double dx = std::fabs(s.result.cues[static_cast<std::size_t>(i)].timeMs - m_selAnchorMs);
                if (dx < bestDx) {
                    bestDx = dx;
                    best = i;
                }
            }
            if (best >= 0 && bestDx / viewSpan * width < 8.0) m_selectedCue = best;
        }
    }

    if (m_regionDragging) {
        // The region tracks the mouse the whole way - you watch the selection
        // grow - but WindowMs() reports the pre-drag window until the button
        // comes up, so every table below holds still until you are done.
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            m_player.SetLoopRegion(std::min(m_regionAnchorMs, mouseMs),
                                   std::max(m_regionAnchorMs, mouseMs));
        } else {
            m_regionDragging = false;
            const double lo = std::min(m_regionAnchorMs, mouseMs);
            const double hi = std::max(m_regionAnchorMs, mouseMs);
            // A press and release in one spot is a click, not a zero-width
            // region: it clears. The strip is where you go to change the
            // region, so it is also where you go to stop having one.
            const bool click = (hi - lo) <= 20.0;
            // Put the pre-drag region back first, so SetRegion records the step
            // against what was there before the gesture rather than against the
            // half-finished selection the drag left behind.
            m_player.SetLoopRegion(m_dragPrevStartMs, m_dragPrevEndMs);
            SetRegion(click ? 0.0 : lo, click ? 0.0 : hi,
                      click ? "clear region" : "loop region");
        }
    }

    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && m_hasSelection &&
        mouseMs >= m_selStartMs && mouseMs <= m_selEndMs && mouse.y < stripTop) {
        ImGui::OpenPopup("##selection");
    }
    if (ImGui::BeginPopup("##selection")) {
        DrawSelectionMenu();
        ImGui::EndPopup();
    }

    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Right) && !m_hasSelection) {
        SetRegion(0.0, 0.0, "clear region");
    }
}

void App::DrawSelectionMenu() {
    ImGui::TextDisabled("%.0f - %.0f ms  (%.0f ms)", m_selStartMs, m_selEndMs,
                        m_selEndMs - m_selStartMs);
    ImGui::Separator();

    if (ImGui::MenuItem("Delete events in this stretch")) DeleteSelection();
    Tip("Rewrites the take's CSV without those rows. Everything after the cut keeps its\n"
        "original timestamp, so the video and every export already written about this\n"
        "take still line up.");

    if (ImGui::MenuItem("Create recording from selection")) CreateRecordingFromSelection();
    Tip("Writes a new take beside this one: the events, re-based to start at zero, with\n"
        "this take's limb table and sidecar. When there is video it cuts the matching\n"
        "stretch out of it and builds the new take's frame cache from that.");

    const bool hasVideo = m_take >= 0 && !m_takes[static_cast<std::size_t>(m_take)].videoPath.empty();
    ImGui::Separator();
    ImGui::BeginDisabled(!hasVideo);
    if (ImGui::MenuItem("Extract the video for this stretch")) ExtractSelectionVideo();
    Tip("Cuts the selection out of the take's mp4 into a clip beside it and leaves the\n"
        "take alone. For pulling one fall out to look at frame by frame.");
    if (ImGui::MenuItem("Delete this take's video")) DeleteTakeVideo();
    Tip("Drops the mp4 and the frame cache built from it. The take itself is untouched\n"
        "and still replays; it simply has no picture any more.");
    ImGui::EndDisabled();
    if (!hasVideo) {
        ImGui::TextDisabled("(this take has no video)");
    }

    ImGui::Separator();
    if (ImGui::MenuItem("Clear selection")) {
        m_hasSelection = false;
        m_selectionNote.clear();
    }
}

void App::DrawStats() {
    const ConfigSide& s = m_side[m_player.ActiveSide()];
    const rds::EngineStats& st = s.result.stats;

    ImGui::Text("side %c: %u contacts in -> %u cues out, %u bursts, %.1f:1", 'A' + m_player.ActiveSide(),
                st.contactsIn, st.emittedCues, st.bursts, st.ReductionRatio());
    Tip("The design's target is about 10:1 - four to six audible moments against 30-60 collisions.");
    ImGui::SameLine();
    ImGui::TextDisabled("| peak %.0f u/s | cues span %.0f-%.0f ms | RunOffline + mix %.1f ms", st.peakSpeed,
                        st.firstCueMs, st.lastCueMs, s.runMs);

    // The bank's per-slot resolution: "imp_sub: 0/2 files, procedural" is the
    // line that explains a thin mix, and it belongs on screen for the same
    // reason it belongs in the log.
    int withFiles = 0, declared = 0;
    for (const rds::SlotDesc& d : rds::Slots()) {
        if (d.expectedVariants == 0) continue;
        ++declared;
        if (m_bank.FileCount(d.id) > 0) ++withFiles;
    }
    if (s.audio) {
        const float rawDb = 20.0f * std::log10(std::max(s.audio->rawPeak, 1e-6f));
        if (s.audio->rawPeak > 1.0f)
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.35f, 1), "mix peak %+.1f dBFS before the limiter", rawDb);
        else
            ImGui::TextDisabled("mix peak %+.1f dBFS before the limiter", rawDb);
        Tip("The testbench soft-clips the sum; the game does not. Above 0 dBFS here means this "
            "config would clip in Skyrim, whatever it sounds like in this window.");
    }

    ImGui::TextDisabled("bank: %d of %d slots have wav files, the rest are procedural stand-ins", withFiles,
                        declared);
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        for (const rds::SlotDesc& d : rds::Slots()) {
            if (d.expectedVariants == 0) continue;
            ImGui::Text("%.*s: %zu/%u files%s", static_cast<int>(d.name.size()), d.name.data(),
                        m_bank.FileCount(d.id), d.expectedVariants,
                        m_bank.FileCount(d.id) ? "" : ", procedural");
        }
        ImGui::EndTooltip();
    }

    // What is actually sounding in this window, by slot.
    //
    // This replaced a breakdown of everything the arbitrator threw away. Both are
    // true, but only one answers the question being asked in front of the video:
    // "there were four thuds there and I heard one" is about which slots fired,
    // not about which rule dropped what - and the drop counts are still in the
    // export, where reading them slowly is the point.
    double lo = 0.0;
    double hi = 0.0;
    WindowMs(lo, hi);

    std::array<int, static_cast<std::size_t>(rds::SlotId::kCount)> perSlot{};
    int total = 0;
    for (const rds::Cue& c : s.result.cues) {
        if (c.timeMs < lo || c.timeMs > hi) continue;
        const auto k = static_cast<std::size_t>(c.slot);
        if (k < perSlot.size()) ++perSlot[k];
        ++total;
    }

    if (WindowIsRegion()) {
        ImGui::TextColored(ImVec4(0.55f, 0.75f, 1.0f, 1.0f),
                           "cues by slot in the loop region  %.0f-%.0f ms  (%d cues)", lo, hi,
                           total);
    } else {
        ImGui::TextDisabled("cues by slot, whole take (%d cues) - drag the strip under the "
                            "timeline to narrow it", total);
    }

    // Only the slots that fired, so the row is a picture of this window rather
    // than a fixed table of mostly zeroes.
    if (total == 0) {
        ImGui::TextDisabled("(nothing sounds here)");
    } else if (ImGui::BeginTable("perslot", 7, ImGuiTableFlags_SizingStretchProp)) {
        int drawn = 0;
        for (const rds::SlotDesc& d : rds::Slots()) {
            const auto k = static_cast<std::size_t>(d.id);
            if (k >= perSlot.size() || perSlot[k] == 0) continue;
            if (drawn % 7 == 0) ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%.*s", static_cast<int>(d.name.size()), d.name.data());
            ImGui::SameLine();
            ImGui::Text("%d", perSlot[k]);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%.*s\n%zu/%u wav files%s",
                                  static_cast<int>(d.character.size()), d.character.data(),
                                  m_bank.FileCount(d.id), d.expectedVariants,
                                  m_bank.FileCount(d.id) ? "" : " - procedural stand-in");
            }
            ++drawn;
        }
        ImGui::EndTable();
    }
}

void App::DrawSelectedCue() {
    const ConfigSide& s = m_side[m_player.ActiveSide()];
    if (m_selectedCue < 0 || m_selectedCue >= static_cast<int>(s.result.cues.size())) {
        ImGui::TextDisabled(
            "click a cue - on the timeline or in the list below - for its provenance");
        return;
    }
    const rds::Cue& c = s.result.cues[static_cast<std::size_t>(m_selectedCue)];
    const std::string_view slot = rds::ToString(c.slot);
    ImGui::Text("cue %d @ %.0f ms   %.*s v%u   %.1f dB   pitch %.3f", m_selectedCue, c.timeMs,
                static_cast<int>(slot.size()), slot.data(), c.variant, c.gainDb, c.pitch);
    const std::string_view reason = rds::ToString(c.reason);
    const std::string_view site = rds::ToString(c.site);
    const std::string_view surf = rds::ToString(c.surface);
    const std::string_view phase = rds::ToString(c.phase);
    ImGui::TextDisabled("actor %08X  limb %u (%.*s)  surface %.*s  reason %.*s  phase %.*s  intensity %.3f  seq %u",
                        c.actorId, c.limbIndex, static_cast<int>(site.size()), site.data(),
                        static_cast<int>(surf.size()), surf.data(), static_cast<int>(reason.size()), reason.data(),
                        static_cast<int>(phase.size()), phase.data(), c.intensity, c.sourceSeq);
    Tip("sourceSeq is the seq column of the take's CSV - the row this cue came from.");
}

void App::DrawRight(int side, float height, bool split) {
    ImGui::PushID(side);
    ConfigSide& s = m_side[side];
    int collapseRequest = -1;

    ImGui::BeginChild("panel", ImVec2(0, height), ImGuiChildFlags_Borders);

    if (split) {
        const bool active = m_player.ActiveSide() == side;
        ImGui::TextColored(active ? ImVec4(0.5f, 0.9f, 1.0f, 1.0f) : ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "%c",
                           'A' + side);
        ImGui::SameLine();
    }

    // named configs
    ImGui::SetNextItemWidth(180.0f);
    if (ImGui::BeginCombo("##configs", s.name.c_str())) {
        for (int i = 0; i < static_cast<int>(m_configFiles.size()); ++i) {
            const std::string item = m_configFiles[static_cast<std::size_t>(i)].stem().string();
            if (ImGui::Selectable(item.c_str(), item == s.name)) {
                m_configIndex = i;
                m_focusSide = side;
                LoadConfigFile(side, m_configFiles[static_cast<std::size_t>(i)]);
            }
        }
        if (m_configFiles.empty()) ImGui::TextDisabled("(none saved yet)");
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160.0f);
    ImGui::InputText("##savename", s.saveName, sizeof(s.saveName));
    ImGui::SameLine();
    if (ImGui::Button(s.unsaved ? "Save *" : "Save")) SaveConfigFile(side);
    Tip(std::string("Writes a RagdollSounds_Algorithm.ini through ConfigManager::SaveFrom - a "
                    "testbench\nconfig is a shippable ini, not a testbench-only format.\n\n") +
        (m_saveAsIteration
             ? "With \"iterate\" on this writes the next number in the family - " +
                   std::string(s.saveName) + " becomes " + NextIteration(s.saveName) + " - so the "
                   "version you are listening to now is still there tomorrow."
             : "With \"iterate\" off this overwrites whatever the name box says."));
    ImGui::SameLine();
    ImGui::Checkbox("iterate", &m_saveAsIteration);
    Tip("A save writes the next iteration rather than over the one it loaded:\n"
        "  config_22_08_4  ->  config_22_08_5\n"
        "On by default. The reason to save mid-session is nearly always that the last one was\n"
        "worth keeping too, and overwriting it is the one thing Ctrl+Z cannot take back.");
    ImGui::SameLine();
    if (ImGui::Button("Defaults")) {
        const rds::AlgorithmConfig before = s.cfg;
        s.cfg = rds::AlgorithmConfig{};
        s.cfg.slots.rngSeed = m_seed;
        s.dirty = true;
        PushEdit(side, before, "defaults");
    }
    ImGui::SameLine();
    ImGui::TextColored(s.unsaved ? ImVec4(1.0f, 0.85f, 0.45f, 1.0f) : ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                       "%s", s.unsaved ? "*" : " ");
    Tip(s.unsaved ? "This side has edits that are not in any file." : "This side matches its file.");

    // undo / redo of the unsaved edits
    ImGui::SameLine();
    ImGui::BeginDisabled(s.undo.empty() && !s.editOpen);
    if (ImGui::Button("Undo")) {
        m_focusSide = side;
        Undo(side);
    }
    ImGui::EndDisabled();
    Tip(s.undo.empty() ? std::string("Ctrl+Z. Nothing to undo on this side.")
                       : "Ctrl+Z - undo \"" + s.undo.back().label + "\" (" +
                             std::to_string(s.undo.size()) + " step" +
                             (s.undo.size() == 1 ? "" : "s") +
                             ").\n"
                             "Slider, checkbox and combo edits only: loading a named config "
                             "starts a fresh history, and Save is not undone.");
    ImGui::SameLine();
    ImGui::BeginDisabled(s.redo.empty());
    if (ImGui::Button("Redo")) {
        m_focusSide = side;
        Redo(side);
    }
    ImGui::EndDisabled();
    Tip(s.redo.empty() ? std::string("Ctrl+Y / Ctrl+Shift+Z. Nothing to redo on this side.")
                       : "Ctrl+Y / Ctrl+Shift+Z - redo \"" + s.redo.back().label + "\" (" +
                             std::to_string(s.redo.size()) + " step" +
                             (s.redo.size() == 1 ? "" : "s") + ").");
    if (split) {
        ImGui::SameLine();
        if (ImGui::Button("Continue with this")) collapseRequest = side;
        Tip("Collapses the split onto this config.");
    }

    // the design's own checks, on demand
    ImGui::SameLine();
    if (ImGui::Button("Verify") && m_recording) {
        rds::OfflineOptions opt;
        opt.seed = m_seed;
        opt.trace = false;
        m_bank.Seed(m_seed);
        m_recording->Rewind();
        s.verify = rds::Verify(*m_recording, s.cfg, m_bank, opt).checks;
        s.verifyRun = true;
    }
    if (s.verifyRun) {
        ImGui::SameLine();
        int failed = 0;
        for (const rds::VerifyExpectation& v : s.verify)
            if (!v.passed) ++failed;
        if (failed == 0)
            ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1), "all %d checks pass", static_cast<int>(s.verify.size()));
        else
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1), "%d of %d checks fail", failed,
                               static_cast<int>(s.verify.size()));
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            for (const rds::VerifyExpectation& v : s.verify)
                ImGui::TextColored(v.passed ? ImVec4(0.5f, 0.9f, 0.6f, 1) : ImVec4(1, 0.55f, 0.45f, 1), "%s %s - %s",
                                   v.passed ? "ok  " : "FAIL", v.name.c_str(), v.detail.c_str());
            ImGui::EndTooltip();
        }
    }

    ImGui::SetNextItemWidth(200.0f);
    ImGui::InputTextWithHint("##filter", "filter parameters", m_filter, sizeof(m_filter));
    ImGui::SameLine();
    ImGui::TextDisabled("%zu cues, %.1f ms", s.result.cues.size(), s.runMs);

    ImGui::Separator();
    ImGui::BeginChild("params", ImVec2(0, 0));
    DrawParams(side);
    ImGui::EndChild();

    ImGui::EndChild();
    ImGui::PopID();

    if (collapseRequest >= 0) CollapseOnto(collapseRequest);
}

void App::DrawParams(int side) {
    ConfigSide& s = m_side[side];
    void* root = &s.cfg;

    std::string needle = m_filter;
    std::transform(needle.begin(), needle.end(), needle.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    std::string_view openGroup;
    bool groupVisible = false;

    for (const rds::ParamDesc& p : rds::AlgorithmParams()) {
        if (!needle.empty()) {
            std::string hay = std::string(p.key) + " " + std::string(p.label) + " " + std::string(p.group);
            std::transform(hay.begin(), hay.end(), hay.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (hay.find(needle) == std::string::npos) continue;
        }

        if (p.group != openGroup) {
            openGroup = p.group;
            groupVisible = ImGui::CollapsingHeader(std::string(p.group).c_str(), ImGuiTreeNodeFlags_DefaultOpen);
        }
        if (!groupVisible) continue;

        // The desc's own address, not the key text: several sections carry a key
        // spelled the same way (fGainDb), and two widgets sharing an ImGui id
        // fight over which one is active.
        ImGui::PushID(static_cast<const void*>(&p));

        double value = rds::GetParam(root, p);
        const bool moved = std::fabs(value - p.defaultValue) > 1e-9;

        std::string tip = std::string(p.tooltip);
        tip += "\n\n" + rds::QualifiedKey(p) + "   default " + rds::FormatParam(p, p.defaultValue);

        // The name gets a column of its own. Letting the widget carry its label
        // pushes the text off the right edge of the panel, which turns a tuning
        // surface into a hundred anonymous numbers.
        if (moved) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.45f, 1.0f));
        ImGui::TextUnformatted(p.label.data(), p.label.data() + p.label.size());
        if (moved) ImGui::PopStyleColor();
        Tip(tip);

        const float nameEnd = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x;
        ImGui::SameLine(std::max(190.0f, nameEnd + 12.0f));
        ImGui::SetNextItemWidth(-4.0f);

        const std::string id = "##" + std::string(p.key);
        bool changed = false;

        switch (p.type) {
            case rds::ParamType::kBool: {
                bool b = value != 0.0;
                if (ImGui::Checkbox(id.c_str(), &b)) {
                    value = b ? 1.0 : 0.0;
                    changed = true;
                }
                break;
            }
            case rds::ParamType::kInt: {
                int v = static_cast<int>(std::lround(value));
                if (ImGui::SliderInt(id.c_str(), &v, static_cast<int>(p.minValue),
                                     static_cast<int>(p.maxValue))) {
                    value = v;
                    changed = true;
                }
                break;
            }
            case rds::ParamType::kEnum: {
                int v = static_cast<int>(std::lround(value));
                // Through std::string: a string_view in the schema is not promised
                // to be null-terminated and ImGui wants a C string.
                const std::string preview = (v >= 0 && v < static_cast<int>(p.enumNames.size()))
                                                ? std::string(p.enumNames[static_cast<std::size_t>(v)])
                                                : std::string("?");
                if (ImGui::BeginCombo(id.c_str(), preview.c_str())) {
                    for (int i = 0; i < static_cast<int>(p.enumNames.size()); ++i) {
                        const std::string n(p.enumNames[static_cast<std::size_t>(i)]);
                        if (ImGui::Selectable(n.c_str(), i == v)) {
                            value = i;
                            changed = true;
                        }
                    }
                    ImGui::EndCombo();
                }
                break;
            }
            case rds::ParamType::kFloat: {
                float v = static_cast<float>(value);
                const char* fmt = (p.maxValue - p.minValue) > 50.0 ? "%.1f" : "%.3f";
                if (ImGui::SliderFloat(id.c_str(), &v, static_cast<float>(p.minValue),
                                       static_cast<float>(p.maxValue), fmt)) {
                    value = v;
                    changed = true;
                }
                break;
            }
        }
        Tip(tip);

        if (changed) {
            // Snapshot before the first write of this gesture. The step is not
            // pushed until the widget is released, below.
            BeginEdit(side, std::string(p.group) + " / " + std::string(p.label));
            m_focusSide = side;

            if (p.step > 0.0 && p.type == rds::ParamType::kFloat)
                value = std::round(value / p.step) * p.step;
            rds::SetParam(root, p, rds::CoerceParam(p, value));
            s.cfg.slots.rngSeed = m_seed;
            s.dirty = true;
            s.verifyRun = false;
        }

        ImGui::PopID();
    }

    // The gesture is over when nothing anywhere is being held: a released
    // slider, a closed combo, a clicked checkbox. Asking the global rather than
    // the widget also closes a step whose widget the filter has since hidden.
    if (s.editOpen && !ImGui::IsAnyItemActive()) CommitEdit(side);
}

void App::HandleKeys() {
    const ImGuiIO& keys = ImGui::GetIO();
    // Ctrl+S first, and before the text-input guard: it has to work while a
    // config name or an sfx note is half typed, which is exactly when you reach
    // for it.
    if (keys.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) {
        SaveEverything();
        return;
    }
    // The browser owns the arrows and the space bar while it has focus. Without
    // this, space would audition the highlighted sound *and* toggle the take's
    // transport - two sounds starting at once, and neither of them the
    // comparison you asked for.
    if (m_browser.WantsKeys()) return;
    if (keys.WantTextInput) return;

    if (ImGui::IsKeyPressed(ImGuiKey_Keypad5, false)) {
        m_player.SeekMs(m_player.HasRegion() ? m_player.LoopStartMs() : 0.0);
        m_player.Play();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Keypad4, false)) StepRecording(-1);
    if (ImGui::IsKeyPressed(ImGuiKey_Keypad6, false)) StepRecording(1);
    const ImGuiIO& io = ImGui::GetIO();
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
        if (io.KeyShift)
            RedoLatest();
        else
            UndoLatest();
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) RedoLatest();

    if (ImGui::IsKeyPressed(ImGuiKey_Keypad8, false)) StepConfig(-1);
    if (ImGui::IsKeyPressed(ImGuiKey_Keypad2, false)) StepConfig(1);
    if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) m_player.TogglePlay();
}

}  // namespace tb
