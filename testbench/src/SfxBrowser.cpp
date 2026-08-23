#include "SfxBrowser.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <format>

#include <spdlog/spdlog.h>

#include "imgui.h"

#include "Mixer.h"
#include "SfxAnalysis.h"
#include "SfxImport.h"

namespace tb {
namespace {

constexpr ImVec4 kMatch{1.0f, 0.85f, 0.35f, 1.0f};
constexpr ImVec4 kWarn{1.0f, 0.62f, 0.35f, 1.0f};
constexpr ImVec4 kBlocked{1.0f, 0.42f, 0.42f, 1.0f};
constexpr ImVec4 kSuggest{0.55f, 0.82f, 1.0f, 1.0f};
constexpr ImVec4 kQuiet{0.62f, 0.64f, 0.70f, 1.0f};
constexpr ImVec4 kLoop{0.50f, 0.90f, 0.65f, 1.0f};

[[nodiscard]] std::string Lower(std::string_view text) {
    std::string out(text);
    std::ranges::transform(out, out.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

void Tip(std::string_view text) {
    if (!text.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(460.0f);
        ImGui::TextUnformatted(text.data(), text.data() + text.size());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

/// A small pill with a border, so a badge reads as a badge rather than as more
/// text on the row.
void Pill(std::string_view text, const ImVec4& colour) {
    const ImVec2 size = ImGui::CalcTextSize(text.data(), text.data() + text.size());
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float pad = 4.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, ImVec2(pos.x + size.x + pad * 2.0f, pos.y + size.y + 2.0f),
                      ImGui::GetColorU32(ImVec4(colour.x, colour.y, colour.z, 0.16f)), 3.0f);
    dl->AddRect(pos, ImVec2(pos.x + size.x + pad * 2.0f, pos.y + size.y + 2.0f),
                ImGui::GetColorU32(ImVec4(colour.x, colour.y, colour.z, 0.55f)), 3.0f);
    ImGui::Dummy(ImVec2(size.x + pad * 2.0f, size.y + 2.0f));
    dl->AddText(ImVec2(pos.x + pad, pos.y + 1.0f), ImGui::GetColorU32(colour), text.data(),
                text.data() + text.size());
}

}  // namespace

// ═════════════════════════════════════════════════════════════════════════════
// shared widgets
// ═════════════════════════════════════════════════════════════════════════════

namespace sfxui {

std::string Duration(float ms) {
    return ms >= 1000.0f ? std::format("{:.2f} s", ms / 1000.0f) : std::format("{:.0f} ms", ms);
}

void HighlightedText(std::string_view text, std::string_view needle) {
    if (needle.empty()) {
        ImGui::TextUnformatted(text.data(), text.data() + text.size());
        return;
    }
    const std::string haystack = Lower(text);
    const std::string want = Lower(needle);

    // Laid out with SameLine and no spacing, so the pieces read as one word.
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, ImGui::GetStyle().ItemSpacing.y));
    std::size_t cursor = 0;
    bool first = true;
    while (cursor < text.size()) {
        const auto hit = haystack.find(want, cursor);
        if (hit == std::string::npos) {
            if (!first) ImGui::SameLine();
            ImGui::TextUnformatted(text.data() + cursor, text.data() + text.size());
            break;
        }
        if (hit > cursor) {
            if (!first) ImGui::SameLine();
            ImGui::TextUnformatted(text.data() + cursor, text.data() + hit);
            first = false;
        }
        if (!first) ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, kMatch);
        ImGui::TextUnformatted(text.data() + hit, text.data() + hit + want.size());
        ImGui::PopStyleColor();
        first = false;
        cursor = hit + want.size();
    }
    ImGui::PopStyleVar();
}

void Badges(const rds::SfxEntry& entry, bool showSuggestions) {
    Pill(Duration(entry.durationMs), kQuiet);
    Tip(std::format("{:.0f} ms of file{}", entry.durationMs,
                    entry.loops ? "" : std::format(", {:.0f} ms of event after {:.0f} ms of lead-in",
                                                   entry.usableMs, entry.leadInMs)));

    if (entry.loops) {
        ImGui::SameLine();
        Pill("loop", kLoop);
        Tip(std::format("Measures as a sustained texture rather than an event: {:.1f} dB envelope "
                        "spread, {:.1f} dB seam, {:.0f} grains/s. A looping slot repeats this whole "
                        "and it is never judged for being long.",
                        entry.steadyDb, entry.seamDb, entry.grainsPerSec));
    }

    for (const rds::SfxWarning& w : entry.warnings) {
        ImGui::SameLine();
        Pill(w.code, w.blocking ? kBlocked : kWarn);
        Tip(w.blocking ? "Cannot be played at all.\n\n" + w.detail : w.detail);
    }

    if (showSuggestions) {
        for (const rds::SlotId slot : entry.suggested) {
            ImGui::SameLine();
            Pill(rds::Slot(slot).name, kSuggest);
            Tip(std::format("Suits {}: {}\n\n{:.0f} to {:.0f} ms.\n\nA suggestion from what this "
                            "measures against that slot's targets, never a restriction - anything "
                            "can be assigned to anything.",
                            rds::Slot(slot).name, rds::Slot(slot).character,
                            rds::Slot(slot).minLengthMs, rds::Slot(slot).maxLengthMs));
        }
    }
}

std::shared_ptr<PreviewClip> LoadPreview(const rds::SfxLibrary& library, const std::string& file,
                                         int sampleRate) {
    auto clip = std::make_shared<PreviewClip>();
    clip->file = file;
    int rate = 0;
    // At the device's rate, so the preview voice has no resampler in it - the
    // audio callback is not where a rate conversion belongs.
    std::vector<float> mono;
    if (!DecodeMonoFile(library.PathOf(file).string(), mono, rate) || mono.empty()) {
        return nullptr;
    }
    if (rate != sampleRate && rate > 0) {
        // Linear, for the same reason Pcm.h resamples linearly: the ratio is at
        // worst 1.088 across the formats the library holds, and the artefact of
        // that sits far below the noise floor of anything in here.
        const double ratio = static_cast<double>(rate) / static_cast<double>(sampleRate);
        const auto frames = static_cast<std::size_t>(static_cast<double>(mono.size()) / ratio);
        clip->mono.resize(frames);
        for (std::size_t i = 0; i < frames; ++i) {
            const double at = static_cast<double>(i) * ratio;
            const auto i0 = static_cast<std::size_t>(at);
            const auto frac = static_cast<float>(at - static_cast<double>(i0));
            const float a = mono[std::min(i0, mono.size() - 1)];
            const float b = mono[std::min(i0 + 1, mono.size() - 1)];
            clip->mono[i] = a * (1.0f - frac) + b * frac;
        }
    } else {
        clip->mono = std::move(mono);
    }
    return clip;
}

void PreviewButton(const char* id, const rds::SfxLibrary& library, const std::string& file,
                   Player& player, int sampleRate) {
    ImGui::PushID(id);
    const bool playing = player.PreviewPlaying() && player.PreviewFile() == file;
    if (ImGui::Button(playing ? "Stop" : "Play", ImVec2(46.0f, 0.0f))) {
        if (playing) {
            player.StopPreview();
        } else if (auto clip = LoadPreview(library, file, sampleRate); clip) {
            player.SetPreview(std::move(clip), false);
        }
    }
    Tip("Auditions this file on its own, over whatever the take is doing. The take's transport is "
        "untouched - that is the point, you want to hear one against the other.");
    if (playing) {
        ImGui::SameLine();
        ImGui::ProgressBar(player.PreviewProgress(), ImVec2(56.0f, ImGui::GetTextLineHeight()), "");
    }
    ImGui::PopID();
}

}  // namespace sfxui

// ═════════════════════════════════════════════════════════════════════════════
// the window
// ═════════════════════════════════════════════════════════════════════════════

void SfxBrowser::Init(rds::SfxLibrary* library, Player* player, int sampleRate) {
    m_library = library;
    m_player = player;
    m_sampleRate = sampleRate;
    m_stale = true;
}

void SfxBrowser::SetPackDirectory(std::filesystem::path directory) {
    m_packDirectory = std::move(directory);
}

bool SfxBrowser::TakeLibraryChanged() {
    const bool changed = m_libraryChanged;
    m_libraryChanged = false;
    return changed;
}

std::vector<std::filesystem::path> SfxBrowser::PackCandidates() const {
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    if (m_library == nullptr || m_packDirectory.empty() ||
        !std::filesystem::is_directory(m_packDirectory, ec)) {
        return out;
    }
    for (const std::filesystem::directory_entry& item :
         std::filesystem::directory_iterator(m_packDirectory, ec)) {
        if (!item.is_regular_file()) {
            continue;
        }
        if (Lower(item.path().extension().string()) != ".wav") {
            continue;
        }
        // By filename, because that is the library's identity. A pack file
        // already adopted is one already there under the same name.
        if (m_library->Find(item.path().filename().string()) == nullptr) {
            out.push_back(item.path());
        }
    }
    std::ranges::sort(out);
    return out;
}

void SfxBrowser::AdoptPack() {
    const std::vector<std::filesystem::path> files = PackCandidates();
    if (files.empty()) {
        m_importNote = "the library already holds the whole pack";
        return;
    }
    // The FMTS fix is off for these whatever the checkbox says: a pack file is
    // named `<slot>_<NN>` on purpose, that name is what seeds the assignments,
    // and tidying it would break exactly the thing this button exists for.
    ImportOptions options;
    options.fixNames = false;

    int ok = 0;
    for (const std::filesystem::path& file : files) {
        if (!ImportSfx(file, *m_library, options).file.empty()) {
            ++ok;
        }
    }
    m_importNote = std::format("adopted {} of {} pack file(s)", ok, files.size());
    m_stale = true;
    m_libraryChanged = true;
    spdlog::info("sfx: {}", m_importNote);
}

void SfxBrowser::Open() {
    m_open = true;
    m_picking = false;
    m_current.clear();
    m_stale = true;
    m_focusSearch = false;
    ImGui::SetWindowFocus("SFX library");
}

void SfxBrowser::OpenForSlot(rds::SlotId slot, int variant, std::string current, bool slotLoops) {
    m_open = true;
    m_picking = true;
    m_slot = slot;
    m_variant = variant;
    m_current = std::move(current);
    m_slotLoops = slotLoops;
    m_stale = true;
    // The search box takes the keyboard: the way you find a sound is to type
    // part of its name, and having to click the box first is the one thing that
    // would make this slower than scrolling a dropdown.
    m_focusSearch = true;
    m_highlight = -1;
    ImGui::SetWindowFocus("SFX library");
}

void SfxBrowser::Close() {
    CommitNote();
    m_open = false;
    m_picking = false;
    if (m_player != nullptr) {
        m_player->StopPreview();
    }
}

bool SfxBrowser::WantsKeys() const { return m_open && m_hasFocus; }

void SfxBrowser::MarkDirty(const std::string& file) {
    if (std::ranges::find(m_dirtyFiles, file) == m_dirtyFiles.end()) {
        m_dirtyFiles.push_back(file);
    }
}

void SfxBrowser::Save() {
    if (m_library == nullptr) {
        return;
    }
    CommitNote();
    for (const std::string& file : m_dirtyFiles) {
        if (const rds::SfxEntry* entry = m_library->Find(file); entry != nullptr) {
            m_library->SaveMeta(*entry);
        }
    }
    if (!m_dirtyFiles.empty()) {
        spdlog::info("sfx: wrote {} metadata file(s)", m_dirtyFiles.size());
    }
    m_dirtyFiles.clear();
}

void SfxBrowser::CommitNote() {
    if (m_library == nullptr) {
        return;
    }
    if (m_noteRow >= 0 && m_noteRow < static_cast<int>(m_rows.size())) {
        rds::SfxEntry& entry =
            m_library->MutableEntries()[m_rows[static_cast<std::size_t>(m_noteRow)].entry];
        if (entry.note != m_noteBuffer) {
            entry.note = m_noteBuffer;
            MarkDirty(entry.file);
        }
    }
    m_noteRow = -1;

    if (m_renameRow >= 0 && m_renameRow < static_cast<int>(m_rows.size())) {
        rds::SfxEntry& entry =
            m_library->MutableEntries()[m_rows[static_cast<std::size_t>(m_renameRow)].entry];
        if (m_renameBuffer[0] != '\0' && entry.name != m_renameBuffer) {
            entry.name = m_renameBuffer;
            MarkDirty(entry.file);
        }
    }
    m_renameRow = -1;
}

void SfxBrowser::Rebuild() {
    m_rows.clear();
    if (m_library == nullptr) {
        return;
    }
    const std::string needle = Lower(m_search);

    const auto entries = m_library->Entries();
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const rds::SfxEntry& entry = entries[i];
        if (!needle.empty()) {
            // Name, note and every badge - which is what "searches name, badges
            // and note" means, and the badges are the warning codes and the
            // suggested slot names.
            std::string hay = Lower(entry.name) + " " + Lower(entry.file) + " " + Lower(entry.note);
            for (const rds::SfxWarning& w : entry.warnings) {
                hay += " " + Lower(w.code);
            }
            for (const rds::SlotId slot : entry.suggested) {
                hay += " " + Lower(rds::Slot(slot).name);
            }
            if (entry.loops) {
                hay += " loop";
            }
            if (hay.find(needle) == std::string::npos) {
                continue;
            }
        }
        Row row;
        row.entry = i;
        if (m_picking) {
            row.fit = SlotFit(entry, m_slot);
            row.lengthSuits = LengthSuits(entry.durationMs, m_slot, m_slotLoops);
        }
        m_rows.push_back(row);
    }

    if (m_picking) {
        // Fitting lengths to the top, and inside that band by name - which is
        // the order asked for, and the one that makes the list navigable: the
        // right length is the hard filter, everything after it is taste.
        std::ranges::stable_sort(m_rows, [&](const Row& a, const Row& b) {
            if (a.lengthSuits != b.lengthSuits) {
                return a.lengthSuits;
            }
            return Lower(entries[a.entry].name) < Lower(entries[b.entry].name);
        });
    }

    m_highlight = std::min(m_highlight, static_cast<int>(m_rows.size()) - 1);
    m_stale = false;
}

SfxBrowser::Pick SfxBrowser::Draw() {
    Pick pick;
    if (!m_open || m_library == nullptr || m_player == nullptr) {
        m_hasFocus = false;
        return pick;
    }
    if (m_stale) {
        Rebuild();
    }

    ImGui::SetNextWindowSize(ImVec2(760.0f, 620.0f), ImGuiCond_FirstUseEver);
    bool open = true;
    if (!ImGui::Begin("SFX library", &open)) {
        m_hasFocus = false;
        ImGui::End();
        if (!open) {
            Close();
        }
        return pick;
    }
    m_hasFocus = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

    DrawHeader();
    ImGui::Separator();

    if (m_picking) {
        DrawPickPreviews();
        ImGui::Separator();
    }

    const char* hint = m_picking ? "search name, note, warnings and suggested slots"
                                 : "search name, note and badges";
    if (m_focusSearch) {
        ImGui::SetKeyboardFocusHere();
        m_focusSearch = false;
    }
    ImGui::SetNextItemWidth(-160.0f);
    if (ImGui::InputTextWithHint("##search", hint, m_search, sizeof(m_search))) {
        m_stale = true;
        // A new search is a new list, and keeping the old index would leave the
        // highlight on whatever happened to land in that position.
        m_highlight = m_rows.empty() ? -1 : 0;
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        m_search[0] = '\0';
        m_stale = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%d / %d", static_cast<int>(m_rows.size()),
                        static_cast<int>(m_library->Size()));

    HandleKeys(pick);

    ImGui::Separator();
    DrawList(pick);

    ImGui::End();
    if (!open) {
        Close();
    }
    if (pick.made) {
        Close();
    }
    return pick;
}

void SfxBrowser::DrawHeader() {
    if (ImGui::Button("Import...")) {
        RunImport();
    }
    Tip("Pick one or more files. Each is converted to the pack's mono / 48 kHz / 16-bit, copied "
        "into the library, measured, and given a metadata file. Nothing is ever rejected - what is "
        "wrong with a file comes back as a badge you can read and ignore.");

    ImGui::SameLine();
    if (ImGui::Checkbox("FMTS fix", &m_fixNames)) {
    }
    Tip("Strip the site stamp out of the filename on the way in:\n"
        "  punch-face-hard-3(fromnoisetosound.com)  ->  punch-face-hard-3\n"
        "Bracketed groups go, and the separators they leave behind are collapsed. Off if a name "
        "genuinely has brackets in it.");

    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();

    const bool haveTools = FfmpegAvailable();
    if (haveTools) {
        ImGui::TextDisabled("ffmpeg ok");
        Tip("Converting and probing go through " + FfmpegPath());
    } else {
        ImGui::TextColored(kWarn, "no ffmpeg");
        Tip("ffmpeg and ffprobe are not on PATH. wav files still import - miniaudio decodes those "
            "on its own - but nothing else will, and nothing will be converted to the pack's "
            "48 kHz mono.");
    }

    // Only while there is something to adopt, which after the first click is
    // never - a button that says "nothing to do" every time is a button that
    // teaches people not to read the header bar.
    if (const std::size_t pending = PackCandidates().size(); pending != 0) {
        ImGui::SameLine();
        if (ImGui::Button(std::format("Adopt pack ({})##adopt", pending).c_str())) {
            AdoptPack();
        }
        Tip(std::format("Import the {} built pack file(s) the library does not have yet, from\n{}\n\n"
                        "These are the sounds the mod plays today. Adopting them fills the library "
                        "and, while nothing is assigned yet, seeds every slot from their "
                        "`<slot>_<NN>` names - so the panel starts from what you already have "
                        "rather than from an empty table.\n\n"
                        "Names are kept exactly as they are, whatever the FMTS box says: the name "
                        "is what seeds the assignment.",
                        pending, m_packDirectory.string()));
    }

    ImGui::SameLine();
    if (ImGui::Button("Re-measure all")) {
        int done = 0;
        // A copy: MeasureExisting writes through the library and would be
        // iterating the vector it re-sorts.
        std::vector<std::string> files;
        for (const rds::SfxEntry& entry : m_library->Entries()) {
            files.push_back(entry.file);
        }
        for (const std::string& file : files) {
            std::string error;
            if (MeasureExisting(*m_library, file, error)) {
                ++done;
            } else {
                spdlog::warn("sfx: {}: {}", file, error);
            }
        }
        m_importNote = std::format("re-measured {} of {}", done, files.size());
        m_stale = true;
    }
    Tip("Decode and measure every file again. For entries that arrived without a metadata file - "
        "dropped into the folder by hand, or written by sfx.py - and after the slot targets change. "
        "Names and notes are kept.");

    if (!m_importNote.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", m_importNote.c_str());
    }

    if (Dirty()) {
        ImGui::SameLine();
        ImGui::TextColored(kMatch, "* %zu unsaved", DirtyCount());
        Tip("Names and notes you have changed but not written. Ctrl+S saves everything unsaved, "
            "here and in the main window.");
    }
}

void SfxBrowser::DrawPickPreviews() {
    const rds::SlotDesc& desc = rds::Slot(m_slot);
    ImGui::TextColored(kSuggest, "%s", std::string(desc.name).c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("- %s  |  %.0f to %.0f ms%s", std::string(desc.character).c_str(),
                        desc.minLengthMs, desc.maxLengthMs, m_slotLoops ? "  |  looping" : "");

    // Two widgets, one above the other, so the comparison is a pair of buttons a
    // few pixels apart rather than a scroll.
    ImGui::BeginChild("compare", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 4.4f),
                      ImGuiChildFlags_Borders);

    ImGui::TextDisabled("in the slot now");
    if (m_current.empty()) {
        ImGui::TextDisabled("  (adding a new one - nothing to compare against)");
    } else {
        sfxui::PreviewButton("cur", *m_library, m_current, *m_player, m_sampleRate);
        ImGui::SameLine();
        if (const rds::SfxEntry* entry = m_library->Find(m_current); entry != nullptr) {
            ImGui::TextUnformatted(entry->name.c_str());
            ImGui::SameLine();
            sfxui::Badges(*entry, false);
        } else {
            ImGui::TextColored(kBlocked, "%s - not in the library", m_current.c_str());
        }
    }

    ImGui::Spacing();
    ImGui::TextDisabled("highlighted");
    if (m_highlight >= 0 && m_highlight < static_cast<int>(m_rows.size())) {
        const rds::SfxEntry& entry =
            m_library->Entries()[m_rows[static_cast<std::size_t>(m_highlight)].entry];
        sfxui::PreviewButton("hl", *m_library, entry.file, *m_player, m_sampleRate);
        ImGui::SameLine();
        ImGui::TextUnformatted(entry.name.c_str());
        ImGui::SameLine();
        sfxui::Badges(entry, false);
    } else {
        ImGui::TextDisabled("  (arrow keys move the highlight, space plays it)");
    }

    ImGui::EndChild();
}

void SfxBrowser::HandleKeys(Pick& pick) {
    if (!m_hasFocus) {
        return;
    }
    const bool typing = ImGui::GetIO().WantTextInput;

    // The arrows work while the search box has the keyboard - that is the whole
    // gesture: type three letters, arrow down, space. Space and Enter cannot,
    // because in a text box they are a space and a newline.
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true) && !m_rows.empty()) {
        m_highlight = std::min(m_highlight + 1, static_cast<int>(m_rows.size()) - 1);
        m_scrollToHighlight = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true) && !m_rows.empty()) {
        m_highlight = std::max(m_highlight - 1, 0);
        m_scrollToHighlight = true;
    }
    if (typing) {
        return;
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Space, false) && m_highlight >= 0 &&
        m_highlight < static_cast<int>(m_rows.size())) {
        const rds::SfxEntry& entry =
            m_library->Entries()[m_rows[static_cast<std::size_t>(m_highlight)].entry];
        if (m_player->PreviewPlaying() && m_player->PreviewFile() == entry.file) {
            m_player->StopPreview();
        } else if (auto clip = sfxui::LoadPreview(*m_library, entry.file, m_sampleRate); clip) {
            m_player->SetPreview(std::move(clip), false);
        }
    }
    if (m_picking && ImGui::IsKeyPressed(ImGuiKey_Enter, false) && m_highlight >= 0 &&
        m_highlight < static_cast<int>(m_rows.size())) {
        pick.made = true;
        pick.slot = m_slot;
        pick.variant = m_variant;
        pick.file = m_library->Entries()[m_rows[static_cast<std::size_t>(m_highlight)].entry].file;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        Close();
    }
}

void SfxBrowser::DrawList(Pick& pick) {
    ImGui::BeginChild("list", ImVec2(0, 0), ImGuiChildFlags_Borders);
    if (m_rows.empty()) {
        ImGui::TextDisabled(m_library->Size() == 0
                                ? "The library is empty. Import something."
                                : "Nothing matches. Clear the search.");
    }
    for (int i = 0; i < static_cast<int>(m_rows.size()); ++i) {
        DrawRow(m_rows[static_cast<std::size_t>(i)], i, pick);
    }
    ImGui::EndChild();
}

void SfxBrowser::DrawRow(const Row& row, int index, Pick& pick) {
    const rds::SfxEntry& entry = m_library->Entries()[row.entry];
    ImGui::PushID(static_cast<int>(row.entry));

    const bool highlighted = index == m_highlight;
    const bool isCurrent = m_picking && entry.file == m_current;

    // The band break: once, where the fitting lengths stop. Cheaper to read than
    // a badge on every row saying which side of the line it is on.
    if (m_picking && index > 0 && m_rows[static_cast<std::size_t>(index - 1)].lengthSuits &&
        !row.lengthSuits) {
        ImGui::Separator();
        ImGui::TextDisabled("  the wrong length for %s - still assignable",
                            std::string(rds::Slot(m_slot).name).c_str());
    }

    ImGui::BeginGroup();

    sfxui::PreviewButton("play", *m_library, entry.file, *m_player, m_sampleRate);
    ImGui::SameLine();

    // The name, which doubles as the row's hit box. Double-click renames.
    if (m_renameRow == index) {
        ImGui::SetNextItemWidth(220.0f);
        if (m_renameRow != m_noteRow) {
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::InputText("##rename", m_renameBuffer, sizeof(m_renameBuffer),
                         ImGuiInputTextFlags_AutoSelectAll);
        if (ImGui::IsItemDeactivated()) {
            rds::SfxEntry& mutableEntry = m_library->MutableEntries()[row.entry];
            if (m_renameBuffer[0] != '\0' && mutableEntry.name != m_renameBuffer) {
                mutableEntry.name = m_renameBuffer;
                MarkDirty(mutableEntry.file);
            }
            m_renameRow = -1;
        }
    } else {
        if (highlighted) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
        } else if (isCurrent) {
            ImGui::PushStyleColor(ImGuiCol_Text, kSuggest);
        }
        sfxui::HighlightedText(entry.name, m_search);
        if (highlighted || isCurrent) {
            ImGui::PopStyleColor();
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            CommitNote();
            m_renameRow = index;
            std::snprintf(m_renameBuffer, sizeof(m_renameBuffer), "%s", entry.name.c_str());
        }
        Tip(std::format("{}\n\ndouble-click to rename. The filename never changes, so renaming "
                        "cannot break an assignment.\n\n"
                        "{:.0f} ms, {} Hz, {} ch, {}-bit. peak {:.1f} dBFS, tilt {:+.1f} dB, "
                        "centroid {:.0f} Hz, {} lo-mid transients.",
                        entry.file, entry.durationMs, entry.sampleRate, entry.channels,
                        entry.bitsPerSample, entry.peakDb, entry.tiltDb, entry.centroidHz,
                        entry.loMidTransients));
    }

    ImGui::SameLine();
    sfxui::Badges(entry, true);

    if (m_picking) {
        ImGui::SameLine();
        if (ImGui::SmallButton(isCurrent ? "keep" : "use")) {
            pick.made = true;
            pick.slot = m_slot;
            pick.variant = m_variant;
            pick.file = entry.file;
        }
    }

    // The note, on its own line. A text box only for the row being typed into:
    // one live widget instead of one per file.
    ImGui::Indent(52.0f);
    if (m_noteRow == index) {
        ImGui::SetNextItemWidth(-4.0f);
        ImGui::InputTextWithHint("##note", "your note - searched as substring", m_noteBuffer,
                                 sizeof(m_noteBuffer));
        if (ImGui::IsItemDeactivated()) {
            rds::SfxEntry& mutableEntry = m_library->MutableEntries()[row.entry];
            if (mutableEntry.note != m_noteBuffer) {
                mutableEntry.note = m_noteBuffer;
                MarkDirty(mutableEntry.file);
            }
            m_noteRow = -1;
        }
    } else if (entry.note.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.42f, 0.44f, 0.50f, 1.0f));
        ImGui::TextUnformatted("+ note");
        ImGui::PopStyleColor();
        if (ImGui::IsItemClicked()) {
            CommitNote();
            m_noteRow = index;
            m_noteBuffer[0] = '\0';
        }
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, kQuiet);
        sfxui::HighlightedText(entry.note, m_search);
        ImGui::PopStyleColor();
        if (ImGui::IsItemClicked()) {
            CommitNote();
            m_noteRow = index;
            std::snprintf(m_noteBuffer, sizeof(m_noteBuffer), "%s", entry.note.c_str());
        }
    }
    ImGui::Unindent(52.0f);

    ImGui::EndGroup();

    // Clicking anywhere on the row moves the highlight, so the mouse and the
    // arrow keys drive the same selection.
    if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        m_highlight = index;
    }
    if (highlighted) {
        const ImVec2 a = ImGui::GetItemRectMin();
        const ImVec2 b = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddRect(ImVec2(a.x - 3.0f, a.y - 2.0f),
                                            ImVec2(b.x + 3.0f, b.y + 2.0f),
                                            ImGui::GetColorU32(kSuggest), 3.0f);
        if (m_scrollToHighlight) {
            ImGui::SetScrollHereY(0.5f);
            m_scrollToHighlight = false;
        }
    }
    ImGui::Separator();
    ImGui::PopID();
}

void SfxBrowser::RunImport() {
    m_importNote.clear();
    const std::vector<std::filesystem::path> files = PickAudioFiles();
    if (files.empty()) {
        return;
    }

    ImportOptions options;
    options.fixNames = m_fixNames;

    int ok = 0;
    int failed = 0;
    std::string firstError;
    for (const std::filesystem::path& file : files) {
        const ImportOutcome outcome = ImportSfx(file, *m_library, options);
        if (outcome.file.empty()) {
            ++failed;
            if (firstError.empty()) {
                firstError = file.filename().string() + ": " + outcome.error;
            }
        } else {
            ++ok;
        }
    }
    m_importNote = failed == 0 ? std::format("imported {}", ok)
                               : std::format("imported {}, {} failed - {}", ok, failed, firstError);
    m_stale = true;
    m_libraryChanged = ok > 0;
    spdlog::info("sfx: import run: {}", m_importNote);
}

}  // namespace tb
