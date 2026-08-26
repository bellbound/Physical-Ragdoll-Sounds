#include "App.h"

#include <algorithm>
#include <array>
#include <charconv>
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
        case rds::CueReason::kSurfaceSkin:     return IM_COL32(148, 148, 152, 255);
        case rds::CueReason::kHeadImpact:      return IM_COL32(255, 96, 96, 255);
        case rds::CueReason::kCrunch:          return IM_COL32(236, 88, 176, 255);
        case rds::CueReason::kGore:            return IM_COL32(190, 40, 60, 255);
        case rds::CueReason::kLimbTap:         return IM_COL32(150, 205, 255, 255);
        case rds::CueReason::kScrape:          return IM_COL32(120, 220, 170, 255);
        case rds::CueReason::kAirborneRise:    return IM_COL32(150, 120, 220, 255);
        // The same warm neutral the envelope under the lane is drawn in, so the
        // handful of cues and the curve they came from read as one thing.
        case rds::CueReason::kRustle:          return IM_COL32(214, 176, 132, 255);
    }
    return IM_COL32(200, 200, 200, 255);
}

/// The material a surface skin named, or 0 for anything that is not one.
///
/// Kept apart from the bar colour rather than folded into it, because a
/// surface skin is drawn as two things: the layer, which is grey like every
/// other skin, and the floor it identified, which is the only part that
/// differs between them. One colour could say either but not both, and the
/// question a timeline gets asked is "did this take land on wood", which is
/// about a handful of caps in a lane of grey and not about hue at all.
ImU32 SurfaceCapColour(rds::SlotId slot) {
    switch (slot) {
        case rds::SlotId::kSurfWood:  return IM_COL32(158, 110, 62, 255);
        case rds::SlotId::kSurfStone: return IM_COL32(96, 138, 196, 255);
        case rds::SlotId::kSurfSoft:  return IM_COL32(96, 166, 108, 255);
        default:                      return 0;
    }
}

/// Why a cue is playing a file named after some other slot, or nothing at all
/// when it is playing its own.
///
/// The surface-coloured scrapes are declared with nothing recorded behind them
/// and resolve to the grind they are a variant of, which is what makes dropping
/// a wav in the only thing needed to colour a floor. Left unexplained, the cue
/// table reads as a slot playing somebody else's sound by mistake.
std::string FallbackNote(rds::SlotId asked, const CueSound& snd) {
    if (!snd.fellBack) {
        return {};
    }
    return std::format("{} has no recording of its own, so it plays {}. Drop a wav on it and "
                       "this cue changes with no other edit.\n\n",
                       rds::ToString(asked), rds::ToString(snd.plays));
}

/// The bar colour, by slot where the reason lumps several together.
///
/// `kImpactComposite` covers three layers that are the whole argument of the
/// design - a light transient, the body, and the late sub that is supposed to
/// be carrying the weight - and drawing them in one orange made the composite
/// unreadable exactly where it matters: whether the sub is louder than the
/// transient is a claim you should be able to check by looking. So the three
/// share a hue and separate by lightness, top to bottom of the stack.
ImU32 CueColour(rds::SlotId slot, rds::CueReason reason) {
    switch (slot) {
        case rds::SlotId::kImpTransient: return IM_COL32(255, 210, 145, 255);
        case rds::SlotId::kImpBody:      return IM_COL32(255, 176, 64, 255);
        case rds::SlotId::kImpSub:       return IM_COL32(196, 121, 30, 255);
        default:                         break;
    }
    return ReasonColour(reason);
}

/// dB to a 0..1 bar height. The references' usable span is about 35 dB, so the
/// timeline shows that span rather than a full 60 - otherwise every cue in a
/// well-tuned mix draws at the same height.
float BarHeight(float gainDb) {
    return std::clamp((gainDb + 40.0f) / 40.0f, 0.03f, 1.0f);
}

/// "12.9k" rather than "12873". A realtime factor is read for its order of
/// magnitude - whether the engine is a hundred or ten thousand times faster
/// than the fall it is replaying - and five significant figures of it are five
/// characters of noise.
std::string Compact(double value) {
    if (value >= 1e6) return std::format("{:.1f}M", value / 1e6);
    if (value >= 1e3) return std::format("{:.1f}k", value / 1e3);
    return std::format("{:.0f}", value);
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

/// The explanatory half of a hover, in the same wrap and the same grey as `Tip`.
///
/// The timeline's marks are drawn straight into the draw list, so they are not
/// ImGui items and `Tip` cannot see them - their tooltips are opened by hand
/// against a hit test. This is what keeps the two kinds of tooltip looking like
/// one thing: the numbers go above it in the caller's own format, and the
/// sentence explaining what the mark *is* comes through here.
/// The mouse wheel over a combo, as a step through its list.
///
/// ImGui has no such thing: a combo is a button that opens a popup, and the
/// wheel over it scrolls whatever window it is sitting in. That is exactly
/// wrong for the two pickers here - the whole of an A/B is "the one above this,
/// then the one below it", and doing that through a popup is three clicks a
/// comparison.
///
/// SetItemKeyOwner is what stops the panel scrolling underneath at the same
/// time: it hands the wheel to this widget for the frame, which is the
/// documented way to take it off the window under an item.
[[nodiscard]] int ComboWheel() {
    if (!ImGui::IsItemHovered()) return 0;
    ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
    const float wheel = ImGui::GetIO().MouseWheel;
    if (wheel == 0.0f) return 0;
    // A trackpad can deliver several notches in one frame, and a list read by
    // wheel is one people flick through.
    const int magnitude = std::max(1, static_cast<int>(std::lround(std::fabs(wheel))));
    // Wheel up is towards the top of the list, which is the newest config.
    return wheel > 0.0f ? -magnitude : magnitude;
}

void TipBody(std::string_view text) {
    // TextUnformatted rather than TextDisabled: these strings name config keys
    // and ratios, and a stray `%` in one of them would be read as a format.
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    ImGui::PushTextWrapPos(420.0f);
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

/// Where a row's number sits against the rest of the ones on screen.
///
/// Both listings carry a column that is the reason a row is interesting - how
/// hard the limb hit, how loud the cue came out - and reading it means reading
/// it against its neighbours. The scale is the visible set rather than a fixed
/// one because "hard" is a property of the take, and of the region when a
/// region is selected: a stumble and a fall down a staircase do not share a
/// loudest hit.
struct Spread {
    double lo{};
    double mid{};
    double hi{};

    /// Sorts a copy, which is the whole cost of this: a few thousand doubles
    /// once a frame, against a table that walks the same set anyway.
    [[nodiscard]] static Spread Of(std::vector<double> values) {
        Spread s;
        if (values.empty()) return s;
        std::sort(values.begin(), values.end());
        s.lo = values.front();
        s.mid = values[values.size() / 2];
        s.hi = values.back();
        return s;
    }

    /// -1 at the smallest value, 0 at the median, +1 at the largest.
    ///
    /// Each half is scaled by its own reach rather than both halves by one
    /// lo..hi range. That puts the median on exactly neutral whichever way the
    /// distribution leans, and it stops one enormous hit from washing every
    /// ordinary one green: the outlier eats the top of its own half and leaves
    /// the bottom half to be read on its own terms.
    [[nodiscard]] double At(double value) const {
        if (value > mid) return hi > mid ? (value - mid) / (hi - mid) : 0.0;
        if (value < mid) return lo < mid ? -(mid - value) / (mid - lo) : 0.0;
        return 0.0;
    }
};

/// The row wash for a `Spread::At` reading: muted green at the bottom, red at
/// the top, and nothing at all through the middle, so an average row keeps the
/// striping it has always had.
///
/// Alpha carries the strength and the hue only says which end. That is what
/// lets this sit under the playhead highlight rather than fight it, and it is
/// why a table of near-identical numbers stays quiet instead of splitting
/// itself into a red half and a green one. Returns a transparent 0 for the
/// flat middle so the caller can skip the call into ImGui entirely.
[[nodiscard]] ImU32 IntensityTint(double t) {
    const double magnitude = std::clamp(std::fabs(t), 0.0, 1.0);
    if (magnitude < 0.05) return 0;
    const auto alpha = static_cast<int>(magnitude * 72.0);
    return t > 0.0 ? IM_COL32(196, 66, 54, alpha) : IM_COL32(86, 140, 96, alpha);
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

    // Indexed once, here: the switch that uses it is clicked between one shove
    // and the next, and a recursive walk of a sound extract on every click would
    // be felt.
    m_vanillaLibrary.SetRoot(m_paths.vanillaSounds);

    m_offsets.Load(m_paths.frameCache / "video-offsets.ini");
    m_takeFlags.Load(m_paths.frameCache / "recording-flags.ini");

    // Before anything is drawn and before the link starts: the video mode and
    // the push-to-game switch are both read on the way up.
    LoadUiPrefs();

    // The link, OBS and the port they use all come out of the deployed mod's own
    // RagdollSounds.ini - see StartLink.
    StartLink();
    // And the control socket sits on the port next door, so a command line can
    // reach this session while it is running - see Control.h.
    StartControl();

    ScanRecordings();
    ScanConfigs();

    if (!m_player.Start(48000)) m_startupError = m_player.LastError();
    m_player.SetLoop(m_autoLoop);
    // After LoadUiPrefs, which is what makes it the volume this machine was
    // last listening at rather than the default.
    m_player.SetPreviewGainDb(m_sfxPreviewGainDb);

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

std::vector<std::pair<std::string, bool*>> App::UiPrefFields() {
    std::vector<std::pair<std::string, bool*>> fields = {
        {"bLimiter", &m_limiter},
        {"bShowTrace", &m_showTrace},
        {"bShowContacts", &m_showContacts},
        {"bSnapOnPlay", &m_zoomRegion},
        {"bLimitToCues", &m_limitToCues},
        {"bSkipCueless", &m_skipCueless},
        {"bFollowImpacts", &m_followImpacts},
        {"bHideSelfImpacts", &m_hideSelfImpacts},
        {"bFollowCues", &m_followCues},
        {"bSaveAsIteration", &m_saveAsIteration},
        {"bExportConfigs", &m_exportConfigs},
        {"bPushToGame", &m_pushToGame},
        {"bVideoSync", &m_videoSync},
        {"bSplitAB", &m_split},
        {"bAutoLoop", &m_autoLoop},
        {"bDiffOnly", &m_diffOnly},
        {"bCompareShowSame", &m_compareShowSame},
    };

    // One per slot, keyed by the slot's own name rather than its index, because
    // the enum's order is not a promise: inserting a slot in the middle of it
    // would otherwise silently move every switch after it onto the wrong row.
    for (const rds::SlotDesc& desc : rds::Slots()) {
        const auto index = static_cast<std::size_t>(desc.id);
        if (index < m_slotHidden.size()) {
            fields.emplace_back(std::format("bHideInTimeline_{}", desc.name),
                                &m_slotHidden[index]);
        }
    }
    return fields;
}

std::vector<App::FracPref> App::UiPrefFracFields() {
    // The bounds are the ones the matching Splitter call drags between. Repeated
    // here rather than shared because the splitter needs them at the call site
    // anyway, and a load that clamped to a wider range than the bar can reach is a
    // panel you cannot drag back.
    return {
        {"fLeftSplit", &m_leftFrac, 0.25f, 0.8f},
        {"fVideoSplit", &m_videoFrac, 0.12f, 0.75f},
        {"fTableSplit", &m_tableFrac, 0.2f, 0.8f},
        {"fConfigSplit", &m_configFrac, 0.2f, 0.85f},
        // Not a fraction, but the same handling: a float with bounds, clamped
        // on the way in. The bounds are the sfx library slider's own.
        {"fSfxPreviewGain", &m_sfxPreviewGainDb, -36.0f, 12.0f},
    };
}

void App::LoadUiPrefs() {
    m_uiPrefsFile = m_paths.frameCache / "ui-state.ini";

    const auto fields = UiPrefFields();
    const auto fracs = UiPrefFracFields();
    for (const std::string& line : rds::ini::ReadLines(m_uiPrefsFile)) {
        std::string_view key;
        std::string_view value;
        if (!rds::ini::SplitAssignment(line, key, value)) continue;
        bool matched = false;
        for (const auto& [name, member] : fields) {
            if (rds::ini::EqualsIgnoreCase(name, key)) {
                *member = value != "0";
                matched = true;
                break;
            }
        }
        if (matched) continue;
        for (const FracPref& f : fracs) {
            if (!rds::ini::EqualsIgnoreCase(f.key, key)) continue;
            // Clamped, and ignored outright when it will not parse. A file meant to
            // be deletable is a file that gets hand-edited, and a layout read off a
            // typo must not be one you have to quit to escape.
            float parsed = 0.0f;
            const char* first = value.data();
            if (std::from_chars(first, first + value.size(), parsed).ec == std::errc{}) {
                *f.member = std::clamp(parsed, f.lo, f.hi);
            }
            break;
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

    // Not while the button is down. A splitter fraction changes every frame of a
    // drag, and rewriting the file sixty times a second to record a bar that is
    // still moving is fifty-nine writes saying what the last one says. Nothing is
    // lost by waiting: a checkbox settles on release too.
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) return;

    std::string text =
        "; Physical Ragdoll Sounds testbench - the UI switches, remembered between launches.\n"
        "; Nothing here changes what the mod does. Delete the file to go back to defaults.\n";
    for (const auto& [key, member] : UiPrefFields()) {
        text += std::format("{}={}\n", key, *member ? 1 : 0);
    }
    for (const FracPref& f : UiPrefFracFields()) {
        text += std::format("{}={:.4f}\n", f.key, *f.member);
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

    // Newest first, by when the file was created rather than by its name.
    //
    // The names sort nearly right and then stop: `config_24_08_10` lands
    // between 1 and 2, a config named after what it was trying sorts under its
    // first letter, and the day part means a folder that spans a month is in
    // day-of-month order. None of that is the order the work happened in, which
    // is the only order this list is ever read in - the one you want is nearly
    // always the one you saved last, and the one under it is what it came from.
    //
    // Created, not written: a save writes a new file, so creation time is when
    // that config came into being, and a file touched by a hand edit afterwards
    // keeps its place in the lineage rather than jumping to the top.
    std::ranges::sort(m_configFiles, [](const fs::path& a, const fs::path& b) {
        const std::uint64_t at = FileCreatedTicks(a);
        const std::uint64_t bt = FileCreatedTicks(b);
        if (at != bt) return at > bt;
        return a.stem().string() > b.stem().string();
    });
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
    // A different take knows nothing about which slots the last one used.
    m_slotSeen.fill(false);
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

std::vector<rds::ParamDesc> App::AlgorithmAndOpenedSurfaces(const rds::AlgorithmConfig& cfg) {
    // Everything, then the surfaces that have a block. A closed class writes no
    // lines, and `SaveFrom` takes any `[Surface.x]` block that is not in this
    // span back out of the file it is rewriting.
    const auto base = rds::AlgorithmFileParams();
    std::vector<rds::ParamDesc> out{base.begin(), base.end()};
    const auto opened = rds::OpenedSurfaceParams(cfg);
    out.insert(out.end(), opened.begin(), opened.end());
    return out;
}

void App::LoadAlgorithmFile(const fs::path& file, rds::AlgorithmConfig& cfg) {
    // A testbench config is one file where the game splits into two, so it
    // carries the surfaces list inline. Which classes are opened still comes
    // from the `[Surface.x]` headers that are present rather than from a key,
    // for the same reason it does in the game: the file is the list.
    rds::ConfigManager::LoadInto(file, &cfg, rds::AlgorithmFileParams());
    rds::ConfigManager::ReadOpenedSurfaces(file, cfg);
    rds::ConfigManager::LoadInto(file, &cfg, rds::SurfaceParams());
    cfg.surfaces.Resolve();
}

void App::LoadConfigFile(int side, const fs::path& file) {
    ConfigSide& s = m_side[side];
    s.cfg = rds::AlgorithmConfig{};
    LoadAlgorithmFile(file, s.cfg);
    s.cfg.slots.rngSeed = m_seed;
    s.name = file.stem().string();
    std::snprintf(s.saveName, sizeof(s.saveName), "%s", s.name.c_str());
    s.dirty = true;
    s.unsaved = false;
    s.patchNote.clear();  // a loaded file is a new baseline, not that patch any more
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
    rds::ConfigManager::SaveFrom(file, &s.cfg, AlgorithmAndOpenedSurfaces(s.cfg),
                                 "RagdollSounds_Algorithm.ini - written by the testbench");
    s.name = name;
    s.unsaved = false;
    ScanConfigs();
    for (std::size_t i = 0; i < m_configFiles.size(); ++i)
        if (m_configFiles[i] == file) m_configIndex = static_cast<int>(i);
}

void App::LoadCompareFile(int side, const fs::path& file) {
    ConfigSide& s = m_side[side];
    s.compareCfg = rds::AlgorithmConfig{};
    LoadAlgorithmFile(file, s.compareCfg);
    // The seed is this machine's, not the file's: every side is forced onto
    // m_seed so an A/B is the same shuffle twice, and a diff whose first line
    // was "rngSeed differs" would be reporting the testbench to itself.
    s.compareCfg.slots.rngSeed = m_seed;
    s.compareName = file.stem().string();
}

void App::ClearCompare(int side) {
    m_side[side].compareName.clear();
    m_side[side].compareCfg = rds::AlgorithmConfig{};
}

bool App::ParamDiffers(const rds::AlgorithmConfig& a, const rds::AlgorithmConfig& b,
                       const rds::ParamDesc& p) {
    if (p.type == rds::ParamType::kString)
        return rds::GetParamString(&a, p) != rds::GetParamString(&b, p);
    return rds::FormatParam(p, rds::GetParam(&a, p)) != rds::FormatParam(p, rds::GetParam(&b, p));
}

void App::CompareCounts(int side, int& differing, int& same) const {
    differing = 0;
    same = 0;
    const ConfigSide& s = m_side[side];
    if (s.compareName.empty()) return;
    for (const rds::ParamDesc& p : rds::AlgorithmParams()) {
        if (ParamDiffers(s.cfg, s.compareCfg, p))
            ++differing;
        else
            ++same;
    }
}

std::vector<const rds::ParamDesc*> App::CompareDeltas(int side) const {
    std::vector<const rds::ParamDesc*> out;
    const ConfigSide& s = m_side[side];
    if (s.compareName.empty()) return out;
    for (const rds::ParamDesc& p : rds::AlgorithmParams())
        if (ParamDiffers(s.cfg, s.compareCfg, p)) out.push_back(&p);
    return out;
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

    rds::OfflineOptions opt = m_pretend;
    opt.seed = m_seed;
    opt.trace = true;

    m_bank.Seed(m_seed);
    m_recording->Rewind();
    s.result = rds::RunOffline(*m_recording, s.cfg, m_bank, opt);

    // Unioned, never pruned - see m_slotSeen. A slot that fired under some
    // config for this take belongs with the slots this take uses, whether or
    // not it is firing under the config being listened to right now.
    for (const rds::Cue& cue : s.result.cues) {
        const auto index = static_cast<std::size_t>(cue.slot);
        if (index < m_slotSeen.size()) {
            m_slotSeen[index] = true;
        }
    }

    // The cue list is built either way - the timeline, the stats and the verify
    // column are all drawn off `s.result`, and a switch that stopped producing it
    // would blank half the window to change what comes out of the speakers.
    // Only what reaches the player changes.
    if (m_useVanillaAudio && !m_recording->VanillaTrack().empty()) {
        s.audio = std::make_shared<MixedAudio>(
            MixVanilla(m_recording->VanillaTrack(), s.result.audioDurationMs,
                       m_recording->Listener(), m_vanillaLibrary, 48000, m_monitorDb, m_limiter,
                       m_seed, m_vanillaPlayed, m_vanillaMisses));
    } else {
        m_vanillaPlayed = 0;
        m_vanillaMisses = 0;
        s.audio = std::make_shared<MixedAudio>(MixCues(s.result.cues, s.result.audioDurationMs,
                                                       m_recording->Listener(), m_sources, 48000,
                                                       m_monitorDb, m_limiter));
    }
    s.runMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

    // The swap is at the current play position: no restart, no gap.
    m_player.SetBuffer(side, s.audio);
}

void App::RerunDirty() {
    // The vanilla switch changes what is mixed, not what is computed, so it
    // leaves no config dirty of its own. Noticing it here rather than at the
    // checkbox keeps one place responsible for "the buffers are stale", which is
    // the same place the sliders and the sfx table already report to.
    if (m_vanillaAudioApplied != m_useVanillaAudio) {
        m_vanillaAudioApplied = m_useVanillaAudio;
        m_side[0].dirty = true;
        m_side[1].dirty = true;
    }
    for (int side = 0; side < 2; ++side)
        if (m_side[side].dirty) Rerun(side);
}

void App::RunBenchmark() {
    if (!m_recording) return;

    // Pause first, and for a reason beyond tidiness: the audio callback is the
    // one other thread in this program with real work to do, and a device that
    // is mixing a take while the measurement runs puts its block boundaries
    // into every sample. Silence is part of the instrument.
    m_player.Pause();

    for (BenchResult& r : m_bench) r.valid = false;

    BenchOptions opt;
    opt.seed = m_seed;
    opt.trace = false;
    opt.budgetMs = static_cast<double>(m_benchBudgetSec) * 1000.0;
    // The take as the transport has it - trimmed, and pretending whatever it is
    // pretending. Anything else measures a run that is not the one on screen.
    opt.replay = m_pretend;

    // Both sides while split, otherwise only the one being listened to. Two
    // configs measured back to back on a machine whose state has not changed in
    // between is the whole design of this: an absolute figure off a debug build
    // means very little, and the difference between two of them means a lot.
    for (int side = 0; side < 2; ++side) {
        if (!m_split && side != m_focusSide) continue;
        m_bench[side] = RunBench(*m_recording, m_side[side].cfg, m_bank, opt);
        m_bench[side].label = m_side[side].name;

        const BenchResult& r = m_bench[side];
        if (!r.valid) continue;
        spdlog::info(
            "bench {} [{}] on {}: best {:.3f} ms, median {:.3f}, {} runs | {:.0f}x realtime, "
            "{:.2f} us/tick over {} ticks | {} contacts -> {} cues",
            static_cast<char>('A' + side), r.label, m_recording->Info().stem, r.bestMs, r.medianMs,
            r.runs, r.RealtimeFactor(), r.UsPerTick(), r.ticks, r.contactsIn, r.emittedCues);
    }
}

bool App::SyncSfxAudition() {
    const SfxBrowser::Audition want = m_browser.InTakeAudition();
    const int slot = want.active ? static_cast<int>(want.slot) : -1;
    if (slot == m_auditionSlot && want.file == m_auditionFile) {
        return false;
    }
    m_auditionSlot = slot;
    m_auditionFile = want.file;

    if (slot < 0) {
        m_sources.ClearAudition();
    } else {
        m_sources.SetAudition(want.slot, m_library.PathOf(want.file).string());
    }
    // Both sides, and through the same path a slider takes: the cue list has not
    // moved, only what one slot of it sounds like, and the swap lands at the
    // current play position so an auditioned take does not restart under you.
    for (ConfigSide& side : m_side) {
        side.dirty = true;
    }
    return true;
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

bool App::OffsetMeasured() const {
    return m_take >= 0 && m_offsets.Has(m_takes[static_cast<std::size_t>(m_take)].stem);
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
    // All of which assumes the take *is* a cut clip. A devbench take is not: this
    // program drove the recording, so the mp4 is OBS's whole output and its
    // sidecar says as much (RecordingInfo::videoIsWholeOutput). There the sync
    // track's intercept is the offset outright, and a pad that was never applied
    // would be two seconds of pure invention.
    //
    // Vayne_impacts_log_2 is the same case arrived at the other way: 103 s of
    // uncut OBS output carrying seventeen knockdowns, from before anything wrote
    // that down. A clip is longer than its take by twice the pad and an uncut
    // recording is not, so the video being no longer than the take still stands
    // as the tell for a take that says nothing about itself.
    constexpr double kClipPadMs = 2000.0;
    if (!m_videoOffsetKnown && m_video.HasVideo() && m_video.Ready() && m_video.FrameCount() > 0 &&
        m_take >= 0) {
        const double takeMs = m_recording ? m_recording->Info().durationMs : 0.0;
        const bool wholeOutput = m_recording && m_recording->Info().videoIsWholeOutput;
        const bool looksCut = !wholeOutput && m_video.DurationMs() > takeMs + kClipPadMs;
        const char* how = nullptr;
        if (looksCut) {
            m_videoOffsetMs = kClipPadMs;
            how = "a cut clip";
        } else if (m_sync.valid) {
            m_videoOffsetMs = m_sync.intercept;
            how = wholeOutput ? "this take's own recording" : "uncut";
        } else {
            // No fit to lean on. Zero for a take whose video it owns - the file
            // starts where the recording started, and a nudge from zero is a
            // small one; the pad only for the clips it was measured off.
            m_videoOffsetMs = wholeOutput ? 0.0 : kClipPadMs;
            how = wholeOutput ? "this take's own recording, unsynced" : "a cut clip, unsynced";
        }
        spdlog::info("{}: video {:.0f} ms against take {:.0f} ms - treating as {}, offset {:+.0f} ms",
                     m_take >= 0 ? m_takes[static_cast<std::size_t>(m_take)].stem : std::string(),
                     m_video.DurationMs(), takeMs, how, m_videoOffsetMs);
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
    // Side B stops existing when the split is collapsed, so a report left open
    // on it follows the config that survived rather than reading a panel that
    // is no longer on screen.
    if (!m_split) m_compareSide = 0;
    DrawCompareReport(m_compareSide);

    // Outside the main window: it is a real floating window, so it can be moved
    // off the app and left open beside it while a slot is auditioned.
    const SfxBrowser::Pick pick = m_browser.Draw();

    // Before the pick is applied, not after: Draw() has already closed the
    // window if something was chosen, so this drops the audition first and the
    // take is mixed once - with the file in its real place rather than in the
    // override that was standing in for it.
    const bool auditionMoved = SyncSfxAudition();

    // Before the rebuild below, so the bank is loaded from assignments that no
    // longer name anything that has gone.
    const std::vector<std::string> deleted = m_browser.TakeDeleted();
    if (!deleted.empty()) {
        const rds::SfxAssignments before = m_sfx;
        std::size_t slots = 0;
        for (const std::string& file : deleted) {
            slots += m_sfx.Forget(file);
        }
        if (slots != 0) {
            PushSfxEdit(before, std::format("delete {} / off {} slot(s)",
                                            deleted.size() == 1 ? deleted.front()
                                                                : std::format("{} sfx", deleted.size()),
                                            slots));
        }
    }

    if (m_browser.TakeLibraryChanged()) {
        // New files in the library. If nothing is assigned yet this is the
        // adopt, and the names it just brought in are the assignment - so seed
        // from them rather than leaving a full library and an empty panel.
        //
        // Never after a deletion: an empty table there is one somebody just
        // emptied, and seeding it would put back from filenames exactly what
        // they were taking off.
        if (m_sfx.Empty() && deleted.empty()) {
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
            // The mute goes with the sound that was here, the condition stays
            // with the position: a tag is what this row on the slot is *for* -
            // "the stone one" - and changing which recording serves it does not
            // change the job. Both rules live in ReplaceAt.
            assignment.ReplaceAt(static_cast<std::size_t>(pick.variant), pick.file);
            PushSfxEdit(before, std::string(name) + " / change");
        } else {
            // The `+ variant` flow: the grid was answered before the library
            // was opened, and this is where that answer lands. It lands on the
            // new row and nowhere else, so picking a file the slot already
            // plays adds a second, tagged placement of it rather than
            // re-tagging the one that is already there.
            const bool conditional = m_pendingConditionSlot == static_cast<int>(pick.slot) &&
                                     !m_pendingCondition.Unconditional();
            assignment.Add(pick.file, conditional ? m_pendingCondition : rds::VariantCondition{});
            PushSfxEdit(before,
                        conditional
                            ? std::format("{} / add for {} / {}", name,
                                          rds::ToString(m_pendingCondition.surface),
                                          rds::ToString(m_pendingCondition.coverage))
                            : std::string(name) + " / add");
        }
        m_pendingConditionSlot = -1;
    }
    // Last, and unconditional: everything above that re-runs has already cleared
    // the dirty flags, so this is a no-op except on the frames where the
    // highlight moved and nothing else did.
    if (auditionMoved) {
        RerunDirty();
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
            if (!t.hasBodySamples) item += "  [no pose]";
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
        // Not a failure - the take replays exactly as it always did - but air
        // time then has no measurement behind it, and a rule tuned against this
        // take is being tuned against an inference.
        ImGui::SameLine();
        if (!t.hasBodySamples) {
            ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.45f, 1.0f), "| no pose data");
            Tip("Captured before the pose sidecar existed, so there is no <stem>_pose.bin\n"
                "beside it and no per-tick limb positions.\n\n"
                "It still replays and still makes cues. What it cannot do is measure air\n"
                "time: the engine falls back to inferring flight from the gaps between\n"
                "contacts, which reads a body that has merely stopped touching anything as\n"
                "airborne. Re-capture the take to tune anything that depends on it.");
        } else {
            ImGui::TextDisabled("| %zu pose frames", t.poseFrames);
            Tip("Per-tick limb positions and velocities, from <stem>_pose.bin.\n"
                "Air time, ground clearance and body speed are measured on this take\n"
                "rather than inferred from the gaps between contacts.");
        }
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
    ImGui::SameLine();
    ImGui::Checkbox("include configs", &m_exportConfigs);
    Tip("Write the whole config into the export as an ini, every key with the comment\n"
        "that says what it changes - exactly as it stands in memory, saved or not.\n\n"
        "It goes at the end, after everything else, and it is what makes the report\n"
        "readable six saves later: without it the file names a config, and the file of\n"
        "that name is a different config by then. Paste the block into\n"
        "RagdollSounds_Algorithm.ini and the mod plays what the report is about.");

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
    req.configUnsaved = side.unsaved;
    req.includeConfigs = m_exportConfigs;
    req.videoOffsetMs = m_videoOffsetMs;
    req.playheadMs = m_player.PositionMs();
    req.seed = m_seed;
    req.limiter = m_limiter;
    req.side = m_focusSide;
    // Measured, not merely known: the clip-pad guess sets m_videoOffsetKnown on
    // the first frame, so reading that would have every export claim its offset
    // came out of video-offsets.ini. The note under it flips from a caveat to a
    // statement of fact on this flag, and a guess is not a fact.
    req.offsetMeasured = OffsetMeasured();
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
    std::size_t hiddenSelf = 0;
    for (int i = 0; i < static_cast<int>(events.size()); ++i) {
        const rds::FeedEvent& e = events[static_cast<std::size_t>(i)];
        if (e.kind != rds::EventKind::kImpact || e.timeMs < lo || e.timeMs > hi) {
            continue;
        }
        if (e.otherLayer == rds::ColLayer::kDeadBip) {
            ++hiddenSelf;
            if (m_hideSelfImpacts) {
                continue;
            }
        }
        index.push_back(i);
    }
    ImGui::Text("impacts  %zu", index.size());
    Tip("Rows are washed by closing speed read against the rest of the list: red at the\n"
        "hard end of what is on screen, muted green at the soft end, and nothing through\n"
        "the middle. The scale is the visible rows, so narrowing to a region re-reads\n"
        "every row against that region rather than against the whole take.");
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

    ImGui::SameLine();
    ImGui::Checkbox("hide self##impacts", &m_hideSelfImpacts);
    Tip("Drop the limb-on-limb contacts - the \"self\" rows - and leave what the body hit\n"
        "the world with. A ragdoll hits itself constantly and those rows are most of a\n"
        "fall while being almost none of what you are listening for.\n\n"
        "They are only hidden from this table. The engine still sees every one of them,\n"
        "and the contacts lane on the timeline still draws them.");
    if (m_hideSelfImpacts && hiddenSelf != 0) {
        ImGui::SameLine();
        ImGui::TextDisabled("(%zu self)", hiddenSelf);
    }

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

    // Off the filtered set, so the wash answers "hard for this list" - which is what
    // the list is for. Closing speed is the metric because it is the one the engine
    // decides on: the other three columns describe the hit, this one causes it.
    std::vector<double> speeds;
    speeds.reserve(index.size());
    for (const int i : index) speeds.push_back(events[static_cast<std::size_t>(i)].impactSpeed);
    const Spread heat = Spread::Of(std::move(speeds));

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
            // RowBg1, not RowBg0: the playhead owns RowBg0 a few lines down and the
            // wash is faint enough to read the blue through.
            if (const ImU32 tint = IntensityTint(heat.At(e.impactSpeed))) {
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, tint);
            }
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
            if (limb) {
                ImGui::Text("%s", limb->boneName.c_str());
            } else {
                // Deliberately not the raw limbIndex: a take whose CSV wrote "-"
                // for the index parses back as 0, so printing a number here
                // would label every row "limb 0" and read as data. "?" is the
                // honest answer, and the tooltip says whose fault it is.
                ImGui::TextDisabled("?");
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip(
                        "No actor profile for %08X, so this take has no limb table.\n"
                        "The game sends one profile per actor per attach; if the actor\n"
                        "was already attached when recording started, the take is\n"
                        "written without it.",
                        static_cast<std::uint32_t>(e.actorId));
                }
            }
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
    Tip("Rows are washed by gain read against the rest of the list: red at the loudest\n"
        "end of what is on screen, muted green at the quietest, and nothing through the\n"
        "middle. Gain rather than intensity because gain is what you hear - intensity is\n"
        "one of the things that set it, and it is in the column beside it.\n\n"
        "A gain followed by a bracketed figure is a cue its class's compressor held down,\n"
        "and the bracket is how many dB it lost.");
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
    if (!ImGui::BeginTable("cuetbl", 9, kFlags)) {
        return;
    }
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("t", ImGuiTableColumnFlags_DefaultSort, 0, 0);
    ImGui::TableSetupColumn("slot", ImGuiTableColumnFlags_None, 0, 1);
    // Which of the slot's files this cue actually chose. The slot column names
    // the role and the variant behind it is a shuffle bag, a stable token or a
    // pin depending on the config, so "imp_body" on its own does not say what
    // was heard - and "that one sounded wrong" is always about a file.
    ImGui::TableSetupColumn("sfx", ImGuiTableColumnFlags_None, 0, 8);
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
            case 6: return static_cast<double>(c.motion) * 2.0 + static_cast<double>(c.moment);
            // Slot first, then variant inside it: sorting by the file is
            // sorting the take into "every cue that played this wav", which is
            // the question that column exists for.
            case 8: return static_cast<double>(c.slot) * 256.0 + c.variant;
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

    std::vector<double> gains;
    gains.reserve(index.size());
    for (const int i : index) gains.push_back(cues[static_cast<std::size_t>(i)].gainDb);
    const Spread heat = Spread::Of(std::move(gains));

    const double nowMs = m_player.PositionMs();
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(index.size()));
    while (clipper.Step()) {
        for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
            const int i = index[static_cast<std::size_t>(r)];
            const rds::Cue& c = cues[static_cast<std::size_t>(i)];
            const std::string_view slot = rds::ToString(c.slot);
            const std::string_view reason = rds::ToString(c.reason);
            const std::string_view phase = rds::ToString(c.motion, c.moment);

            ImGui::TableNextRow();
            if (const ImU32 tint = IntensityTint(heat.At(c.gainDb))) {
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, tint);
            }
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
            const ImU32 cap = SurfaceCapColour(c.slot);
            const ImU32 nameCol = cap != 0 ? cap : CueColour(c.slot, c.reason);
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(nameCol), "%.*s",
                               static_cast<int>(slot.size()), slot.data());

            ImGui::TableNextColumn();
            const CueSound snd = SoundOf(c);
            if (snd.silent) {
                ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.40f, 1.0f), "%s", snd.label.c_str());
            } else if (snd.forced) {
                ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.45f, 1.0f), "%s", snd.label.c_str());
            } else {
                ImGui::TextUnformatted(snd.label.c_str());
            }
            Tip(snd.file.empty()
                    ? std::string("No recording behind this slot and none behind anything it "
                                  "falls back to, so this cue is silent. Nothing is synthesised "
                                  "to cover it: the mix you are hearing is the mix that ships.")
                    : std::format("{}\nvariant {} of {}{}\n\n{}{}", snd.file,
                                  snd.variant + 1, snd.variantCount,
                                  snd.forced ? ", pinned - the slot is not picking" : "",
                                  FallbackNote(c.slot, snd),
                                  snd.variantCount > 1 && !snd.forced
                                      ? "One of several. Which one a cue gets is the shuffle bag, "
                                        "or the contact's own token with stable variants on."
                                      : "The only file this slot has."));

            ImGui::TableNextColumn();
            if (c.compressCutDb < 0.0f) {
                // The number in this cell is what plays; the bracket is what the
                // compressor took, which is the difference between a cue that is
                // quiet and a cue that is being held quiet.
                ImGui::TextColored(ImVec4(1.0f, 0.80f, 0.51f, 1.0f), "%.1f (-%.1f)", c.gainDb,
                                   -c.compressCutDb);
                Tip(std::format("Compress:{} is {:.1f} dB and this layer came in over it, "
                                "so the {:.0f}:1 ratio took {:.1f} dB off: it would have played "
                                "at {:.1f} dB and is playing at {:.1f}.\n\n"
                                "Per layer, so the other layers of this composite may have been "
                                "held by a different amount or not at all.",
                                rds::ToString(c.compressBand),
                                rds::CompressThresholdDb(s.cfg.compress, c.compressBand),
                                s.cfg.compress.ratio, -c.compressCutDb,
                                c.gainDb - c.compressCutDb, c.gainDb));
            } else {
                ImGui::Text("%.1f", c.gainDb);
            }
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
    // saying so and the button that fixes it. What it does *not* get is a
    // different clock. Direct mode seeks index/30 s into the mp4 and the cache
    // is that same mp4 decoded at 30 fps from zero, so the two index the same
    // timeline and a nudge measured through one is a nudge measured through the
    // other. Previewing unbuilt takes at raw take time threw the whole per-take
    // offset away - 3044-3683 ms on the clipped takes - and left the nudge
    // slider visibly doing nothing while every export still applied it.
    if (m_video.Unbuilt()) {
        ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.45f, 1.0f), "unbuilt take - one seek per frame");
        Tip("The picture is pulled straight out of the mp4, a frame at a time, so scrubbing\n"
            "shows the last frame it managed until the new one lands. It is lined up the\n"
            "same way a built take is; it simply arrives slower.");
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

    // Whether the picture is lined up is a different question from whether it
    // was decoded, and it is this one. Until a take has a line in
    // video-offsets.ini its offset is the clip-pad guess, which is 1-1.7 s out
    // on the takes where it has been measured.
    if (m_video.HasVideo() && !OffsetMeasured()) {
        ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.45f, 1.0f), "video offset is a guess");
        Tip("Nobody recorded where this clip was cut out of the longer OBS recording, so\n"
            "the offset under the timeline started at a guess. Nudge it until a landing\n"
            "lands on the frame it lands on and it is written down for good.");
    }

    if (!m_video.HasVideo() && m_video.FrameCount() == 0) {
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        ImGui::SetCursorPos(ImVec2(avail.x * 0.5f - 90.0f, avail.y * 0.5f - 8.0f));
        ImGui::TextDisabled("no video for this take");
    } else if (!m_video.Ready()) {
        ImGui::TextDisabled("%s", m_video.Status());
    } else {
        // The same mapping the exports and both video cuts use. One clock, so
        // what you select against is what you get.
        const double vt = VideoTimeMs(m_player.PositionMs());
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

    DrawSimulateRow();

    UpdateCueWindow();
    DrawBenchmarkRow();

    if (m_video.HasVideo()) {
        float nudge = static_cast<float>(m_videoOffsetMs);
        ImGui::SetNextItemWidth(260.0f);
        // Up the whole clip, not a flat six seconds. A cut clip's offset is a
        // keyframe interval or two, but a take that owns its recording carries
        // however long the Record button was down before anybody fell over -
        // fifteen seconds is ordinary, and a slider that cannot reach it is a
        // slider that cannot line the take up at all.
        const float reach =
            std::max(6000.0f, static_cast<float>(m_video.Ready() ? m_video.DurationMs() : 0.0));
        if (ImGui::SliderFloat("video offset ms", &nudge, -6000.0f, reach, "%.0f")) {
            m_videoOffsetMs = nudge;
            if (m_take >= 0) m_offsets.Set(m_takes[static_cast<std::size_t>(m_take)].stem, m_videoOffsetMs);
        }
        Tip("video_time_ms = slope * t_ms + offset.\n"
            "Both come from fitting the low-rtt rows of the take's _sync.csv.\n\n"
            "On a take the devbench recorded, the mp4 is OBS's own output and the fit's\n"
            "intercept is the offset outright - there should be nothing left to nudge.\n\n"
            "On the takes in Research/, the mp4 was cut out of a longer recording whose\n"
            "cut point is recorded nowhere, so only the slope is usable and the offset is\n"
            "yours to set. Either way it persists per take once you touch it.");
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
        const bool wholeOutput = m_recording && m_recording->Info().videoIsWholeOutput;
        if (m_sync.valid)
            ImGui::TextDisabled("sync: %d/%d rows, min rtt %.0f ms, drift %+.2f ms/s | %s %.0f ms | %s",
                                m_sync.rowsUsed, m_sync.rowsTotal, m_sync.minRttMs, m_sync.driftMsPerSec,
                                wholeOutput ? "recording" : "clip",
                                m_video.Ready() ? m_video.DurationMs() : 0.0, m_video.Status());
        else if (wholeOutput)
            // Not a complaint: OBS started and stopped for this take, so its
            // video begins where the take does and the offset near zero is
            // right. Only the drift is missing.
            ImGui::TextDisabled("no _sync.csv - this take's own recording, so zero is the offset | %s",
                                m_video.Status());
        else
            ImGui::TextDisabled("no _sync.csv - the offset is the whole story here | %s", m_video.Status());
    }
}

// ── the cue window ────────────────────────────────────────────────────────
//
// Two switches on one measurement: where this take's cues actually are.
//
// A take is a recording of a fall, not a piece of music. It starts when the
// ragdoll does and ends whenever the capture was stopped, and the interesting
// part is a second and a half somewhere in the middle. Tuning is a loop of
// "move a slider, listen again", so silence at either end is paid for on every
// pass over it.
//
// Both switches keep a pad on either side of every cue. Landing exactly on the
// first sample of a cue is worse than useless: an impact with no run-up does
// not read as an impact, and the sub arrives 65 ms after the transient, so a
// cut that lands mid-stack takes the weight off the very thing being judged.

namespace {
/// Silence kept either side of a cue. Enough to hear one coming and to let the
/// late sub finish, short enough not to be the wait it is removing.
constexpr double kCuePadMs = 500.0;

/// A gap has to be this long before it is worth jumping. Under it the skip
/// would save less than the pads it has to leave, and a transport that lurches
/// to save 200 ms is a transport you stop trusting about where you are.
constexpr double kCuelessGapMs = 2000.0;
}  // namespace

void App::UpdateCueWindow() {
    const std::vector<rds::Cue>& cues = m_side[m_player.ActiveSide()].result.cues;

    // Cleared rather than left stale when the switch is off or the take has
    // nothing in it. Bounds that outlived their reason would trim a take with
    // no cues down to a window computed from the last take that had some.
    if (!m_limitToCues || cues.empty()) {
        m_player.SetPlayBounds(0.0, 0.0);
    } else {
        double first = cues.front().timeMs;
        double last = cues.front().timeMs;
        for (const rds::Cue& c : cues) {
            first = std::min(first, c.timeMs);
            last = std::max(last, c.timeMs);
        }
        m_player.SetPlayBounds(std::max(0.0, first - kCuePadMs),
                               std::min(m_player.DurationMs(), last + kCuePadMs));
    }

    if (!m_skipCueless || !m_player.Playing() || cues.empty()) {
        return;
    }

    // Inside a drawn region the region is the answer, so a skip may only move
    // within it. Without that, selecting one impact and pressing play would
    // jump straight out of the thing that was selected.
    double lo = 0.0;
    double hi = std::max(1.0, m_player.DurationMs());
    if (m_player.HasRegion()) {
        lo = m_player.LoopStartMs();
        hi = m_player.LoopEndMs();
    } else if (m_player.HasPlayBounds()) {
        lo = m_player.PlayStartMs();
        hi = m_player.PlayEndMs();
    }

    const double now = m_player.PositionMs();
    if (now < lo || now > hi) {
        return;
    }

    // The cue behind and the cue ahead. Scanned rather than assumed sorted:
    // the list is built in time order today, and a skip that trusted that and
    // was wrong would jump backwards - the one failure that would be read as a
    // bug in the audio rather than in the transport.
    double behind = -1e18;
    double ahead = 1e18;
    for (const rds::Cue& c : cues) {
        if (c.timeMs < lo || c.timeMs > hi) continue;
        if (c.timeMs <= now)
            behind = std::max(behind, c.timeMs);
        else
            ahead = std::min(ahead, c.timeMs);
    }
    if (ahead > 1e17) {
        return;  // nothing left ahead - the tail is the loop's business
    }

    // Measured from the window edge when nothing is behind yet, so the run-in
    // to the first cue is jumped on the same rule as every gap after it.
    const double from = behind < -1e17 ? lo : behind;
    const double target = ahead - kCuePadMs;
    if (ahead - from <= kCuelessGapMs || now < from + kCuePadMs || now >= target) {
        return;
    }
    m_player.SeekMs(target);
}

// ── the benchmark ────────────────────────────────────────────────────────────
//
// Under the transport because it is a transport control: it stops playback and
// then replays the same take the scrub bar is sitting in, which is the same
// thing the Play button does with a different clock. Its answer belongs where
// the thing it measured is.

void App::DrawSimulateRow() {
    // Every take in the corpus was recorded on one floor wearing one thing, and
    // three of the four armour classes have no capture at all - so without this
    // the armour feature cannot be heard, let alone judged. The engine is not
    // told: the surface goes in as a MATERIAL_ID and the wardrobe through the
    // actor profile, exactly as the game supplies them.
    ImGui::TextDisabled("simulate");
    Tip("Replay this take as if it happened somewhere else, in something else. Nothing here is "
        "saved and nothing here reaches the game - it rewrites what the engine is fed, not what "
        "the engine does. Both A/B sides share it, so a comparison is still two configs in one "
        "world.");

    ImGui::SameLine();
    ImGui::SetNextItemWidth(150.0f);
    const char* kSurfaces[] = {"soft", "wood", "stone", "metal", "water", "body"};
    int surfaceIndex = m_pretend.surfaceAs == rds::SurfaceClass::kCount
                           ? -1
                           : static_cast<int>(m_pretend.surfaceAs);
    const char* surfaceLabel = surfaceIndex < 0 ? "as recorded" : kSurfaces[surfaceIndex];
    if (ImGui::BeginCombo("##pretendSurface", surfaceLabel)) {
        if (ImGui::Selectable("as recorded", surfaceIndex < 0)) {
            m_pretend.surfaceAs = rds::SurfaceClass::kCount;
            MarkPretendDirty();
        }
        for (int i = 0; i < IM_ARRAYSIZE(kSurfaces); ++i) {
            if (ImGui::Selectable(kSurfaces[i], surfaceIndex == i)) {
                m_pretend.surfaceAs = static_cast<rds::SurfaceClass>(i);
                MarkPretendDirty();
            }
        }
        ImGui::EndCombo();
    }
    Tip("Forces every world contact onto this floor. Self- and body-contacts are left alone: they "
        "are routed by which limb was hit, and dressing one up as stone would move it into a "
        "different branch of ingest - a behaviour change that has nothing to do with the floor.");

    ImGui::SameLine();
    ImGui::SetNextItemWidth(150.0f);
    const char* kCoverage[] = {"bare", "clothed", "light", "heavy"};

    // The all-sites control. It reads "mixed" when the per-site expander has
    // been used, because with six sites disagreeing there is no one answer and
    // showing one of them would be a lie about the other five.
    int allIndex = -1;
    bool mixed = false;
    for (std::size_t i = 0; i < std::size(m_pretend.coverageSet); ++i) {
        const int here = m_pretend.coverageSet[i] ? static_cast<int>(m_pretend.coverageAs[i]) : -1;
        if (i == 0) {
            allIndex = here;
        } else if (here != allIndex) {
            mixed = true;
        }
    }
    const char* armourLabel = mixed              ? "mixed"
                              : allIndex < 0     ? "as recorded"
                                                 : kCoverage[allIndex];
    if (ImGui::BeginCombo("##pretendArmour", armourLabel)) {
        if (ImGui::Selectable("as recorded", !mixed && allIndex < 0)) {
            for (bool& set : m_pretend.coverageSet) {
                set = false;
            }
            MarkPretendDirty();
        }
        for (int i = 0; i < IM_ARRAYSIZE(kCoverage); ++i) {
            if (ImGui::Selectable(kCoverage[i], !mixed && allIndex == i)) {
                for (std::size_t s = 0; s < std::size(m_pretend.coverageSet); ++s) {
                    m_pretend.coverageSet[s] = true;
                    m_pretend.coverageAs[s] = static_cast<rds::Coverage>(i);
                }
                MarkPretendDirty();
            }
        }
        ImGui::EndCombo();
    }
    Tip("Dresses every body site at once. Use the per-site expander for the case this cannot "
        "express - heavy boots on an otherwise naked body, which is the one that actually tests "
        "the per-limb rule.");

    ImGui::SameLine();
    if (ImGui::TreeNodeEx("per limb", ImGuiTreeNodeFlags_NoTreePushOnOpen)) {
        for (std::size_t s = 0; s < std::size(m_pretend.coverageSet); ++s) {
            const auto site = static_cast<rds::CoverageSite>(s);
            const std::string name{rds::ToString(site)};
            ImGui::SetNextItemWidth(130.0f);
            const int index =
                m_pretend.coverageSet[s] ? static_cast<int>(m_pretend.coverageAs[s]) : -1;
            const char* label = index < 0 ? "as recorded" : kCoverage[index];
            if (ImGui::BeginCombo(("##pretend_" + name).c_str(), label)) {
                if (ImGui::Selectable("as recorded", index < 0)) {
                    m_pretend.coverageSet[s] = false;
                    MarkPretendDirty();
                }
                for (int i = 0; i < IM_ARRAYSIZE(kCoverage); ++i) {
                    if (ImGui::Selectable(kCoverage[i], index == i)) {
                        m_pretend.coverageSet[s] = true;
                        m_pretend.coverageAs[s] = static_cast<rds::Coverage>(i);
                        MarkPretendDirty();
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(name.c_str());
        }
    }

    if (m_pretend.Pretending()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.35f, 1), "PRETENDING");
        Tip("This take is not being replayed as it was recorded. Every level you are judging is "
            "being judged in a world that did not happen.");
    }
}

void App::MarkPretendDirty() {
    // The take changed, not a config, so both sides re-run and re-mix at the
    // current play position exactly as they do for any other edit.
    m_side[0].dirty = true;
    m_side[1].dirty = true;
}

void App::DrawBenchmarkRow() {
    ImGui::BeginDisabled(!m_recording);
    if (ImGui::Button("benchmark", ImVec2(110, 0))) RunBenchmark();
    ImGui::EndDisabled();
    Tip("Pauses playback and replays this take through the backend as fast as it will go, "
        "reporting the fastest run.\n\n"
        "Tracing is off for it - the game never traces, and the testbench's own "
        "\"RunOffline + mix\" figure beside the stats is mostly the cost of filling the "
        "timeline. So this number is lower than that one on purpose, and it is the one that "
        "says something about the mod.\n\n"
        "The window freezes while it runs. That is deliberate: measuring against a UI that is "
        "drawing at the same time measures the UI.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    ImGui::SliderFloat("##benchbudget", &m_benchBudgetSec, 0.1f, 5.0f, "%.1f s each");
    Tip("How long to spend measuring, per config. Longer is a tighter number and a longer "
        "freeze. While split A/B, both sides are measured, so the freeze is twice this.");
    ImGui::SameLine();
    if (m_split)
        ImGui::TextDisabled("| A and B, back to back");
    else
        ImGui::TextDisabled("| side %c only - turn on split A/B to compare two configs",
                            'A' + m_focusSide);

    // The cue window, beside the benchmark because both are about the same
    // thing: how long you wait to hear the answer to a change you just made.
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::Checkbox("limit playback to cues", &m_limitToCues);
    Tip("Play from 500 ms before the first cue to 500 ms after the last one, instead of\n"
        "the whole recording. Most takes are mostly silence - the ragdoll starts when the\n"
        "capture does and the body lies still long after the last sound - and on a tuning\n"
        "loop that silence is paid for on every pass.\n\n"
        "A drawn loop region still wins: it is the more specific answer to the same\n"
        "question, and it is the one you asked for by hand.");
    ImGui::SameLine();
    ImGui::Checkbox("skip cueless sections", &m_skipCueless);
    Tip("Jump the gaps inside a take as well as the ones at its ends. A stretch with more\n"
        "than 2 s of nothing in it is skipped from 500 ms after the last cue to 500 ms\n"
        "before the next one.\n\n"
        "The pads are the point. Landing on the first sample of an impact is worse than\n"
        "useless - a hit with no run-up does not read as a hit, and the sub arrives 65 ms\n"
        "after the transient, so a cut that lands mid-stack takes the weight off exactly\n"
        "what you were listening for.\n\n"
        "Inside a loop region it only ever moves within that region.");

    for (int side = 0; side < 2; ++side) {
        const BenchResult& r = m_bench[side];
        if (!r.valid) continue;

        ImGui::TextDisabled(
            "  %c %s: %.3f ms/run  |  %sx realtime  |  %.2f us/tick over %u ticks  |  %u "
            "contacts -> %u cues  |  best of %d, median %.3f",
            'A' + side, r.label.c_str(), r.bestMs, Compact(r.RealtimeFactor()).c_str(),
            r.UsPerTick(), r.ticks, r.contactsIn, r.emittedCues, r.runs, r.medianMs);
        Tip(std::format(
            "best {:.3f} ms, median {:.3f}, mean {:.3f}, worst {:.3f} over {} runs in {:.0f} ms.\n\n"
            "A tick is one game frame, so {:.2f} us/tick is what a frame of this fall would cost "
            "the mod - the per-run figure carries this take's own length in it and does not "
            "transfer.\n\n"
            "median/best is {:.2f}. Much over 1.15 and something else was running on the machine, "
            "which means the two sides were not measured under the same conditions - run it again "
            "before believing a small difference.",
            r.bestMs, r.medianMs, r.meanMs, r.worstMs, r.runs, r.wallMs, r.UsPerTick(), r.Spread()));

        // The delta, which is the entire point. Against A because A is the side
        // that was there first; the sign says which way, so it reads the same
        // whichever one you were editing.
        if (side == 1 && m_bench[0].valid && m_bench[0].bestMs > 0.0) {
            const double pct = (r.bestMs / m_bench[0].bestMs - 1.0) * 100.0;
            ImGui::SameLine();
            // Under a percent is inside the noise of a best-of on a machine
            // that is also running Skyrim, and colouring it would be claiming a
            // difference that is not there.
            const ImVec4 col = std::fabs(pct) < 1.0   ? ImVec4(0.6f, 0.6f, 0.6f, 1.0f)
                               : pct > 0.0            ? ImVec4(1.0f, 0.6f, 0.35f, 1.0f)
                                                      : ImVec4(0.5f, 0.9f, 0.5f, 1.0f);
            ImGui::TextColored(col, "  B is %+.1f%% vs A", pct);
            Tip("Cost, not quality. A config that is cheaper because arbitration threw more "
                "away is cheaper for a reason you can hear.");
        }
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
        "timeline - colour is the slot (the impact stack shares a hue and separates by "
        "lightness; a surface skin is grey with its material on top), height is gain, a ghost "
        "tick above a bar is the height "
        "it would have had before its class was compressed, a translucent ramp behind everything "
        "is a loop's envelope. Along the foot of the lane: a pale blue bar brackets one "
        "burst (cues closer together than Arbitration:fBurstMinGapMs), gold above it is a "
        "hero moment with a light tick where it re-anchored, lavender above that is "
        "measured flight, and teal above that is a slide - its end cap coloured by how the "
        "slide ended, lavender for launched, red for struck, grey for come to rest. Wheel "
        "zooms, middle drags, left drags a selection, bottom strip drags a loop region - by an "
        "edge to resize, from inside to move");
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

    // With a slot's widget hovered in the sfx panel, that slot's cues are drawn
    // full height and everything else is dimmed - which answers "what does this
    // slot actually do in this take" by pointing at it rather than by reading a
    // count. Read by the envelopes below as well as by the cue bars, so a
    // hovered scrape loop lights up its ramp and not only its three bars.
    const bool highlighting = m_hoverSlot >= 0;

    // Whether this slot is drawn at all, off the sfx panel's `timeline` boxes.
    // Everything below that puts ink on the cue lane asks this first, and
    // nothing that computes a decision does: the burst brackets and the three
    // state lanes are properties of arbitration and of the body, not of any
    // slot, so hiding a scrape does not move a bracket or shorten a slide.
    const auto drawn = [this](rds::SlotId slot) {
        const auto index = static_cast<std::size_t>(slot);
        return index >= m_slotHidden.size() || !m_slotHidden[index];
    };

    // ── the loop envelopes ───────────────────────────────────────────────────
    //
    // A loop is not an event, and drawing it as a row of bars was the lane
    // telling a small lie about the biggest thing in a slide. What the engine
    // actually emits is a start, a handful of updates whenever the level moves
    // more than 0.75 dB, and a stop - and as bars those read as five unrelated
    // impacts with a gap in the middle. What they *are* is one voice fading in,
    // tracking the slide, and fading out: a ramp. So it is drawn as one.
    //
    // Underneath everything else and translucent, because the one-shots riding
    // on top of a slide are the thing being tuned and the envelope is the
    // context they are being read against. The top edge is drawn straight
    // between updates rather than as a staircase: the voice does step, but the
    // steps are a 0.75 dB quantisation of a level that is genuinely continuous,
    // and the shape is the honest half of that.
    {
        struct LoopRun {
            std::uint32_t voice{};
            rds::CueReason reason{};
            rds::SlotId slot{};
            float startFadeMs{};
            float stopFadeMs{};
            double stopMs{-1.0};
            std::vector<ImVec2> pts;  ///< x is time in ms, y is gain in dB
        };
        std::vector<LoopRun> runs;
        for (const rds::Cue& c : s.result.cues) {
            if (c.op == rds::CueOp::kPlayOneShot) continue;
            auto it = std::ranges::find_if(runs,
                                           [&](const LoopRun& r) { return r.voice == c.voiceId; });
            if (it == runs.end()) {
                runs.push_back(LoopRun{c.voiceId, c.reason, c.slot, c.fadeMs});
                it = runs.end() - 1;
            }
            // A stop cue carries no gain - there is nothing left to set - so its
            // level is whatever the trims add up to, which on this lane draws as
            // a full-height bar at the end of the slide. The envelope ends at
            // the floor over the stop's own fade instead, which is what the
            // voice does.
            if (c.op == rds::CueOp::kStopLoop) {
                it->stopMs = c.timeMs;
                it->stopFadeMs = c.fadeMs;
            } else {
                it->pts.push_back(ImVec2(static_cast<float>(c.timeMs), c.gainDb));
            }
        }

        const float base = cueBot;
        for (const LoopRun& run : runs) {
            if (run.pts.empty() || !drawn(run.slot)) continue;
            const bool lit = highlighting && static_cast<int>(run.slot) == m_hoverSlot;
            const ImU32 rgb = CueColour(run.slot, run.reason) & 0x00FFFFFFu;
            const ImU32 fill =
                rgb | (static_cast<ImU32>(highlighting && !lit ? 14 : 46) << IM_COL32_A_SHIFT);
            const ImU32 edge =
                rgb | (static_cast<ImU32>(highlighting && !lit ? 40 : 150) << IM_COL32_A_SHIFT);

            // The fade in, the updates, then the fade out - or a hold to the end
            // of the take for a loop that never stopped, which is a slide the
            // recording was cut in the middle of rather than one with no end.
            std::vector<ImVec2> top;
            top.reserve(run.pts.size() + 3);
            top.push_back(ImVec2(X(run.pts.front().x), base));
            for (std::size_t i = 0; i < run.pts.size(); ++i) {
                // The first point is where the fade in *ends*: the start cue
                // asks for a level and a fade to reach it over, and drawing it
                // as an instant step would put a wall at the front of every
                // slide that the engine went to some trouble not to have.
                const double atMs =
                    run.pts[i].x + (i == 0 ? static_cast<double>(run.startFadeMs) : 0.0);
                top.push_back(ImVec2(X(atMs), base - BarHeight(run.pts[i].y) * (cueH - 6.0f)));
            }
            const double endMs = run.stopMs >= 0.0 ? run.stopMs : dur;
            top.push_back(ImVec2(X(endMs), top.back().y));
            if (run.stopMs >= 0.0) {
                top.push_back(ImVec2(X(run.stopMs + run.stopFadeMs), base));
            }

            for (std::size_t i = 1; i < top.size(); ++i) {
                const ImVec2& a = top[i - 1];
                const ImVec2& b = top[i];
                if (b.x <= a.x) continue;
                // One quad per segment rather than one polygon: the envelope is
                // not convex and AddConvexPolyFilled would fold it inside out.
                dl->AddQuadFilled(a, b, ImVec2(b.x, base), ImVec2(a.x, base), fill);
            }
            dl->AddPolyline(top.data(), static_cast<int>(top.size()), edge, ImDrawFlags_None,
                            1.5f);
        }
    }

    // ── what the foot lanes are, for the hover ───────────────────────────────
    //
    // Every mark along the bottom of the cue lane is three pixels tall and
    // carries no label, and the four of them are between them the only things on
    // this timeline that cannot be looked up in a table afterwards: a burst is a
    // property of the rhythm, a hero window and a flight are properties of the
    // actor rather than of any cue, and how a slide ended is written nowhere
    // else at all. The legend above says what the colours mean; it cannot say
    // what *this* mark is. So each span is recorded as it is drawn.
    //
    // Geometry and a tag rather than a formatted string: this runs for every
    // span of every take on every frame and at most one of them is ever hovered,
    // so the words are built at the point of asking.
    struct LaneSpan {
        enum class Kind : std::uint8_t { kBurst, kHero, kFlight, kSlide };
        Kind kind{};
        double startMs{};
        double endMs{};
        float top{};
        float bottom{};
        int count{};  ///< burst: cues inside it. hero: re-anchors. otherwise 0.
        rds::SlideExit exit{};
    };
    std::vector<LaneSpan> laneSpans;

    // burst brackets: cues separated by less than the config's burst gap are one
    // burst, which is the rhythm the whole design is aiming at.
    const double burstGap = s.cfg.arb.burstMinGapMs;
    double burstStart = -1.0, burstEnd = -1.0;
    int burstCount = 0;
    auto flushBurst = [&] {
        if (burstStart >= 0.0 && burstCount > 0) {
            dl->AddRectFilled(ImVec2(X(burstStart) - 2.0f, cueBot - 3.0f), ImVec2(X(burstEnd) + 2.0f, cueBot),
                              IM_COL32(120, 200, 255, 110));
            laneSpans.push_back(LaneSpan{LaneSpan::Kind::kBurst, burstStart, burstEnd,
                                         cueBot - 3.0f, cueBot, burstCount, {}});
        }
        burstStart = burstEnd = -1.0;
        burstCount = 0;
    };
    for (const rds::Cue& c : s.result.cues) {
        // Loops are excluded because a texture that lasts is not a burst. On the
        // op and not on the reason: the catches a slide is punctuated by carry
        // kScrape too, and they are one-shots that belong in the rhythm - which
        // is the whole point of building them.
        if (c.op != rds::CueOp::kPlayOneShot) continue;
        if (burstStart < 0.0 || c.timeMs - burstEnd > burstGap) {
            flushBurst();
            burstStart = c.timeMs;
        }
        burstEnd = c.timeMs;
        ++burstCount;
    }
    flushBurst();

    // ── the garment ──────────────────────────────────────────────────────────
    //
    // Drawn as a curve and not as a span, because a loop is not an event - the
    // same reason the cue lane draws the grind's ramp rather than a row of bars.
    // Under everything else, because it is a bed and reads as one.
    //
    // Two lines, and the second is the point: the filled area is the smoothed
    // level the voice actually plays, and the thin line over it is the raw
    // per-tick measurement. **The gap between them is the attack and the
    // release**, which is the only way to set those two by eye - watch the raw
    // drive spike on a stair tread and the fill decay into the next one.
    //
    // Skipped entirely when nothing moved, so a take recorded before the layer
    // existed - or one with the feature off, which is the default - gets no ink
    // rather than a flat line along the bottom asserting silence.
    {
        float peak = 0.0f;
        for (const rds::BodySample& b : s.result.body) {
            peak = std::max(peak, std::max({b.rustleDrive, b.rustleDriveRaw, b.motionViolence}));
        }
        if (peak > 0.001f) {
            // A third of the lane. Subordinate on purpose: it is the quietest
            // thing in the mix and should not read as competing with the cues.
            const float h = cueH * 0.33f;
            constexpr ImU32 kFill = IM_COL32(196, 158, 116, 70);
            constexpr ImU32 kEdge = IM_COL32(214, 176, 132, 150);
            constexpr ImU32 kRaw = IM_COL32(236, 206, 150, 190);
            constexpr ImU32 kViolence = IM_COL32(150, 200, 214, 190);

            const auto Y = [&](float v) { return cueBot - std::clamp(v, 0.0f, 1.0f) * h; };

            for (std::size_t i = 1; i < s.result.body.size(); ++i) {
                const rds::BodySample& a = s.result.body[i - 1];
                const rds::BodySample& b = s.result.body[i];
                const float xa = X(a.timeMs);
                const float xb = X(b.timeMs);
                // Quads per segment rather than one filled polygon: the envelope
                // is not convex and ImGui's fill would close it across its own
                // dips.
                dl->AddQuadFilled(ImVec2(xa, cueBot), ImVec2(xa, Y(a.rustleDrive)),
                                  ImVec2(xb, Y(b.rustleDrive)), ImVec2(xb, cueBot), kFill);
                dl->AddLine(ImVec2(xa, Y(a.rustleDrive)), ImVec2(xb, Y(b.rustleDrive)), kEdge,
                            1.0f);
                dl->AddLine(ImVec2(xa, Y(a.rustleDriveRaw)), ImVec2(xb, Y(b.rustleDriveRaw)), kRaw,
                            1.0f);
                // The damage rule's violence window, in a colder colour so it
                // does not read as a third opinion about the garment. It is the
                // same measurement averaged instead of enveloped, and averaged
                // only over the ticks with no collision in them - so where it
                // sits *below* a tall raw spike, that gap is the impact being
                // deliberately excluded from its own context.
                dl->AddLine(ImVec2(xa, Y(a.motionViolence)), ImVec2(xb, Y(b.motionViolence)),
                            kViolence, 1.0f);
            }
        }
    }

    // ── the state lanes ────────────────────────────────────────────────
    //
    // Two stripes along the foot of the cue lane, above the burst brackets:
    // when the mix was in a hero moment, and when the body was off the ground.
    //
    // Both are read off the sampled engine state rather than inferred from the
    // cues, because neither can be inferred from the cues. A hero window is a
    // decision about the actor and not a property of any one of them - the
    // loudest cue in a take is regularly not inside one - and flight is a
    // measurement that leaves no cue at all. They are also the two things the
    // Stage 2 rewrite turns on, so being able to see where they actually land
    // is most of being able to say whether it worked.
    //
    // End caps on every span, because the question is where one starts and
    // stops and a plain bar reads as shading rather than as an interval.
    {
        // One drawing routine, two lanes: what differs between them is which
        // field opens a span, and that belongs in the loop rather than in a
        // second copy of the geometry.
        const auto lane = [&](LaneSpan::Kind kind, float bottom, ImU32 colour, ImU32 tickColour,
                              auto&& openIn, auto&& startOf, auto&& markerOf) {
            const float top = bottom - 3.0f;
            double spanStart = -1.0;
            double spanEnd = -1.0;
            std::uint32_t spanMark = 0;
            std::vector<double> ticks;
            const auto flush = [&] {
                if (spanStart < 0.0) return;
                // A span that opened and closed inside one tick still gets a
                // mark: it happened, and a lane that dropped it would be
                // quietly arguing that it did not.
                const float a = X(spanStart);
                const float b = std::max(X(spanEnd), a + 2.0f);
                dl->AddRectFilled(ImVec2(a, top), ImVec2(b, bottom), colour);
                dl->AddLine(ImVec2(a, top - 3.0f), ImVec2(a, bottom + 1.0f), colour, 1.5f);
                dl->AddLine(ImVec2(b, top - 3.0f), ImVec2(b, bottom + 1.0f), colour, 1.5f);
                for (const double at : ticks) {
                    dl->AddLine(ImVec2(X(at), top - 2.0f), ImVec2(X(at), bottom), tickColour,
                                1.0f);
                }
                laneSpans.push_back(LaneSpan{kind, spanStart, spanEnd, top, bottom,
                                             static_cast<int>(ticks.size()), {}});
                ticks.clear();
                spanStart = spanEnd = -1.0;
            };
            for (const rds::BodySample& b : s.result.body) {
                if (!openIn(b)) {
                    flush();
                    continue;
                }
                const double began = startOf(b) > 0.0 ? startOf(b) : b.timeMs;
                if (spanStart < 0.0) {
                    spanStart = began;
                    spanMark = markerOf(b);
                } else if (markerOf(b) != spanMark) {
                    ticks.push_back(began);
                    spanMark = markerOf(b);
                }
                spanEnd = b.timeMs;
            }
            flush();
        };

        // Hero in gold - the mix axis, and the thing the design is loudest
        // about. A tick inside a span is a re-anchor: the window stayed open
        // and found a bigger contact to sit on, so it is one moment with peers
        // rather than two moments running together.
        lane(
            LaneSpan::Kind::kHero, cueBot - 4.0f, IM_COL32(255, 205, 70, 210),
            IM_COL32(255, 245, 200, 230),
            [](const rds::BodySample& b) { return b.moment == rds::Moment::kHero; },
            [](const rds::BodySample& b) { return b.heroSinceMs; },
            [](const rds::BodySample& b) { return b.heroSeq; });

        // Flight in lavender, echoing air_whoosh, which is the one cue this
        // lane is the cause of. Empty on a take with no pose sidecar - there is
        // no measurement to draw, and a flat "never airborne" would be a claim
        // rather than an absence. No re-anchor equivalent: a flight is one
        // continuous stretch by construction, so the marker never changes.
        lane(
            LaneSpan::Kind::kFlight, cueBot - 8.0f, IM_COL32(175, 160, 255, 205),
            IM_COL32(175, 160, 255, 205),
            [](const rds::BodySample& b) { return b.airborne && b.haveBodySamples; },
            [](const rds::BodySample& b) { return b.airborneSinceMs; },
            [](const rds::BodySample&) { return 0u; });

        // Slide in teal, and its own loop for one reason: how a slide *ended* is
        // most of what there is to know about it, and the generic lane above can
        // only draw a span. A slide leaves the state three ways - the body came
        // to rest, the body left the ground, or something stopped it - and they
        // sound nothing like each other. A lane that drew only the bar would be
        // showing the least interesting half.
        //
        // The exit is read off the first sample *after* the span, because that
        // is where it is written: `LeaveSlide` sets it on the same tick the
        // motion state changes, so no sample inside the span can carry it.
        {
            const float bottom = cueBot - 12.0f;
            const float top = bottom - 3.0f;
            constexpr ImU32 kSlideBody = IM_COL32(110, 205, 190, 200);
            const auto capOf = [](rds::SlideExit exit) -> ImU32 {
                switch (exit) {
                    // The flight lane's own lavender, because that is where the
                    // body went and the two marks line up on screen.
                    case rds::SlideExit::kLaunched: return IM_COL32(175, 160, 255, 240);
                    // The graze stream dried up, which is a fact about the
                    // contact data rather than about the body - so a neutral
                    // cap, not an alarm colour.
                    case rds::SlideExit::kEnded: return IM_COL32(150, 165, 185, 225);
                    case rds::SlideExit::kNone: break;
                }
                return kSlideBody;
            };

            double spanStart = -1.0;
            double spanEnd = -1.0;
            const auto flush = [&](rds::SlideExit exit) {
                if (spanStart < 0.0) return;
                const float a = X(spanStart);
                const float b = std::max(X(spanEnd), a + 2.0f);
                dl->AddRectFilled(ImVec2(a, top), ImVec2(b, bottom), kSlideBody);
                dl->AddLine(ImVec2(a, top - 3.0f), ImVec2(a, bottom + 1.0f), kSlideBody, 1.5f);
                // The closing cap is drawn thicker and taller than the opening
                // one: the two ends of a slide are not equally interesting, and
                // this is the one the rework is about.
                dl->AddLine(ImVec2(b, top - 5.0f), ImVec2(b, bottom + 1.0f), capOf(exit), 2.5f);
                laneSpans.push_back(
                    LaneSpan{LaneSpan::Kind::kSlide, spanStart, spanEnd, top, bottom, 0, exit});
                spanStart = spanEnd = -1.0;
            };
            for (const rds::BodySample& b : s.result.body) {
                if (b.motion == rds::Motion::kSlide) {
                    if (spanStart < 0.0) {
                        spanStart = b.motionEnteredMs > 0.0 ? b.motionEnteredMs : b.timeMs;
                    }
                    spanEnd = b.timeMs;
                } else {
                    flush(b.slideExit);
                }
            }
            // A slide the recording was cut in the middle of has no exit to draw,
            // which is not the same as one that ended and is marked as neither.
            flush(rds::SlideExit::kNone);
        }
    }

    // the cues
    for (int i = 0; i < static_cast<int>(s.result.cues.size()); ++i) {
        const rds::Cue& c = s.result.cues[static_cast<std::size_t>(i)];
        if (!drawn(c.slot)) continue;
        const float x = X(c.timeMs);
        // A stop cue has no gain of its own - there is nothing left to set - so
        // its level is whatever the trims happen to add up to, which drew as a
        // full-height bar at the end of every slide. It is a marker, not a
        // level, so it is drawn as one: a stub on the baseline, at the point
        // where the envelope behind it has already reached the floor.
        const float h = c.op == rds::CueOp::kStopLoop
                            ? 5.0f
                            : BarHeight(c.gainDb) * (cueH - 6.0f);
        const bool lit = highlighting && static_cast<int>(c.slot) == m_hoverSlot;
        ImU32 col = CueColour(c.slot, c.reason);
        if (highlighting && !lit) {
            // Dimmed in place rather than hidden: the rhythm of the take is the
            // context that makes one slot's placement mean anything.
            col = (col & 0x00FFFFFFu) | (static_cast<ImU32>(45) << IM_COL32_A_SHIFT);
        }
        const bool sel = i == m_selectedCue;
        const float halfWidth = lit ? 2.5f : (sel ? 2.0f : 1.0f);
        dl->AddRectFilled(ImVec2(x - halfWidth, cueBot - h), ImVec2(x + halfWidth, cueBot), col);
        if (const ImU32 cap = SurfaceCapColour(c.slot); cap != 0) {
            // A grey bar with the floor on top of it. The skins are one role
            // and read as one lane of grey; the cap is the only thing that
            // differs between them, so it is the only thing given a hue.
            const float capH = std::min(4.0f, std::max(2.0f, h * 0.35f));
            const ImU32 tinted = (cap & 0x00FFFFFFu) | (col & IM_COL32_A_MASK);
            dl->AddRectFilled(ImVec2(x - halfWidth, cueBot - h),
                              ImVec2(x + halfWidth, cueBot - h + capH), tinted);
        }
        if (lit) {
            dl->AddLine(ImVec2(x, cueTop), ImVec2(x, cueBot), col, 1.0f);
            dl->AddCircleFilled(ImVec2(x, cueTop + 4.0f), 2.5f, col);
        }
        if (c.compressCutDb < 0.0f) {
            // Where the bar would have reached, and a hairline down to where it
            // actually does. Height is gain everywhere else on this lane, so a
            // held cue is drawn at a height that is not its own and the lane
            // would quietly lie about the top of the range; the gap is the
            // compression, which is the one thing a number in a table cannot
            // show you across a whole take at once.
            const float top = cueBot - h;
            const float ghost = cueBot - BarHeight(c.gainDb - c.compressCutDb) * (cueH - 6.0f);
            const int alpha = highlighting && !lit ? 55 : 200;
            dl->AddLine(ImVec2(x - halfWidth - 2.5f, ghost), ImVec2(x + halfWidth + 2.5f, ghost),
                        IM_COL32(255, 205, 130, alpha), 1.5f);
            dl->AddLine(ImVec2(x, ghost), ImVec2(x, top), IM_COL32(255, 205, 130, alpha / 2), 1.0f);
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
        const float ra = X(m_player.LoopStartMs());
        const float rb = X(m_player.LoopEndMs());
        dl->AddRectFilled(ImVec2(ra, stripTop), ImVec2(rb, stripTop + regionStripH),
                          IM_COL32(90, 140, 220, 190));
        // Two grips, because the edges are draggable and nothing else about the
        // strip says so. Drawn inward so a region a few pixels wide still shows
        // both rather than one bar of the wrong colour.
        dl->AddRectFilled(ImVec2(ra, stripTop), ImVec2(ra + 2.0f, stripTop + regionStripH),
                          IM_COL32(200, 225, 255, 230));
        dl->AddRectFilled(ImVec2(rb - 2.0f, stripTop), ImVec2(rb, stripTop + regionStripH),
                          IM_COL32(200, 225, 255, 230));
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

    // Nothing is hovered mid-gesture: a drag is already saying what it is doing,
    // and a tooltip following the cursor through it is in the way of the thing
    // being drawn.
    const bool inspecting =
        hovered && !m_selecting && !m_maybeSelecting && !m_regionDragging;

    // ── the foot lanes ───────────────────────────────────────────────────────
    //
    // They answer first inside their own sixteen pixels. A short cue bar can
    // reach down here and lose its hover to a lane, which is the right way round
    // to lose it: a cue has a table row, an export line and a click that selects
    // it, and a lane mark has none of those - the hover is its only route.
    const LaneSpan* laneHit = nullptr;
    if (inspecting && mouse.y < traceTop) {
        for (const LaneSpan& span : laneSpans) {
            // The same two pixels the marks are drawn with, so the target is
            // what is on screen rather than what is in the data.
            if (mouse.y < span.top - 1.0f || mouse.y > span.bottom + 1.0f) continue;
            const float a = X(span.startMs) - 2.0f;
            const float b = std::max(X(span.endMs), a + 2.0f) + 2.0f;
            if (mouse.x >= a && mouse.x <= b) {
                laneHit = &span;
                break;
            }
        }
    }
    if (laneHit != nullptr) {
        const double lengthMs = laneHit->endMs - laneHit->startMs;
        ImGui::BeginTooltip();
        switch (laneHit->kind) {
            case LaneSpan::Kind::kBurst:
                ImGui::TextColored(ImVec4(0.47f, 0.78f, 1.0f, 1.0f), "burst   %d cues",
                                   laneHit->count);
                ImGui::TextDisabled("%.0f - %.0f ms   (%.0f ms)", laneHit->startMs, laneHit->endMs,
                                    lengthMs);
                TipBody("Cues closer together than Arbitration:fBurstMinGapMs are one burst, and "
                        "the arbitrator picks bursts rather than sounds: three to five grains "
                        "inside 200-400 ms, then near-silence. How many grains fit comes from the "
                        "motion state's budget, and the hero latch overrides that while its "
                        "window is open. The loops and the bed are not counted - they are not "
                        "onsets.");
                break;
            case LaneSpan::Kind::kHero:
                ImGui::TextColored(ImVec4(1.0f, 0.80f, 0.27f, 1.0f), "hero moment   %.0f ms",
                                   lengthMs);
                if (laneHit->count > 0) {
                    ImGui::TextDisabled("%.0f - %.0f ms   %d re-anchor%s", laneHit->startMs,
                                        laneHit->endMs, laneHit->count,
                                        laneHit->count == 1 ? "" : "s");
                } else {
                    ImGui::TextDisabled("%.0f - %.0f ms", laneHit->startMs, laneHit->endMs);
                }
                TipBody("The mix axis. While the window is open the actor spends the hero budget "
                        "instead of the motion state's, and with "
                        "Arbitration:bSpatialCollapseOnHero every layer is placed at one point so "
                        "the whole thing reads as one event with detail.");
                if (laneHit->count > 0) {
                    TipBody("A light tick inside the span is a re-anchor: a contact at "
                            "Hero:fReanchorRatio of the window's own anchor speed took the moment "
                            "over, restarting the window and moving the collapse point. That is "
                            "what makes a landing one moment with peers rather than three events "
                            "running together.");
                }
                break;
            case LaneSpan::Kind::kFlight:
                ImGui::TextColored(ImVec4(0.69f, 0.63f, 1.0f, 1.0f), "measured flight   %.0f ms",
                                   lengthMs);
                ImGui::TextDisabled("%.0f - %.0f ms", laneHit->startMs, laneHit->endMs);
                TipBody("Downward acceleration past Motion:fFreeFallFrac of gravity, measured off "
                        "the pose sidecar rather than inferred from the gap since the last "
                        "contact. This is what the hero test's arrival clause and the Tumble -> "
                        "Airborne edge read, and both switch off rather than guess on a take with "
                        "no pose - which is why this lane is empty on the older captures instead "
                        "of claiming the body never left the ground.");
                break;
            case LaneSpan::Kind::kSlide: {
                const std::string_view exitName = rds::ToString(laneHit->exit);
                ImGui::TextColored(ImVec4(0.43f, 0.80f, 0.75f, 1.0f), "slide   %.0f ms", lengthMs);
                ImGui::TextDisabled("%.0f - %.0f ms   ended: %.*s", laneHit->startMs,
                                    laneHit->endMs, static_cast<int>(exitName.size()),
                                    exitName.data());
                switch (laneHit->exit) {
                    case rds::SlideExit::kLaunched:
                        TipBody("The body left the ground, which is why this cap is the flight "
                                "lane's own lavender - the two marks line up. The loop fades over "
                                "the shorter ScrapeLoop:fLaunchFadeMs instead: the surface is "
                                "simply gone, and the ordinary fade drags a grinding rumble out "
                                "behind a body already in the air.");
                        break;
                    case rds::SlideExit::kEnded:
                        TipBody("The body stopped: the graze stream dried up and, where "
                                "Motion:fSlideHoldSpeed is set, the body had slowed under it. The "
                                "loop fades over ScrapeLoop:fStopFadeMs. A collision arriving in "
                                "this tick is an ordinary contact and is judged like one - the "
                                "slide-end lift that used to make it bigger is gone, because it "
                                "was standing in for an exit test the body's own speed now "
                                "provides.");
                        break;
                    case rds::SlideExit::kNone:
                        TipBody("No exit: the recording ends inside this slide. Not the same as a "
                                "slide that ended and was marked as neither - there is nothing to "
                                "draw rather than nothing to say.");
                        break;
                }
                break;
            }
        }
        ImGui::EndTooltip();
    }

    // What the cursor is over.
    //
    // A cue is a bar two pixels wide and cannot carry a label, so "what is that
    // one" has to be a hover. It answers with the same three things the cue
    // table's row does - the level, the file it resolved to, and whether the
    // ceiling held it - because the lane and the table disagreeing about one cue
    // is worse than neither of them saying anything.
    if (laneHit == nullptr && inspecting && mouse.y < traceTop) {
        int nearest = -1;
        double nearestDx = 1e18;
        for (int i = 0; i < static_cast<int>(s.result.cues.size()); ++i) {
            // Hidden slots are skipped rather than dimmed: a tooltip for a bar
            // that is not on the lane is the timeline answering a question
            // nobody could have asked it.
            if (!drawn(s.result.cues[static_cast<std::size_t>(i)].slot)) continue;
            const double dx =
                std::fabs(s.result.cues[static_cast<std::size_t>(i)].timeMs - mouseMs);
            if (dx < nearestDx) {
                nearestDx = dx;
                nearest = i;
            }
        }
        // The same six pixels the region grips use, so pointing at something is
        // the same gesture at every zoom.
        if (nearest >= 0 && nearestDx / viewSpan * width < 6.0) {
            const rds::Cue& c = s.result.cues[static_cast<std::size_t>(nearest)];
            const CueSound snd = SoundOf(c);
            const std::string_view slotName = rds::ToString(c.slot);
            const std::string_view reasonName = rds::ToString(c.reason);
            ImGui::BeginTooltip();
            ImGui::Text("%.0f ms   %.*s   %.1f dB   pitch %.2f", c.timeMs,
                        static_cast<int>(slotName.size()), slotName.data(), c.gainDb, c.pitch);
            if (snd.variantCount > 1) {
                ImGui::Text("%s   (variant %d of %d%s)", snd.label.c_str(), snd.variant + 1,
                            snd.variantCount, snd.forced ? ", forced" : "");
            } else {
                ImGui::Text("%s%s", snd.label.c_str(), snd.forced ? "   (forced)" : "");
            }
            if (snd.fellBack) {
                const std::string_view playsName = rds::ToString(snd.plays);
                ImGui::TextDisabled("nothing recorded for this slot - playing %.*s",
                                    static_cast<int>(playsName.size()), playsName.data());
            }
            if (c.compressCutDb < 0.0f) {
                const std::string_view bandKey = rds::ToString(c.compressBand);
                ImGui::TextColored(ImVec4(1.0f, 0.80f, 0.51f, 1.0f),
                                   "compressed %.1f dB at %.0f:1 - Compress:%.*s is %.1f and this "
                                   "layer was over it, so it plays at %.1f instead of %.1f",
                                   -c.compressCutDb, s.cfg.compress.ratio,
                                   static_cast<int>(bandKey.size()), bandKey.data(),
                                   rds::CompressThresholdDb(s.cfg.compress, c.compressBand),
                                   c.gainDb, c.gainDb - c.compressCutDb);
            }
            ImGui::TextDisabled("%.*s   intensity %.2f   seq %u",
                                static_cast<int>(reasonName.size()), reasonName.data(), c.intensity,
                                c.sourceSeq);
            ImGui::EndTooltip();
        }
    }

    // Six pixels either side, in milliseconds, so the grab is the same size on
    // screen at every zoom - it is a target for a mouse, not a stretch of the
    // take. One number for every lane and for the region edges, because
    // pointing at something should be the same gesture wherever you do it.
    const double grabMs = 6.0 / static_cast<double>(std::max(1.0f, width)) * viewSpan;

    // ── the trace lane ───────────────────────────────────────────────────────
    //
    // Every red tick here is a contact the engine decided not to play, and the
    // whole point of the lane is that they outnumber the green ones ten to one.
    // Which rule dropped which one is the question the lane raises and cannot
    // answer on its own, so it answers it here rather than sending you to the
    // export for a line you are already looking at.
    if (inspecting && m_showTrace && mouse.y >= traceTop && mouse.y < contactTop &&
        !s.result.trace.empty()) {
        const rds::TraceRecord* best = nullptr;
        double bestDx = 1e18;
        for (const rds::TraceRecord& t : s.result.trace) {
            const double dx = std::fabs(t.timeMs - mouseMs);
            if (dx < bestDx) {
                bestDx = dx;
                best = &t;
            }
        }
        if (best != nullptr && bestDx <= grabMs) {
            const rds::ActorProfile* profile =
                m_recording ? m_recording->Profile(best->actorId) : nullptr;
            const rds::LimbInfo* limb = profile ? profile->Limb(best->limbIndex) : nullptr;
            const bool emitted = std::strstr(best->outcome, "emit") != nullptr;
            const std::string_view motionName = rds::ToString(best->motion);
            const std::string_view momentName = rds::ToString(best->moment);
            ImGui::BeginTooltip();
            ImGui::Text("%.0f ms   %s   seq %u", best->timeMs, limb ? limb->boneName.c_str() : "?",
                        best->sourceSeq);
            ImGui::TextColored(emitted ? ImVec4(0.43f, 0.86f, 0.51f, 1.0f)
                                       : ImVec4(1.0f, 0.51f, 0.43f, 1.0f),
                               "%s", best->outcome);
            ImGui::TextDisabled("%.1f u/s closing   intensity %.2f   %.*s / %.*s",
                                best->impactSpeed, best->intensity,
                                static_cast<int>(motionName.size()), motionName.data(),
                                static_cast<int>(momentName.size()), momentName.data());
            TipBody("Every contact that reached arbitration and what became of it. Green played, "
                    "red was dropped - and the design wants most of them red: against 30-60 "
                    "collisions a knockdown is four to six audible moments, so this lane against "
                    "the bars above it is the 10:1 reduction as a picture.");
            ImGui::EndTooltip();
        }
    }

    // ── the contacts lane ────────────────────────────────────────────────────
    //
    // Upstream of everything: what the feed carried, before ingest had an
    // opinion about any of it. Reading a gap in the cue lane means knowing
    // whether there was nothing there or nothing survived, and those are the
    // two different answers this lane and the one above it give.
    if (inspecting && m_showContacts && m_recording && mouse.y >= contactTop &&
        mouse.y < stripTop) {
        const rds::FeedEvent* best = nullptr;
        double bestDx = 1e18;
        for (const rds::FeedEvent& e : m_recording->Events()) {
            if (e.kind != rds::EventKind::kImpact) continue;
            const double dx = std::fabs(e.timeMs - mouseMs);
            if (dx < bestDx) {
                bestDx = dx;
                best = &e;
            }
        }
        if (best != nullptr && bestDx <= grabMs) {
            const rds::ActorProfile* profile = m_recording->Profile(best->actorId);
            const rds::LimbInfo* limb = profile ? profile->Limb(best->limbIndex) : nullptr;
            const bool self = best->otherLayer == rds::ColLayer::kDeadBip;
            ImGui::BeginTooltip();
            ImGui::Text("%.0f ms   %s   seq %u", best->timeMs, limb ? limb->boneName.c_str() : "?",
                        best->sourceSeq);
            ImGui::TextDisabled("closing %.1f   tangent %.1f   body %.1f   angular %.1f",
                                best->impactSpeed, best->tangentSpeed, best->bodySpeed,
                                best->angularSpeed);
            if (self) {
                ImGui::TextColored(ImVec4(0.65f, 0.68f, 0.78f, 1.0f), "limb on limb");
                TipBody("A self-contact. Half of all contacts are one limb touching another limb "
                        "of the same body, and an arm brushing your own thigh makes no impact "
                        "sound - so ingest routes these to the foley bed rather than the impact "
                        "path, and a high threshold lets a genuine self-hit through.");
            } else {
                ImGui::TextColored(ImVec4(0.65f, 0.68f, 0.78f, 1.0f), "limb on world");
                TipBody("A raw contact off the feed, before ingest. Most of them never reach "
                        "arbitration at all: the speed floor, the blow-up test, the mirrored copy "
                        "of a self-hit and the manifold collapse all run before the trace lane "
                        "above has anything to say.");
            }
            ImGui::EndTooltip();
        }
    }

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

    // Which gesture a press at `atMs` on the strip is. A lambda rather than a
    // function because both the press and the hover cursor have to agree about it.
    const auto RegionDragAt = [&](double atMs, double slack) {
        if (!m_player.HasRegion()) return RegionDrag::kNew;
        const double lo = m_player.LoopStartMs();
        const double hi = m_player.LoopEndMs();
        if (std::fabs(atMs - lo) <= slack || std::fabs(atMs - hi) <= slack) {
            return RegionDrag::kEdge;
        }
        return atMs > lo && atMs < hi ? RegionDrag::kMove : RegionDrag::kNew;
    };

    // The cursor is the whole of the affordance: the strip looks the same
    // everywhere, so what a press is about to do has to be said before it lands.
    if (hovered && mouse.y >= stripTop && !m_regionDragging) {
        const RegionDrag at = RegionDragAt(mouseMs, grabMs);
        switch (at) {
            case RegionDrag::kEdge: ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW); break;
            case RegionDrag::kMove: ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll); break;
            case RegionDrag::kNew: break;
        }
        // And the same thing in words. The cursor says which of the three
        // gestures a press is about to be, which is the urgent half; what a
        // region is *for* is the half that has to be discovered, and a strip
        // that looks identical everywhere has nowhere else to say it.
        ImGui::BeginTooltip();
        switch (at) {
            case RegionDrag::kEdge:
                ImGui::TextColored(ImVec4(0.55f, 0.75f, 1.0f, 1.0f), "resize the loop region");
                break;
            case RegionDrag::kMove:
                ImGui::TextColored(ImVec4(0.55f, 0.75f, 1.0f, 1.0f), "move the loop region");
                break;
            case RegionDrag::kNew:
                ImGui::TextColored(ImVec4(0.55f, 0.75f, 1.0f, 1.0f),
                                   m_player.HasRegion() ? "drag a new loop region"
                                                        : "drag a loop region");
                break;
        }
        if (m_player.HasRegion()) {
            ImGui::TextDisabled("%.0f - %.0f ms   (%.0f ms)", m_player.LoopStartMs(),
                                m_player.LoopEndMs(), m_player.LoopEndMs() - m_player.LoopStartMs());
        }
        TipBody("Playback loops it, the impacts and cues tables narrow to it, the stats panel "
                "counts inside it and an export covers it. Drag an edge to resize, drag from "
                "inside to move, drag anywhere else to draw a new one. Clicking a bare strip "
                "clears it - tapping a region you already have does not, because a gesture that "
                "changed its mind should not cost you the region.");
        if (zoomed) {
            TipBody("The pale blue bar along the bottom edge is where the view sits in the whole "
                    "take, which is a different thing from the region: the wheel moves that one "
                    "and it changes nothing about what is measured.");
        }
        ImGui::EndTooltip();
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
            m_regionDragKind = RegionDragAt(mouseMs, grabMs);

            switch (m_regionDragKind) {
                case RegionDrag::kEdge:
                    // Anchored on the edge that is not moving, so the rest of the
                    // gesture is the same code a fresh drag runs.
                    m_regionAnchorMs = std::fabs(mouseMs - m_dragPrevStartMs) <=
                                               std::fabs(mouseMs - m_dragPrevEndMs)
                                           ? m_dragPrevEndMs
                                           : m_dragPrevStartMs;
                    break;
                case RegionDrag::kMove:
                    m_regionGrabOffsetMs = mouseMs - m_dragPrevStartMs;
                    m_regionGrabSpanMs = m_dragPrevEndMs - m_dragPrevStartMs;
                    break;
                case RegionDrag::kNew:
                    // Only this one collapses the region on the press. The other
                    // two are adjusting what is there and it has to stay on
                    // screen while they do.
                    m_regionAnchorMs = mouseMs;
                    m_player.SetLoopRegion(mouseMs, mouseMs);
                    break;
            }
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
                // Same reason as the hover: clicking must not select a bar that
                // is not there and light up its row in the table.
                if (!drawn(s.result.cues[static_cast<std::size_t>(i)].slot)) continue;
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
        // Where the region is this frame. Moving slides a fixed span under the
        // cursor and clamps as a whole, so dragging it off either end parks it
        // against that end at its own length instead of squashing it.
        double lo = 0.0;
        double hi = 0.0;
        if (m_regionDragKind == RegionDrag::kMove) {
            lo = std::clamp(mouseMs - m_regionGrabOffsetMs, 0.0,
                            std::max(0.0, dur - m_regionGrabSpanMs));
            hi = lo + m_regionGrabSpanMs;
        } else {
            lo = std::min(m_regionAnchorMs, mouseMs);
            hi = std::max(m_regionAnchorMs, mouseMs);
        }

        // The region tracks the mouse the whole way - you watch the selection
        // grow - but WindowMs() reports the pre-drag window until the button
        // comes up, so every table below holds still until you are done.
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            m_player.SetLoopRegion(lo, hi);
        } else {
            m_regionDragging = false;
            // A press and release in one spot is a click, not a zero-width
            // region: it clears. The strip is where you go to change the
            // region, so it is also where you go to stop having one - but only
            // when the press was drawing a new one. Tapping an edge or the
            // middle of a region you already have is a gesture that changed its
            // mind, and losing the region for it would be the opposite of what
            // grabbing it was for.
            const bool click = m_regionDragKind == RegionDrag::kNew && (hi - lo) <= 20.0;
            // Put the pre-drag region back first, so SetRegion records the step
            // against what was there before the gesture rather than against the
            // half-finished selection the drag left behind.
            m_player.SetLoopRegion(m_dragPrevStartMs, m_dragPrevEndMs);
            const char* label = m_regionDragKind == RegionDrag::kMove  ? "move region"
                                : m_regionDragKind == RegionDrag::kEdge ? "resize region"
                                                                        : "loop region";
            SetRegion(click ? 0.0 : lo, click ? 0.0 : hi, click ? "clear region" : label);
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

    DrawBodyReadout(s);
}

// ── the measured body ────────────────────────────────────────────────────
//
// Under the timeline rather than in the stats panel, because it is read while
// looking at the timeline: a cue is selected, and the question is what the body
// was doing when it fired. In the stats panel that is a glance away and a
// number that has to be held in the head on the way back.
//
// It follows the playhead, which is also the selection - clicking a cue on the
// timeline or in the table seeks to it - so "the selected frame" and "where the
// playhead is" are one place and need only one readout.

void App::DrawBodyReadout(const ConfigSide& side) {
    if (side.result.body.empty()) {
        ImGui::TextDisabled("body - no run yet");
        return;
    }

    // The last sample at or before the playhead, not the nearest. A tick is a
    // frame of state that held until the next one, so the value in force at
    // 100 ms is the one sampled at 96 ms - and rounding forward would show a
    // measurement taken after the cue being looked at.
    const double now = m_player.PositionMs();
    const rds::BodySample* found = nullptr;
    for (const rds::BodySample& b : side.result.body) {
        if (b.timeMs > now) break;
        found = &b;
    }
    if (found == nullptr) {
        found = &side.result.body.front();
    }

    if (!found->haveBodySamples) {
        ImGui::TextDisabled("body - this take has no pose data, so there is nothing to "
                            "measure");
        Tip("Every field here comes from the <take>_pose.bin sidecar. Takes captured before\n"
            "it existed still replay - the engine falls back on every rule that reads the\n"
            "body - but the measurement itself is simply not in the file.");
        return;
    }

    ImGui::TextDisabled("body");
    ImGui::SameLine();
    // The speed in white and everything else dimmed: one of these is the number
    // being looked for and the rest is the context that makes it mean something.
    ImGui::TextColored(ImVec4(0.90f, 0.93f, 1.0f, 1.0f), "%.0f u/s", found->speed);
    Tip("The magnitude of the ragdoll's mass-weighted centre-of-mass velocity at this\n"
        "frame, read off the engine's own state rather than recomputed here - so it is\n"
        "the same number the rules were reading when they fired.");
    ImGui::SameLine();
    ImGui::TextDisabled("| vert %+.0f u/s  accel %+.0f u/s2  | z %.0f", found->verticalSpeed,
                        found->verticalAccel, found->comPosition.z);
    Tip("Vertical speed, its rate of change, and the height of the centre of mass.\n\n"
        "The acceleration is what the free-fall gate reads: in free flight it sits near\n"
        "-686 u/s2 (9.8 m/s2 x 69.99 u/m), and anything holding the body up pulls it\n"
        "toward zero. That is why nothing here needs a ground reference, and why it\n"
        "survives a staircase.");
    ImGui::SameLine();
    if (found->airborne) {
        ImGui::TextColored(ImVec4(0.60f, 0.85f, 1.0f, 1.0f), "| airborne, dropped %.0f",
                           found->fallDropUnits);
        Tip("Nothing is holding the body up at this frame. The drop is measured from where\n"
            "the flight began, so it is relative and a staircase does not confuse it.");
    } else {
        ImGui::TextDisabled("| supported");
        Tip("Something is holding the body up at this frame - the floor, a step, a wall.");
    }
    ImGui::SameLine();
    ImGui::TextDisabled("| %.*s / %.*s @ %.0f ms",
                        static_cast<int>(rds::ToString(found->motion).size()),
                        rds::ToString(found->motion).data(),
                        static_cast<int>(rds::ToString(found->moment).size()),
                        rds::ToString(found->moment).data(), found->timeMs);
    Tip("Stage 2's two axes at this frame, and the tick the numbers were sampled on.\n"
        "Motion is what the body is doing and physics owns it; Moment is what the mix is\n"
        "doing and design owns it. A quiet-looking contact inside a hero moment comes out\n"
        "loud, and this is the line that says so.");
    if (found->slideExit != rds::SlideExit::kNone) {
        ImGui::SameLine();
        ImGui::TextDisabled("| last slide %.*s",
                            static_cast<int>(rds::ToString(found->slideExit).size()),
                            rds::ToString(found->slideExit).data());
        Tip("How the last slide ended, which is the only part of one the motion state\n"
            "cannot say. Going airborne is measured, and so is coming to rest once\n"
            "Motion:fSlideHoldSpeed is set - under it the grind outlives its own grazes\n"
            "and ends with the body rather than with the contact stream.");
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
    Tip("Peak is the hardest closing speed the take carried, and it is the number every\n"
        "gate is a fraction of - Intensity:fSpeedRefHigh is what the mod calls loud, and a\n"
        "take peaking well under it is a take nothing will reach the top of the range on.\n\n"
        "The run time is this replay plus the mix, not the engine's cost in the game. It is\n"
        "mostly the cost of filling the timeline: for what a frame actually costs, use the\n"
        "benchmark button under the transport.");

    // The bank's per-slot resolution: "imp_sub: 0/2 files, nothing to play" is
    // the line that explains a thin mix, and it belongs on screen for the same
    // reason it belongs in the log.
    int withFiles = 0, declared = 0, fellBack = 0;
    for (const rds::SlotDesc& d : rds::Slots()) {
        if (d.expectedVariants == 0) continue;
        ++declared;
        if (m_bank.FileCount(d.id) > 0) {
            ++withFiles;
        } else if (m_bank.FileCount(m_bank.PlaysAs(d.id)) > 0) {
            // Empty, but not silent and not synthesised: the surface-coloured
            // scrapes play the grind they are a variant of. Counted apart from
            // both, because a slot that needs a recording and a slot that is
            // waiting for an optional one are different jobs.
            ++fellBack;
        }
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

    const int silentSlots = declared - withFiles - fellBack;
    if (silentSlots > 0) {
        ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.40f, 1.0f),
                           "bank: %d of %d slots have wav files, %d play a fallback slot's, %d "
                           "have nothing to play",
                           withFiles, declared, fellBack, silentSlots);
    } else {
        ImGui::TextDisabled("bank: %d of %d slots have wav files, %d play a fallback slot's",
                            withFiles, declared, fellBack);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        for (const rds::SlotDesc& d : rds::Slots()) {
            if (d.expectedVariants == 0) continue;
            const rds::SlotId plays = m_bank.PlaysAs(d.id);
            const bool falls = plays != d.id && m_bank.FileCount(plays) > 0;
            const std::string_view playsName = rds::ToString(plays);
            ImGui::Text("%.*s: %zu/%u files%s%.*s", static_cast<int>(d.name.size()), d.name.data(),
                        m_bank.FileCount(d.id), d.expectedVariants,
                        m_bank.FileCount(d.id) ? "" : (falls ? ", plays " : ", nothing to play"),
                        falls ? static_cast<int>(playsName.size()) : 0, playsName.data());
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
                const rds::SlotId plays = m_bank.PlaysAs(d.id);
                const bool falls = plays != d.id && m_bank.FileCount(plays) > 0;
                const std::string_view playsName = rds::ToString(plays);
                ImGui::SetTooltip("%.*s\n%zu/%u wav files%s%.*s",
                                  static_cast<int>(d.character.size()), d.character.data(),
                                  m_bank.FileCount(d.id), d.expectedVariants,
                                  m_bank.FileCount(d.id) ? ""
                                                         : (falls ? " - nothing recorded, playing "
                                                                  : " - nothing to play"),
                                  falls ? static_cast<int>(playsName.size()) : 0, playsName.data());
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
    const CueSound snd = SoundOf(c);
    ImGui::Text("cue %d @ %.0f ms   %.*s v%u   %.1f dB   pitch %.3f", m_selectedCue, c.timeMs,
                static_cast<int>(slot.size()), slot.data(), c.variant, c.gainDb, c.pitch);
    ImGui::SameLine();
    ImGui::TextDisabled("| %s%s", snd.label.c_str(), snd.forced ? " (forced)" : "");
    Tip(snd.file.empty() ? std::string("Silent - this slot has no recording and nothing to fall "
                                       "back on.")
                         : std::format("{}  -  variant {} of {}{}", snd.file, snd.variant + 1,
                                       snd.variantCount, FallbackNote(c.slot, snd)));
    if (c.compressCutDb < 0.0f) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.80f, 0.51f, 1.0f), "| compressed -%.1f dB",
                           -c.compressCutDb);
        Tip(std::format("Compress:{} is {:.1f} dB and THIS LAYER's level was over it, so the "
                        "{:.0f}:1 ratio took {:.1f} dB.\n\nThe cut is taken per layer against "
                        "that layer's own level, so the rest of this composite came down by "
                        "different amounts or not at all - the stack is reshaped rather than "
                        "moved, which is what lets the body be held while the transient is left "
                        "alone.\n\nThe threshold is measured before any trim, on the scale where "
                        "0 dB is the loudest contact the engine can hear - so turning the master "
                        "gain up moves this cue and the point it starts being held together.",
                        rds::ToString(c.compressBand),
                        rds::CompressThresholdDb(s.cfg.compress, c.compressBand),
                        s.cfg.compress.ratio, -c.compressCutDb));
    }
    const std::string_view reason = rds::ToString(c.reason);
    const std::string_view site = rds::ToString(c.site);
    const std::string_view surf = rds::ToString(c.surface);
    const std::string_view phase = rds::ToString(c.motion, c.moment);
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
    // The wheel walks the list without opening it, from wherever *this* side is
    // rather than from the shared index - with the panel split the two sides
    // are on different configs, and stepping side B off side A's place is how
    // an A/B loses track of what it was comparing.
    if (const int delta = ComboWheel(); delta != 0 && !m_configFiles.empty()) {
        const int n = static_cast<int>(m_configFiles.size());
        int at = m_configIndex;
        for (int i = 0; i < n; ++i)
            if (m_configFiles[static_cast<std::size_t>(i)].stem().string() == s.name) at = i;
        const int next = ((at + delta) % n + n) % n;
        m_configIndex = next;
        m_focusSide = side;
        LoadConfigFile(side, m_configFiles[static_cast<std::size_t>(next)]);
    }
    Tip(std::string("Which config this side is playing. Loading one starts a fresh undo history.\n\n"
                    "Newest first - the list is in the order the files were created, so the one\n"
                    "you saved last is at the top and the one it came from is under it. The\n"
                    "mouse wheel over this box steps through them without opening it.") +
        (s.patchNote.empty() ? std::string{}
                             : "\n\nThe last patch onto this side said: " + s.patchNote));

    // the config it is being read against
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150.0f);
    const std::string comparePreview =
        s.compareName.empty() ? std::string("vs ...") : "vs " + s.compareName;
    if (ImGui::BeginCombo("##compare", comparePreview.c_str())) {
        if (ImGui::Selectable("(none)", s.compareName.empty())) ClearCompare(side);
        for (const fs::path& file : m_configFiles) {
            const std::string item = file.stem().string();
            if (ImGui::Selectable(item.c_str(), item == s.compareName)) {
                LoadCompareFile(side, file);
                m_compareSide = side;
            }
        }
        if (m_configFiles.empty()) ImGui::TextDisabled("(none saved yet)");
        ImGui::EndCombo();
    }
    // Same wheel, same list. Where the comparison has not been set yet it opens
    // on the neighbour of what this side is playing, which is the comparison
    // somebody reaching for the wheel nearly always means.
    if (const int delta = ComboWheel(); delta != 0 && !m_configFiles.empty()) {
        const int n = static_cast<int>(m_configFiles.size());
        const std::string& anchor = s.compareName.empty() ? s.name : s.compareName;
        int at = 0;
        for (int i = 0; i < n; ++i)
            if (m_configFiles[static_cast<std::size_t>(i)].stem().string() == anchor) at = i;
        const int next = ((at + delta) % n + n) % n;
        LoadCompareFile(side, m_configFiles[static_cast<std::size_t>(next)]);
        m_compareSide = side;
    }
    Tip("A second config to read this one against. It is never played and never edited -\n"
        "picking it only marks up the panel below:\n\n"
        "  *   this parameter is written differently in the two files\n"
        "  =   the two agree on it\n\n"
        "\"the same as config_22_08_17 but with the ramp pulled in\" is a sentence about\n"
        "two files, and this is the only place the program can say it. Turn on \"diff\n"
        "only\" under the filter box to see nothing but the parameters that moved.");
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

    ImGui::SetNextItemWidth(200.0f);
    ImGui::InputTextWithHint("##filter", "filter parameters", m_filter, sizeof(m_filter));
    ImGui::SameLine();
    ImGui::TextDisabled("%zu cues, %.1f ms", s.result.cues.size(), s.runMs);

    // what the compare config makes of this one
    if (CompareOn(side)) {
        int differing = 0;
        int same = 0;
        CompareCounts(side, differing, same);
        ImGui::SameLine();
        ImGui::Checkbox("diff only", &m_diffOnly);
        Tip("Hide every parameter the two configs agree on.");
        ImGui::SameLine();
        if (differing == 0)
            ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), "identical to %s",
                               s.compareName.c_str());
        else
            ImGui::TextColored(ImVec4(0.45f, 0.8f, 1.0f, 1.0f), "%d differ, %d same", differing,
                               same);
        Tip(std::format("Against {}. Click for the list.\n\n{} of {} parameters would be written "
                        "differently; the other {} are the same in both files.",
                        s.compareName, differing, differing + same, same));
        if (ImGui::IsItemClicked()) {
            m_compareSide = side;
            m_showCompareReport = true;
        }
    }

    // the design's own checks, on demand
    ImGui::SameLine();
    if (ImGui::Button("Verify") && m_recording) {
        rds::OfflineOptions opt = m_pretend;
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

    std::string needle = m_filter;
    std::transform(needle.begin(), needle.end(), needle.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    std::string_view openGroup;
    bool groupVisible = false;

    /// Whether this group has put a row on screen yet - what a rule needs above
    /// it before it is worth drawing.
    bool groupDrew = false;

    // Two columns, and only where the schema asked for one: the row that opens a
    // pair leaves this set, and the next row that actually gets drawn takes it.
    // "The next row that gets drawn" rather than "the next row" because the
    // filter can hide either half, and a lone widget parked in the right-hand
    // column with nothing to its left reads as a bug.
    bool pairOpen = false;
    float columnWidth = 0.0f;
    constexpr float kColumnGap = 12.0f;

    for (const rds::ParamDesc& p : rds::AlgorithmParams()) {
        // A surface with no block of its own has no panel either: its rows are
        // holding inherited values, and a slider that silently gets overwritten
        // by the next Resolve is worse than no slider. The `+` below is how one
        // gets opened.
        const rds::SurfaceClass rowSurface = rds::SurfaceClassOfParam(p);
        if (rowSurface != rds::SurfaceClass::kCount && !s.cfg.surfaces.Opened(rowSurface)) {
            continue;
        }

        if (!needle.empty()) {
            std::string hay = std::string(p.key) + " " + std::string(p.label) + " " + std::string(p.group);
            std::transform(hay.begin(), hay.end(), hay.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (hay.find(needle) == std::string::npos) continue;
        }

        // Hidden before the group header rather than after it, so a stage the
        // two configs agree on top to bottom disappears entirely instead of
        // leaving an empty heading behind - same as the text filter above.
        if (m_diffOnly && CompareOn(side) && !ParamDiffers(s.cfg, s.compareCfg, p)) continue;

        if (p.group != openGroup) {
            openGroup = p.group;
            pairOpen = false;
            groupDrew = false;
            // Scoped to the run's first desc, not the header text: a group name
            // that comes back after another one has interrupted it - a schema
            // ordering slip - otherwise gives two headers one id, and ImGui
            // says so in a dialog across the panel.
            ImGui::PushID(static_cast<const void*>(&p));
            groupVisible = ImGui::CollapsingHeader(std::string(p.group).c_str(), ImGuiTreeNodeFlags_DefaultOpen);
            // An opened surface can be given back. Closing is not a reset to the
            // defaults - it is a return to inheriting, so a class under a tuned
            // parent goes back to that parent rather than to zero, which is why
            // the button says what it says.
            if (rowSurface != rds::SurfaceClass::kCount) {
                const std::string text =
                    std::format("Inherit from {}", SurfaceParentName(rowSurface));
                const std::string label = text + "###inherit";
                const float width =
                    ImGui::CalcTextSize(text.c_str()).x + ImGui::GetStyle().FramePadding.x * 2.0f;
                // Measured off the window rather than off the cursor: a
                // CollapsingHeader is full width, so there is no room left after
                // it and SameLine() with no argument would push the button off
                // the panel.
                ImGui::SameLine(ImGui::GetWindowWidth() - width -
                                ImGui::GetStyle().WindowPadding.x -
                                ImGui::GetStyle().ScrollbarSize);
                if (ImGui::SmallButton(label.c_str())) {
                    BeginEdit(side, std::format("close Surface: {}", rds::ToString(rowSurface)));
                    s.cfg.surfaces.opened[static_cast<std::size_t>(rowSurface)] = false;
                    s.cfg.surfaces.Resolve();
                    s.dirty = true;
                    s.verifyRun = false;
                    m_focusSide = side;
                }
                Tip("Give this surface back to its parent. Its own numbers are\n"
                    "dropped and it plays whatever it inherits - which is what it\n"
                    "was doing before it was opened, not the shipped defaults.");
            }
            ImGui::PopID();
        }
        if (!groupVisible) continue;

        // A rule where one feature inside the group ends and the next begins.
        // Only once the group has drawn something, because the filter and the
        // diff toggle can both take away everything above it, and a rule under
        // a header separates the header from nothing. `!pairOpen` is a guard
        // against a schema slip rather than a case: there is no line to break
        // halfway along a two-column row.
        if (p.ruleBefore && groupDrew && !pairOpen) {
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
        }

        // Both in the same space ImGui::SameLine takes: measured from the
        // window's own left edge, not from the cursor, so a right-hand column
        // and a widget inside it can be placed with one number each.
        const float left = ImGui::GetCursorPosX();
        const float right = left + ImGui::GetContentRegionAvail().x;
        float startX = 0.0f;  ///< 0 means "start the row here", without a SameLine
        float columnRight = right;
        if (pairOpen) {
            startX = left + columnWidth + kColumnGap;
        } else if (p.pairWithNext) {
            columnWidth = (right - left - kColumnGap) * 0.5f;
            columnRight = left + columnWidth;
        }
        DrawParam(side, p, startX, columnRight);
        pairOpen = !pairOpen && p.pairWithNext;
        groupDrew = true;
    }

    DrawSurfaceAdd(side);

    // The gesture is over when nothing anywhere is being held: a released
    // slider, a closed combo, a clicked checkbox. Asking the global rather than
    // the widget also closes a step whose widget the filter has since hidden.
    if (s.editOpen && !ImGui::IsAnyItemActive()) CommitEdit(side);
}

std::string App::SurfaceParentName(rds::SurfaceClass surface) {
    const rds::SurfaceClass parent = rds::SurfaceParent(surface);
    return parent == rds::SurfaceClass::kCount ? std::string{"[Surfaces]"}
                                               : std::string{rds::ToString(parent)};
}

void App::DrawSurfaceAdd(int side) {
    ConfigSide& s = m_side[side];

    // Drawn unconditionally at the bottom rather than as a row in the schema,
    // because it is the one control here that is not a parameter: it decides
    // whether a set of parameters exists at all. The filter deliberately does
    // not hide it - somebody who typed "ice" and got nothing needs to be able to
    // open ice from the same screen.
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    std::size_t closed = 0;
    for (int i = 0; i < static_cast<int>(rds::SurfaceClass::kCount); ++i) {
        closed += s.cfg.surfaces.opened[static_cast<std::size_t>(i)] ? 0u : 1u;
    }
    if (closed == 0) {
        ImGui::TextDisabled("Every surface has a block of its own.");
        return;
    }

    if (ImGui::Button("+ Surface")) ImGui::OpenPopup("##addsurface");
    Tip("Give one floor its own offset, ramp, trim and mute.\n\n"
        "It opens holding exactly what it was already inheriting, so nothing\n"
        "changes until you move something. Close it again from its header to\n"
        "go back to following its parent.");

    if (ImGui::BeginPopup("##addsurface")) {
        for (int i = 0; i < static_cast<int>(rds::SurfaceClass::kCount); ++i) {
            const auto surface = static_cast<rds::SurfaceClass>(i);
            if (s.cfg.surfaces.Opened(surface)) continue;
            const std::string label = std::format("{}   (now follows {})", rds::ToString(surface),
                                                  SurfaceParentName(surface));
            if (ImGui::Selectable(label.c_str())) {
                BeginEdit(side, std::format("open Surface: {}", rds::ToString(surface)));
                // No values are copied: Resolve has already left this block
                // holding what it inherits, so setting the bit *is* the
                // snapshot. That is the whole reason opening one is free.
                s.cfg.surfaces.opened[static_cast<std::size_t>(surface)] = true;
                s.dirty = true;
                s.verifyRun = false;
                m_focusSide = side;
            }
        }
        ImGui::EndPopup();
    }
}

void App::DrawParam(int side, const rds::ParamDesc& p, float startX, float rightX) {
    ConfigSide& s = m_side[side];
    void* root = &s.cfg;

    // The desc's own address, not the key text: several sections carry a key
    // spelled the same way (fGainDb), and two widgets sharing an ImGui id
    // fight over which one is active.
    ImGui::PushID(static_cast<const void*>(&p));

    double value = rds::GetParam(root, p);
    const bool moved = std::fabs(value - p.defaultValue) > 1e-9;

    std::string tip = std::string(p.tooltip);
    tip += "\n\n" + rds::QualifiedKey(p) + "   default " + rds::FormatParam(p, p.defaultValue);

    // What the compare config, if there is one, has on this row.
    const bool comparing = !s.compareName.empty();
    const bool differs = comparing && ParamDiffers(s.cfg, s.compareCfg, p);
    if (comparing) {
        const std::string theirs =
            p.type == rds::ParamType::kString
                ? std::string(rds::GetParamString(&s.compareCfg, p))
                : rds::FormatParam(p, rds::GetParam(&s.compareCfg, p));
        tip += "\n" + s.compareName + " has " + theirs + (differs ? "" : "   - the same");
    }

    // The right-hand half of a pair goes back up onto the line the left-hand
    // half just finished.
    if (startX > 0.0f) ImGui::SameLine(startX);
    const float left = ImGui::GetCursorPosX();

    // The compare marker, in a column of its own in front of the name. Both
    // states get a glyph, not just the interesting one: "these two agree" is an
    // answer to the same question as "these two differ", and a row with nothing
    // in front of it reads as a row the comparison never looked at.
    if (comparing) {
        if (differs)
            ImGui::TextColored(ImVec4(0.45f, 0.8f, 1.0f, 1.0f), "*");
        else
            ImGui::TextDisabled("=");
        Tip(differs ? "Written differently in " + s.compareName + "."
                    : s.compareName + " has the same value.");
        ImGui::SameLine(0.0f, 4.0f);
    }

    // The name gets a column of its own. Letting the widget carry its label
    // pushes the text off the right edge of the panel, which turns a tuning
    // surface into a hundred anonymous numbers.
    if (moved) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.45f, 1.0f));
    ImGui::TextUnformatted(p.label.data(), p.label.data() + p.label.size());
    if (moved) ImGui::PopStyleColor();
    Tip(tip);

    // Half as wide means half the room for a name, so the widget column starts
    // at whichever is further right: the 190 the full-width rows line up on, or
    // whatever this name actually needed.
    const float nameEnd = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x;
    const float indent = std::min(190.0f, (rightX - left) * 0.5f);
    const float widgetX = std::max(left + indent, nameEnd + 12.0f);
    ImGui::SameLine(widgetX);
    ImGui::SetNextItemWidth(std::max(60.0f, rightX - widgetX - 4.0f));

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
        // pushed until the widget is released, in DrawParams.
        BeginEdit(side, std::string(p.group) + " / " + std::string(p.label));
        m_focusSide = side;

        if (p.step > 0.0 && p.type == rds::ParamType::kFloat)
            value = std::round(value / p.step) * p.step;
        rds::SetParam(root, p, rds::CoerceParam(p, value));
        // Any surface edit can move a class that is following it: pulling
        // stone's trim has to take ice and glass with it, and pulling the
        // [Surfaces] ramp has to take every unopened class. Cheap enough to run
        // on every edit rather than work out which edits are the ones that
        // matter - thirteen classes, three hops at worst.
        s.cfg.surfaces.Resolve();
        s.cfg.slots.rngSeed = m_seed;
        s.dirty = true;
        s.verifyRun = false;
    }

    ImGui::PopID();
}

void App::DrawCompareReport(int side) {
    if (!m_showCompareReport) return;
    ConfigSide& s = m_side[side];
    if (s.compareName.empty()) {  // the combo was set back to (none)
        m_showCompareReport = false;
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(720.0f, 420.0f), ImGuiCond_FirstUseEver);
    const std::string title =
        std::format("{} vs {}###compare", s.name, s.compareName);
    if (!ImGui::Begin(title.c_str(), &m_showCompareReport)) {
        ImGui::End();
        return;
    }

    int differing = 0;
    int same = 0;
    CompareCounts(side, differing, same);
    ImGui::Text("%d of %d parameters differ", differing, differing + same);
    ImGui::SameLine();
    ImGui::TextDisabled("- the other %d are identical in both", same);
    if (s.unsaved) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.45f, 1.0f), "(side %c has unsaved edits)",
                           'A' + side);
        Tip("This is the config as it stands in the panel, not as its file was written.");
    }

    ImGui::Checkbox("show identical too", &m_compareShowSame);
    Tip("List every parameter, with the ones both files agree on dimmed.");
    ImGui::SameLine();
    if (ImGui::Button("Copy")) {
        // One line per differing parameter, in the ini's own spelling, so it
        // pastes into a note or a changelog and still means something a week
        // later.
        std::string text = std::format("{} vs {}\n", s.name, s.compareName);
        for (const rds::ParamDesc* p : CompareDeltas(side))
            text += std::format("{} = {}   ({} = {})\n", rds::QualifiedKey(*p),
                                rds::FormatParam(*p, rds::GetParam(&s.cfg, *p)), s.compareName,
                                rds::FormatParam(*p, rds::GetParam(&s.compareCfg, *p)));
        ImGui::SetClipboardText(text.c_str());
    }
    Tip("The differing parameters as text, on the clipboard.");

    ImGui::Separator();

    if (ImGui::BeginTable("comparerows", 4,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                              ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("parameter", ImGuiTableColumnFlags_WidthStretch, 2.2f);
        ImGui::TableSetupColumn("group", ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableSetupColumn(s.name.c_str(), ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn(s.compareName.c_str(), ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableHeadersRow();

        for (const rds::ParamDesc& p : rds::AlgorithmParams()) {
            const bool rowDiffers = ParamDiffers(s.cfg, s.compareCfg, p);
            if (!rowDiffers && !m_compareShowSame) continue;

            const std::string mine = p.type == rds::ParamType::kString
                                         ? std::string(rds::GetParamString(&s.cfg, p))
                                         : rds::FormatParam(p, rds::GetParam(&s.cfg, p));
            const std::string theirs = p.type == rds::ParamType::kString
                                           ? std::string(rds::GetParamString(&s.compareCfg, p))
                                           : rds::FormatParam(p, rds::GetParam(&s.compareCfg, p));

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (rowDiffers)
                ImGui::TextUnformatted(p.label.data(), p.label.data() + p.label.size());
            else
                ImGui::TextDisabled("%.*s", static_cast<int>(p.label.size()), p.label.data());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s\n\n%.*s", rds::QualifiedKey(p).c_str(),
                                  static_cast<int>(p.tooltip.size()), p.tooltip.data());
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%.*s", static_cast<int>(p.group.size()), p.group.data());
            ImGui::TableNextColumn();
            if (rowDiffers)
                ImGui::TextColored(ImVec4(0.45f, 0.8f, 1.0f, 1.0f), "%s", mine.c_str());
            else
                ImGui::TextDisabled("%s", mine.c_str());
            ImGui::TableNextColumn();
            if (rowDiffers)
                ImGui::TextUnformatted(theirs.c_str());
            else
                ImGui::TextDisabled("%s", theirs.c_str());
        }
        ImGui::EndTable();
    }

    ImGui::End();
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
