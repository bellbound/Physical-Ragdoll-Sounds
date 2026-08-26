#include "SfxBrowser.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <ctime>
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

/// The confirmation's title, in one place because it is written twice - once to
/// open it and once to draw it - and a typo in either is a dialog that never
/// appears.
constexpr const char* kDeletePopup = "Delete this sfx?";

/// `just now`, `4 hours ago`, `9 days ago`. The date says which day it was; this
/// says how long that is without anybody counting back from it.
[[nodiscard]] std::string Ago(std::int64_t unixSeconds) {
    const auto now = static_cast<std::int64_t>(std::time(nullptr));
    const std::int64_t seconds = now - unixSeconds;
    // A clock that moved, or a file dated in the future. Nothing useful to say
    // about it and no reason to say something wrong.
    if (seconds < 0) {
        return {};
    }
    if (seconds < 90) {
        return "just now";
    }
    const std::int64_t minutes = seconds / 60;
    if (minutes < 90) {
        return std::format("{} minutes ago", minutes);
    }
    const std::int64_t hours = minutes / 60;
    if (hours < 36) {
        return std::format("{} hours ago", hours);
    }
    const std::int64_t days = hours / 24;
    if (days < 14) {
        return std::format("{} days ago", days);
    }
    if (days < 60) {
        return std::format("{} weeks ago", days / 7);
    }
    return std::format("{} months ago", days / 30);
}

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
    if (entry.disabled) {
        Pill("muted", kBlocked);
        Tip("Disabled in the library. It stays where it is - the slots that name it still name it - "
            "and the bank skips it, so nothing plays it until it is enabled again.");
        ImGui::SameLine();
    }
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
        Pill(w.code, (w.blocking || w.dead) ? kBlocked : kWarn);
        // Three openings, because the three mean three different things to do:
        // nothing, another file, or the thing the detail goes on to name. The
        // colour only says "red or orange" and a badge that says `clipped` on
        // one file and `clipped` on another has to be able to say which.
        if (w.blocking) {
            Tip("Cannot be played at all.\n\n" + w.detail);
        } else if (w.dead) {
            Tip("Not usable, and no amount of processing changes that.\n\n" + w.detail);
        } else {
            Tip(w.detail);
        }
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

void SfxBrowser::Init(rds::SfxLibrary* library, const rds::SfxAssignments* assignments,
                      Player* player, int sampleRate, float* previewGainDb) {
    m_library = library;
    m_assignments = assignments;
    m_player = player;
    m_sampleRate = sampleRate;
    m_previewGainDb = previewGainDb;
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

std::vector<std::string> SfxBrowser::TakeDeleted() {
    std::vector<std::string> out;
    out.swap(m_deleted);
    return out;
}

std::vector<std::string> SfxBrowser::SlotsUsing(const std::string& file) const {
    std::vector<std::string> out;
    if (m_assignments == nullptr) {
        return out;
    }
    for (const rds::SlotDesc& desc : rds::Slots()) {
        const rds::SlotAssignment& assignment = m_assignments->For(desc.id);
        const bool named = std::ranges::any_of(
            assignment.files, [&](const std::string& name) { return Lower(name) == Lower(file); });
        if (named) {
            out.emplace_back(desc.name);
        }
    }
    return out;
}

void SfxBrowser::DeleteEntry(const std::string& file) {
    if (m_library == nullptr || m_library->Find(file) == nullptr) {
        return;
    }
    const std::filesystem::path audio = m_library->PathOf(file);

    // The sidecar goes with it. Leaving it behind would put the name, the import
    // date and the mute back on the next file imported under this name, which is
    // a sound wearing another sound's history.
    std::string error;
    if (!RecycleFiles({audio, rds::SfxLibrary::MetaPathFor(audio)}, error)) {
        // Nothing is dropped from the library: the file is still there, and an
        // index that has forgotten a file on disk is one Load away from coming
        // back with the name unexplained.
        m_importNote = std::format("could not delete {} - {}", file, error);
        spdlog::warn("sfx: delete {}: {}", file, error);
        return;
    }

    if (m_player != nullptr && m_player->PreviewFile() == file) {
        m_player->StopPreview();
    }
    m_library->Remove(file);
    // Unsaved edits to something that no longer exists. Left in, Save would
    // look it up, not find it, and quietly do nothing - correct, and it would
    // also keep saying "1 unsaved" at a file nobody can open.
    std::erase(m_dirtyFiles, file);
    if (m_current == file) {
        m_current.clear();
    }
    m_deleted.push_back(file);
    m_importNote = std::format("deleted {} - it is in the recycle bin", file);
    // The list is short by one and the bank still holds its samples, so both
    // have to be rebuilt: the row and the sound go at the same time.
    m_stale = true;
    m_libraryChanged = true;
    spdlog::info("sfx: deleted {}", file);
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
    CommitEdits();
    m_open = false;
    m_picking = false;
    if (m_player != nullptr) {
        m_player->StopPreview();
    }
}

bool SfxBrowser::WantsKeys() const { return m_open && m_hasFocus; }

SfxBrowser::Audition SfxBrowser::InTakeAudition() const {
    Audition out;
    if (!m_open || !m_picking || !m_auditionInTake || m_library == nullptr) {
        return out;
    }
    if (m_highlight < 0 || m_highlight >= static_cast<int>(m_rows.size())) {
        return out;
    }
    const rds::SfxEntry& entry =
        m_library->Entries()[m_rows[static_cast<std::size_t>(m_highlight)].entry];
    out.active = true;
    out.slot = m_slot;
    out.file = entry.file;
    return out;
}

void SfxBrowser::MarkDirty(const std::string& file) {
    if (std::ranges::find(m_dirtyFiles, file) == m_dirtyFiles.end()) {
        m_dirtyFiles.push_back(file);
    }
}

void SfxBrowser::ToggleDisabled(std::size_t entry) {
    if (m_library == nullptr || entry >= m_library->Size()) {
        return;
    }
    rds::SfxEntry& mutableEntry = m_library->MutableEntries()[entry];
    mutableEntry.disabled = !mutableEntry.disabled;
    MarkDirty(mutableEntry.file);
    // The row moves to the bottom of the list, and the bank has to be rebuilt:
    // muting is a statement about what the take plays, so it has to be audible
    // on the next block rather than after a reload.
    m_stale = true;
    m_libraryChanged = true;
    if (m_player != nullptr && m_player->PreviewPlaying() &&
        m_player->PreviewFile() == mutableEntry.file && mutableEntry.disabled) {
        m_player->StopPreview();
    }
}

void SfxBrowser::DisableButton(std::size_t entry) {
    const rds::SfxEntry& e = m_library->Entries()[entry];
    ImGui::PushID(static_cast<int>(entry));
    if (ImGui::SmallButton(e.disabled ? "enable" : "disable")) {
        ToggleDisabled(entry);
    }
    Tip(e.disabled
            ? "Put it back in play. It returns to whatever slots still name it, in the position "
              "it had - which is why muting is the thing to reach for when a sound is wrong but "
              "you are not certain it is wrong."
            : "Mute it. It keeps its place on every slot that names it and stops being played by "
              "any of them, and it drops to the bottom of this list so it stops turning up in "
              "the search. This writes to its metadata file, so Ctrl+S.");
    ImGui::PopID();
}

void SfxBrowser::CorrectionRow(std::size_t entry) {
    if (m_library == nullptr || entry >= m_library->Size()) {
        return;
    }
    rds::SfxEntry& e = m_library->MutableEntries()[entry];
    ImGui::PushID(static_cast<int>(entry));
    ImGui::PushID("correct");

    // Semitones rather than a ratio, because "it is a bit low" is a musical
    // complaint and 1.0595 is not an answer to one. The ratio is what is stored
    // and what the engine multiplies by; this is only how it is asked for.
    float semis = 12.0f * std::log2(std::max(0.01f, e.pitch));
    bool changed = false;

    ImGui::TextUnformatted("pitch");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    if (ImGui::DragFloat("##semis", &semis, 0.05f, -12.0f, 12.0f, "%+.2f st")) {
        e.pitch = std::clamp(std::pow(2.0f, semis / 12.0f), 0.5f, 2.0f);
        changed = true;
    }
    Tip("Correct this recording's pitch. It applies wherever the sound is used, because a file "
        "that is flat is flat on every slot that names it - the same rule the mute beside it "
        "follows.\n\n"
        "It multiplies the pitch the engine already chose rather than replacing it, so the "
        "per-cue scatter and the intensity bias still do their work on top.\n\n"
        "This is resampling, not a formant shift, so it changes how long the file plays - the "
        "length beside it is the real one. That matters for a one-shot, whose slot has a "
        "min/max and whose place in the impact stack is timed; it does not for a loop.");

    ImGui::SameLine();
    ImGui::TextUnformatted("level");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    if (ImGui::DragFloat("##trim", &e.trimDb, 0.1f, -24.0f, 12.0f, "%+.1f dB")) {
        e.trimDb = std::clamp(e.trimDb, -24.0f, 12.0f);
        changed = true;
    }
    Tip("Level for this file alone, applied at Stage 5 with the other trims.\n\n"
        "It cannot change which cue was chosen, and that is the ordering rather than a promise: "
        "nothing knows which *file* a layer resolved to until after arbitration has sorted, "
        "rate-capped and burst-shaped. See config.md.\n\n"
        "For one take that sits hot or shy against its siblings on a slot. If the whole slot is "
        "wrong, that is `SlotGain:f...` in the config panel, not this.");

    // The consequence of the pitch, in the place the pitch is turned. A length
    // that only appears in the metadata file is a length nobody reads until it
    // has already broken a spec.
    if (e.pitch != 1.0f && e.durationMs > 0.0f) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, kQuiet);
        ImGui::Text("plays %.0f ms (was %.0f)", e.EffectiveDurationMs(), e.durationMs);
        ImGui::PopStyleColor();
    }

    if (e.Corrected()) {
        ImGui::SameLine();
        if (ImGui::SmallButton("reset")) {
            e.pitch = 1.0f;
            e.trimDb = 0.0f;
            changed = true;
        }
        Tip("Back to the file as recorded - 1.00x and 0 dB.");
    }

    if (changed) {
        MarkDirty(e.file);
        // Same three as a mute, and for the same reason: a correction is a
        // statement about what the take plays, so it has to be audible on the
        // next block rather than after a reload.
        m_stale = true;
        m_libraryChanged = true;
    }

    ImGui::PopID();
    ImGui::PopID();
}

void SfxBrowser::Save() {
    if (m_library == nullptr) {
        return;
    }
    CommitEdits();
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

std::string SfxBrowser::ImportedLine(const rds::SfxEntry& entry) {
    const std::string when = rds::FormatImportTime(entry.importedAt);
    if (when.empty()) {
        return {};
    }
    const std::string ago = Ago(entry.importedAt);
    return ago.empty() ? std::format("imported {}", when)
                       : std::format("imported {} - {}", when, ago);
}

void SfxBrowser::CommitEdits() {
    if (m_library == nullptr) {
        return;
    }
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
        Row row;
        row.entry = i;
        row.disabled = entry.disabled;
        if (!needle.empty()) {
            // The name and every badge, and the badges are the warning codes and
            // the suggested slot names. Which of the two it hit is kept: typing
            // "body" means the sound called body far more often than it means
            // the ninety sounds the analysis suggested for imp_body, so the one
            // goes above the ninety rather than into them.
            const bool name = Lower(entry.name).find(needle) != std::string::npos ||
                              Lower(entry.file).find(needle) != std::string::npos;

            std::string tags;
            for (const rds::SfxWarning& w : entry.warnings) {
                tags += " " + Lower(w.code);
                // So "dead" finds everything nothing can mend in one search,
                // whatever each of them is called.
                if (w.dead) {
                    tags += " dead";
                }
            }
            for (const rds::SlotId slot : entry.suggested) {
                tags += " " + Lower(rds::Slot(slot).name);
            }
            if (entry.loops) {
                tags += " loop";
            }
            if (entry.disabled) {
                tags += " disabled muted";
            }
            const bool tag = tags.find(needle) != std::string::npos;

            if (!name && !tag) {
                continue;
            }
            row.match = name ? Match::kName : Match::kTag;
        }
        if (m_picking) {
            row.fit = SlotFit(entries[i], m_slot);
            row.lengthSuits = LengthSuits(entries[i].durationMs, m_slot, m_slotLoops);
        }
        m_rows.push_back(row);
    }

    // Stable, and over the library's own order, so with nothing typed and
    // nothing muted this is the order the folder is in - the sort only ever
    // moves the rows it has a reason to move.
    std::ranges::stable_sort(m_rows, [&](const Row& a, const Row& b) {
        // Muted last, ahead of every other consideration: a sound somebody
        // switched off is not a candidate, and leaving it in the band it would
        // have sorted into is how it gets picked again by mistake.
        if (a.disabled != b.disabled) {
            return !a.disabled;
        }
        // Then, while picking, the ones nothing can mend - a squared-off
        // waveform, hiss inside 30 dB of the hero, a duplicate of something
        // already in the library. Not hidden and not refused: they sort under
        // the sounds that are actually candidates, in the band above the muted,
        // because a slot filled with one of these is a slot that has to be
        // filled again later.
        const bool deadA = entries[a.entry].Dead();
        const bool deadB = entries[b.entry].Dead();
        if (m_picking && deadA != deadB) {
            return !deadA;
        }
        // Fitting lengths to the top - the right length is the hard filter,
        // everything after it is taste.
        if (m_picking && a.lengthSuits != b.lengthSuits) {
            return a.lengthSuits;
        }
        if (a.match != b.match) {
            return a.match < b.match;
        }
        // Inside a band, whatever the sort box says - and only inside it: every
        // test above this one has already had its say, so newest-first cannot
        // lift a muted sound, a wrong-length sound or a badge-only hit over the
        // rows that beat it on those. The library's own order is already by
        // name, so `name` while browsing is the order the folder listing has and
        // the stable sort leaves every row exactly where it was.
        const rds::SfxEntry& ea = entries[a.entry];
        const rds::SfxEntry& eb = entries[b.entry];
        switch (m_sort) {
            case Sort::kNewest:
                // By name inside one timestamp, so a batch that came in on the
                // same second - which is what importing twelve files at once
                // is - reads alphabetically rather than in scan order.
                if (ea.importedAt != eb.importedAt) {
                    return ea.importedAt > eb.importedAt;
                }
                return Lower(ea.name) < Lower(eb.name);
            case Sort::kOldest:
                if (ea.importedAt != eb.importedAt) {
                    return ea.importedAt < eb.importedAt;
                }
                return Lower(ea.name) < Lower(eb.name);
            case Sort::kName:
                break;
        }
        if (m_picking) {
            return Lower(ea.name) < Lower(eb.name);
        }
        return false;
    });

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

    const char* hint = m_picking ? "search name, warnings and suggested slots"
                                 : "search name and badges";
    if (m_focusSearch) {
        ImGui::SetKeyboardFocusHere();
        m_focusSearch = false;
    }
    ImGui::SetNextItemWidth(-490.0f);
    if (ImGui::InputTextWithHint("##search", hint, m_search, sizeof(m_search))) {
        m_stale = true;
        // A new search is a new list, and keeping the old index would leave the
        // highlight on whatever happened to land in that position.
        m_highlight = m_rows.empty() ? -1 : 0;
    }
    ImGui::SameLine();
    // "search" is in the label because the one thing this button must never be
    // read as is one that empties the library.
    if (ImGui::Button("Clear search")) {
        m_search[0] = '\0';
        m_stale = true;
    }
    Tip("Empties the search box, and nothing else - every file stays exactly where it is.\n\n"
        "Nothing in this window removes a sound except `delete` on its own row, and that asks "
        "first and tells you which slots play it.");

    ImGui::SameLine();
    ImGui::TextDisabled("sort");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(96.0f);
    {
        int sort = static_cast<int>(m_sort);
        if (ImGui::Combo("##sort", &sort, "name\0newest\0oldest\0")) {
            m_sort = static_cast<Sort>(sort);
            m_stale = true;
            // The rows are about to move under the highlight, and a highlight
            // left on an index is a highlight on a different sound.
            m_highlight = m_rows.empty() ? -1 : 0;
            m_scrollToHighlight = true;
        }
    }
    Tip("What orders the list. `newest` by default - what you are after is usually what you "
        "just imported.\n\n"
        "`newest` and `oldest` are by the date each file was imported, which the row under "
        "each name shows - so an evening's downloads come back as one block at the top.\n"
        "`name` is the library's own order, alphabetical.\n\n"
        "This only ever orders *inside* the bands. While picking for a slot the right length "
        "still comes first, the ones nothing can repair sit under the rest and the muted are "
        "still last; while searching, a name hit still beats a badge hit. Newest decides the "
        "order within each of those.");

    ImGui::SameLine();
    ImGui::TextDisabled("%d / %d", static_cast<int>(m_rows.size()),
                        static_cast<int>(m_library->Size()));

    // Right-aligned, on the row it belongs to: this is the volume of every Play
    // button in the window, so it sits with the list rather than in the header
    // bar with the things that write to disk.
    if (m_previewGainDb != nullptr) {
        ImGui::SameLine(ImGui::GetContentRegionMax().x - 168.0f);
        ImGui::TextDisabled("vol");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::SliderFloat("##previewgain", m_previewGainDb, -36.0f, 12.0f, "%+.0f dB",
                               ImGuiSliderFlags_AlwaysClamp)) {
            m_player->SetPreviewGainDb(*m_previewGainDb);
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
            *m_previewGainDb = 0.0f;
            m_player->SetPreviewGainDb(*m_previewGainDb);
        }
        Tip("How loud an audition is - the Play buttons here and on the slot panel, and the "
            "space bar on the highlighted row.\n\n"
            "The preview voice only. The take's own transport is untouched, and nothing here "
            "reaches the pack: this is the volume you listen at, not a gain on the sound.\n\n"
            "Right-click for 0 dB. Remembered between launches.");
    }

    HandleKeys(pick);

    ImGui::Separator();
    DrawList(pick);

    DrawDeleteConfirm();

    ImGui::End();
    if (!open) {
        Close();
    }
    if (pick.made) {
        Close();
    }
    return pick;
}

void SfxBrowser::DrawDeleteConfirm() {
    if (m_openConfirm) {
        ImGui::OpenPopup(kDeletePopup);
        m_openConfirm = false;
    }
    // Centred on the app rather than on the mouse, because the Del key opens
    // this too and a dialog that appears wherever the pointer was left is a
    // dialog somebody confirms without reading.
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(
        ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f),
        ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(kDeletePopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    const rds::SfxEntry* entry = m_library->Find(m_confirmFile);
    if (entry == nullptr) {
        // It went while the dialog was up - a re-measure that could not read it,
        // or a second delete. Nothing to ask about.
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }

    ImGui::TextUnformatted(entry->name.c_str());
    ImGui::SameLine();
    sfxui::Badges(*entry, false);
    ImGui::TextDisabled("%s", entry->file.c_str());

    ImGui::Spacing();
    const std::vector<std::string> slots = SlotsUsing(entry->file);
    if (slots.empty()) {
        ImGui::TextDisabled("No slot plays it.");
    } else {
        std::string list;
        for (const std::string& name : slots) {
            list += (list.empty() ? "" : ", ") + name;
        }
        ImGui::TextColored(kWarn, "%d slot(s) play it: %s", static_cast<int>(slots.size()),
                           list.c_str());
        ImGui::TextDisabled("It comes off them. That much is on the undo stack - Ctrl+Z puts the\n"
                            "assignment back, and then names a file that is not there any more.");
    }

    ImGui::Spacing();
    ImGui::PushTextWrapPos(430.0f);
    ImGui::TextUnformatted(
        "The sound and its .meta.ini go to the recycle bin together - the name, the import "
        "date, the mute and every measurement with them. Restoring the pair from the bin puts it "
        "back whole, and nothing in here will find it before you do.");
    ImGui::PopTextWrapPos();

    ImGui::Spacing();
    ImGui::Separator();

    if (ImGui::Button("Cancel", ImVec2(110.0f, 0.0f))) {
        m_confirmFile.clear();
        ImGui::CloseCurrentPopup();
    }
    // Cancel takes the keyboard, so Enter on a dialog nobody read is the answer
    // that changes nothing.
    ImGui::SetItemDefaultFocus();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.42f, 0.12f, 0.14f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.55f, 0.16f, 0.18f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.62f, 0.18f, 0.20f, 1.0f));
    const bool go = ImGui::Button("Delete", ImVec2(110.0f, 0.0f));
    ImGui::PopStyleColor(3);
    if (go) {
        DeleteEntry(m_confirmFile);
        m_confirmFile.clear();
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

void SfxBrowser::DrawHeader() {
    if (ImGui::Button("Import...")) {
        RunImport();
    }
    Tip("Pick one or more files. Each is converted to the pack's mono / 48 kHz / 16-bit, copied "
        "into the library, measured, and given a metadata file. Nothing is ever rejected - what is "
        "wrong with a file comes back as a badge you can read and ignore.\n\n"
        "On the way in it is also repaired, silently: peak normalised to -1.5 dBFS for the runtime "
        "pitch scatter, DC subtracted, head and trailing silence trimmed, a hard ending faded, and "
        "a stereo source whose channels fight each other kept as its left channel rather than "
        "summed. Those are Slots.md §5's delivery rules and none of them is a judgement, so none "
        "of them arrives as a badge.\n\n"
        "What is left on the row is what needs you: a decision (a second contact, a baked tail, a "
        "seam) or a dead end (a squared-off waveform, hiss inside 30 dB of the hit). Hover any "
        "badge for what it means and what to do.");

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

    // Same rule as the row button: only while there is something to repair, so
    // the header bar never carries a control that does nothing.
    if (const std::size_t broken = std::ranges::count_if(m_library->Entries(), NeedsRepair);
        broken != 0) {
        ImGui::SameLine();
        if (ImGui::Button(std::format("Repair {}##repairall", broken).c_str())) {
            int done = 0;
            // A copy of the names, for the same reason the re-measure takes one:
            // the library is written through while it is being walked.
            std::vector<std::string> files;
            for (const rds::SfxEntry& entry : m_library->Entries()) {
                if (NeedsRepair(entry)) {
                    files.push_back(entry.file);
                }
            }
            for (const std::string& file : files) {
                std::string error;
                if (RepairExisting(*m_library, file, error, nullptr)) {
                    ++done;
                } else {
                    spdlog::warn("sfx: repair {}: {}", file, error);
                }
            }
            m_importNote = std::format("repaired {} of {}", done, files.size());
            m_stale = true;
        }
        Tip(std::format("Run the import's own repair pass over the {} file(s) carrying a fault it "
                        "can fix: a rate or channel count that is not the pack's, a peak off "
                        "-1.5 dBFS, DC, head silence.\n\n"
                        "Everything an import touches is already repaired on the way in, so these "
                        "are files that arrived another way. Nothing else in the library is "
                        "opened, and a file whose only faults are the ones nothing mends - a "
                        "squared-off wave, a baked tail, a second contact - is not counted here.\n\n"
                        "Rewrites files in place. There is no undo.",
                        broken));
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
        "Names, mutes and import dates are kept.");

    if (!m_importNote.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", m_importNote.c_str());
    }

    if (Dirty()) {
        ImGui::SameLine();
        ImGui::TextColored(kMatch, "* %zu unsaved", DirtyCount());
        Tip("Names and mutes you have changed but not written. Ctrl+S saves everything unsaved, "
            "here and in the main window.");
    }
}

void SfxBrowser::DrawPickPreviews() {
    const rds::SlotDesc& desc = rds::Slot(m_slot);
    ImGui::TextColored(kSuggest, "%s", std::string(desc.name).c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("- %s  |  %.0f to %.0f ms%s", std::string(desc.character).c_str(),
                        desc.minLengthMs, desc.maxLengthMs, m_slotLoops ? "  |  looping" : "");

    ImGui::SameLine();
    ImGui::Checkbox("hear it in the take", &m_auditionInTake);
    Tip("Drop whatever is highlighted into this slot and re-mix the take, so moving down the "
        "list plays each candidate where it will actually land - under the transient, against "
        "the body layer, at the gain the arbitration gave it.\n\n"
        "Nothing is assigned by this: it lasts as long as the highlight does, and closing the "
        "window puts the slot's own sound back. Off if the take is long enough that the re-mix "
        "on every keypress is in the way.");

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
            // Here as well as in the list, because this is where you are looking
            // when you decide a sound is wrong: the slot's own row is the one
            // place the answer "not this, and not anywhere else either" is one
            // click away.
            ImGui::SameLine();
            DisableButton(static_cast<std::size_t>(entry - m_library->Entries().data()));
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
        if (m_auditionInTake) {
            ImGui::SameLine();
            ImGui::TextColored(kSuggest, "<- in the take");
            Tip("The take is mixed with this file in the slot right now. Play it and you are "
                "hearing this candidate in place.");
        }
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
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && m_highlight >= 0 &&
        m_highlight < static_cast<int>(m_rows.size())) {
        CommitEdits();
        m_confirmFile =
            m_library->Entries()[m_rows[static_cast<std::size_t>(m_highlight)].entry].file;
        m_openConfirm = true;
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

    const Row* previous = index > 0 ? &m_rows[static_cast<std::size_t>(index - 1)] : nullptr;

    // The band break: once, where the fitting lengths stop. Cheaper to read than
    // a badge on every row saying which side of the line it is on. Only in the
    // live block - the muted rows below carry their own break and would
    // otherwise draw a second one inside themselves.
    if (m_picking && !row.disabled && previous != nullptr && previous->lengthSuits &&
        !row.lengthSuits) {
        ImGui::Separator();
        ImGui::TextDisabled("  the wrong length for %s - still assignable",
                            std::string(rds::Slot(m_slot).name).c_str());
    }
    if (row.disabled && (previous == nullptr || !previous->disabled)) {
        ImGui::Separator();
        ImGui::TextDisabled("  muted - not played by anything until enabled");
    }

    ImGui::BeginGroup();

    sfxui::PreviewButton("play", *m_library, entry.file, *m_player, m_sampleRate);
    ImGui::SameLine();

    // The name, which doubles as the row's hit box. Double-click renames.
    if (m_renameRow == index) {
        ImGui::SetNextItemWidth(220.0f);
        ImGui::SetKeyboardFocusHere();
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
        } else if (entry.disabled) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.48f, 0.50f, 0.56f, 1.0f));
        }
        sfxui::HighlightedText(entry.name, m_search);
        if (highlighted || isCurrent || entry.disabled) {
            ImGui::PopStyleColor();
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            CommitEdits();
            m_renameRow = index;
            std::snprintf(m_renameBuffer, sizeof(m_renameBuffer), "%s", entry.name.c_str());
        }
        Tip(std::format("{}\n\ndouble-click to rename. The filename never changes, so renaming "
                        "cannot break an assignment.\n\n"
                        "{:.0f} ms, {} Hz, {} ch, {}-bit. peak {:.1f} dBFS, tilt {:+.1f} dB, "
                        "centroid {:.0f} Hz, {} lo-mid transients.\n"
                        "{}{}",
                        entry.file, entry.durationMs, entry.sampleRate, entry.channels,
                        entry.bitsPerSample, entry.peakDb, entry.tiltDb, entry.centroidHz,
                        entry.loMidTransients,
                        entry.noiseFloorDb < 99.0f
                            ? std::format("noise floor {:.0f} dB down. ", entry.noiseFloorDb)
                            : std::string(),
                        entry.contacts > 1
                            ? std::format("{} contacts, the second {:.0f} dB down at {:+.0f} ms.",
                                          entry.contacts, entry.satelliteDb, entry.satelliteAtMs)
                            : (entry.contacts == 1 ? std::string("one contact.") : std::string())));
    }

    ImGui::SameLine();
    sfxui::Badges(entry, true);

    // Only when it would do something. Every import is repaired on the way in,
    // so this is for the files that arrived another way - dropped into the
    // folder, written by sfx.py at another rate, imported before ffmpeg was on
    // PATH - and on everything else it is absent rather than inert.
    if (NeedsRepair(entry)) {
        ImGui::SameLine();
        if (ImGui::SmallButton("repair")) {
            const std::string file = entry.file;
            std::string error;
            std::string did;
            if (RepairExisting(*m_library, file, error, &did)) {
                m_importNote = did.empty() ? "nothing to repair" : "repaired: " + did;
            } else {
                m_importNote = "repair failed: " + error;
                spdlog::warn("sfx: repair {}: {}", file, error);
            }
            m_stale = true;
        }
        Tip("Rewrite the file the way an import would have: convert it to the pack's mono / "
            "48 kHz / 16-bit, subtract any DC, trim head and trailing silence, fade a hard "
            "ending, and normalise the peak to -1.5 dBFS for the runtime pitch scatter.\n\n"
            "Every one of those is mechanical - Slots.md §5 - which is why an import does them "
            "without asking and without saying so. This button exists for files that did not come "
            "through it.\n\n"
            "It does nothing to a file that is already right: a pack file at -1.0 dBFS sits inside "
            "the band that is left alone and comes back byte-identical. Loops are never trimmed or "
            "faded - their seam is `sfx.py make`'s job.\n\n"
            "The name, the mute and the import date are kept. There is no undo: the file is "
            "rewritten in place.");
    }

    ImGui::SameLine();
    DisableButton(row.entry);

    ImGui::SameLine();
    if (ImGui::SmallButton("delete")) {
        // The rename first: the row is about to be asked about, and a half-typed
        // name left in the buffer would be committed to whatever row inherits
        // this index after the list rebuilds.
        CommitEdits();
        m_confirmFile = entry.file;
        m_openConfirm = true;
    }
    Tip("Take it out of the library and send the file to the recycle bin. Asks first, and says "
        "which slots play it before it does.\n\n"
        "For a sound that should not have been imported. A sound that is merely wrong wants "
        "`disable` - that one is a mute you can take back, this one is a file you have to go "
        "and fetch.");

    if (m_picking) {
        ImGui::SameLine();
        if (ImGui::SmallButton(isCurrent ? "keep" : "use")) {
            pick.made = true;
            pick.slot = m_slot;
            pick.variant = m_variant;
            pick.file = entry.file;
        }
    }

    // When it arrived, on its own line under the name. Text rather than a badge
    // because it is the one piece of metadata that is a sentence, and it sits
    // here so the two import orders in the sort box have something to be read
    // against - a list ordered by a date nobody can see is a list in no order.
    ImGui::Indent(52.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.42f, 0.44f, 0.50f, 1.0f));
    if (const std::string line = ImportedLine(entry); line.empty()) {
        ImGui::TextUnformatted("imported - date unknown");
    } else {
        ImGui::TextUnformatted(line.c_str());
    }
    ImGui::PopStyleColor();
    Tip("When this file came into the library, from its metadata file.\n\n"
        "Anything imported before the date was recorded - and anything dropped into the "
        "library folder by hand - shows the file's own date instead, which is the same "
        "moment for everything the importer copied in.\n\n"
        "The sort box orders the list by this.");
    CorrectionRow(row.entry);
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
