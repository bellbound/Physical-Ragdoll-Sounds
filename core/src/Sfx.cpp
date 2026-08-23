#include "rds/Sfx.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <format>

#include "rds/Ini.h"
#include "rds/Pcm.h"

namespace fs = std::filesystem;

namespace rds {
namespace {

[[nodiscard]] std::size_t Index(SlotId id) { return static_cast<std::size_t>(id); }

[[nodiscard]] std::string Lower(std::string_view text) {
    std::string out(text);
    std::ranges::transform(out, out.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

/// What the library will index. Everything else in the folder - the sidecars
/// themselves, a readme, a stray png - is skipped silently.
[[nodiscard]] bool IsAudioExtension(std::string_view lowerExt) {
    return lowerExt == ".wav" || lowerExt == ".mp3" || lowerExt == ".ogg" || lowerExt == ".flac" ||
           lowerExt == ".m4a" || lowerExt == ".aac" || lowerExt == ".wma" || lowerExt == ".aiff" ||
           lowerExt == ".aif" || lowerExt == ".opus";
}

[[nodiscard]] double ToDouble(std::string_view text, double fallback = 0.0) {
    double value = fallback;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} ? value : fallback;
}

[[nodiscard]] int ToInt(std::string_view text, int fallback = 0) {
    int value = fallback;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} ? value : fallback;
}

[[nodiscard]] bool ToBool(std::string_view text, bool fallback = false) {
    if (text.empty()) {
        return fallback;
    }
    if (ini::EqualsIgnoreCase(text, "true") || ini::EqualsIgnoreCase(text, "yes")) {
        return true;
    }
    if (ini::EqualsIgnoreCase(text, "false") || ini::EqualsIgnoreCase(text, "no")) {
        return false;
    }
    return ToInt(text, fallback ? 1 : 0) != 0;
}

/// A note, on one line. Newlines become `\n` so the ini stays line-oriented -
/// a note is a sentence or two and a multi-line ini value is a parser nobody
/// wants to own.
[[nodiscard]] std::string EscapeLine(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            continue;
        } else {
            out += c;
        }
    }
    return out;
}

[[nodiscard]] std::string UnescapeLine(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\\' && i + 1 < text.size() && text[i + 1] == 'n') {
            out += '\n';
            ++i;
        } else {
            out += text[i];
        }
    }
    return out;
}

/// Warnings round-trip as `code|detail` on one `Warning` key each, so a file
/// with four of them reads as four lines rather than one unreadable one.
[[nodiscard]] SfxWarning ParseWarning(std::string_view text) {
    SfxWarning w;
    const auto bar = text.find('|');
    if (bar == std::string_view::npos) {
        w.code.assign(text);
        w.detail.assign(text);
        return w;
    }
    w.code.assign(ini::Trim(text.substr(0, bar)));
    std::string_view rest = ini::Trim(text.substr(bar + 1));
    // A second bar carries the blocking flag, which is absent on almost every
    // warning and so is not worth a key of its own.
    const auto second = rest.find('|');
    if (second != std::string_view::npos) {
        w.blocking = ToBool(ini::Trim(rest.substr(second + 1)));
        rest = ini::Trim(rest.substr(0, second));
    }
    w.detail.assign(rest);
    return w;
}

}  // namespace

// ═════════════════════════════════════════════════════════════════════════════
// SfxEntry
// ═════════════════════════════════════════════════════════════════════════════

bool SfxEntry::Blocked() const {
    return std::ranges::any_of(warnings, [](const SfxWarning& w) { return w.blocking; });
}

std::string SfxEntry::Stem() const { return fs::path(file).stem().string(); }

// ═════════════════════════════════════════════════════════════════════════════
// SfxLibrary
// ═════════════════════════════════════════════════════════════════════════════

fs::path SfxLibrary::MetaPathFor(const fs::path& wav) {
    // `<file>.meta.ini`, not `<stem>.meta.ini`: two files that differ only in
    // extension are two sfx and must not share one sidecar.
    return fs::path(wav).replace_extension(wav.extension().string() + ".meta.ini");
}

fs::path SfxLibrary::PathOf(std::string_view file) const { return m_directory / file; }

void SfxLibrary::Load(const fs::path& directory) {
    m_directory = directory;
    m_entries.clear();

    std::error_code ec;
    fs::create_directories(m_directory, ec);
    if (!fs::exists(m_directory, ec)) {
        spdlog::info("sfx: no library at {}", m_directory.string());
        return;
    }

    std::size_t withMeta = 0;
    for (const fs::directory_entry& item : fs::directory_iterator(m_directory, ec)) {
        if (!item.is_regular_file()) {
            continue;
        }
        const fs::path& path = item.path();
        if (!IsAudioExtension(Lower(path.extension().string()))) {
            continue;
        }

        SfxEntry entry;
        entry.file = path.filename().string();
        entry.name = path.stem().string();

        if (LoadMeta(path, entry)) {
            ++withMeta;
        } else {
            // No sidecar: say what the container says and leave the rest at
            // zero. The browser offers to measure it; until then it is a file
            // with a duration, which is enough to assign and play.
            const WavInfo info = ProbeWav(path.string());
            if (info.Valid()) {
                entry.sampleRate = info.sampleRate;
                entry.channels = info.channels;
                entry.bitsPerSample = info.bitsPerSample;
                entry.durationMs = 1000.0f * static_cast<float>(info.frames) /
                                   static_cast<float>(info.sampleRate);
            }
        }
        m_entries.push_back(std::move(entry));
    }
    if (ec) {
        spdlog::warn("sfx: scanning {} failed: {}", m_directory.string(), ec.message());
    }

    std::ranges::sort(m_entries, [](const SfxEntry& a, const SfxEntry& b) {
        return Lower(a.name) < Lower(b.name);
    });

    spdlog::info("sfx: library {}: {} file(s), {} with metadata", m_directory.string(),
                 m_entries.size(), withMeta);
}

const SfxEntry* SfxLibrary::Find(std::string_view file) const {
    const auto it = std::ranges::find_if(
        m_entries, [&](const SfxEntry& e) { return ini::EqualsIgnoreCase(e.file, file); });
    return it == m_entries.end() ? nullptr : &*it;
}

SfxEntry* SfxLibrary::Find(std::string_view file) {
    return const_cast<SfxEntry*>(std::as_const(*this).Find(file));
}

void SfxLibrary::Upsert(const SfxEntry& entry) {
    if (SfxEntry* existing = Find(entry.file); existing != nullptr) {
        *existing = entry;
    } else {
        m_entries.push_back(entry);
        std::ranges::sort(m_entries, [](const SfxEntry& a, const SfxEntry& b) {
            return Lower(a.name) < Lower(b.name);
        });
    }
    SaveMeta(entry);
}

bool SfxLibrary::SaveMeta(const SfxEntry& entry) const {
    std::string out;
    out += "; Physical Ragdoll Sounds - sfx metadata\n";
    out += "; Written by the testbench when this file was imported or edited. Everything under\n";
    out += "; [Measured] is what the importer measured; Name and Note are yours. Deleting this\n";
    out += "; file loses the measurements and the note, not the sound.\n\n";

    out += "[Sfx]\n";
    out += std::format("Name = {}\n", EscapeLine(entry.name));
    out += std::format("Note = {}\n", EscapeLine(entry.note));
    out += std::format("Loops = {}\n\n", entry.loops ? 1 : 0);

    out += "[Format]\n";
    out += std::format("SampleRate = {}\n", entry.sampleRate);
    out += std::format("Channels = {}\n", entry.channels);
    out += std::format("BitsPerSample = {}\n", entry.bitsPerSample);
    out += std::format("DurationMs = {:.1f}\n\n", entry.durationMs);

    out += "; Measured the way tools/sfx.py measures them, so these numbers can be compared\n";
    out += "; against the tables in Slots.md directly. Tilt is (sub+low)/2 - (high+air)/2 in dB:\n";
    out += "; positive is bass-led, negative is bright, and it is the discriminator that works.\n";
    out += "[Measured]\n";
    out += std::format("PeakDb = {:.2f}\n", entry.peakDb);
    out += std::format("LeadInMs = {:.1f}\n", entry.leadInMs);
    out += std::format("UsableMs = {:.1f}\n", entry.usableMs);
    out += std::format("Decay20Ms = {:.1f}\n", entry.decay20Ms);
    out += std::format("CentroidHz = {:.0f}\n", entry.centroidHz);
    out += std::format("TiltDb = {:.2f}\n", entry.tiltDb);
    out += std::format("DcOffset = {:.5f}\n", entry.dcOffset);
    out += std::format("LoMidTransients = {}\n", entry.loMidTransients);
    out += std::format("SeamDb = {:.2f}\n", entry.seamDb);
    out += std::format("SteadyDb = {:.2f}\n", entry.steadyDb);
    out += std::format("GrainsPerSec = {:.1f}\n\n", entry.grainsPerSec);

    out += "; Slots this suits, best first. A suggestion the browser sorts on, never a rule -\n";
    out += "; anything can be assigned to anything.\n";
    out += "[Suggested]\n";
    std::vector<std::string> names;
    names.reserve(entry.suggested.size());
    for (const SlotId slot : entry.suggested) {
        names.emplace_back(ToString(slot));
    }
    out += std::format("Slots = {}\n\n", ini::JoinList(names));

    out += "; What the file breaks, as `code|detail` - or `code|detail|1` for the one class\n";
    out += "; that cannot be played at all. None of these stop it being assigned.\n";
    out += "[Warnings]\n";
    for (const SfxWarning& w : entry.warnings) {
        out += std::format("Warning = {}|{}{}\n", EscapeLine(w.code), EscapeLine(w.detail),
                           w.blocking ? "|1" : "");
    }

    return ini::WriteFile(MetaPathFor(PathOf(entry.file)), out);
}

bool SfxLibrary::LoadMeta(const fs::path& wav, SfxEntry& out) {
    const auto lines = ini::ReadLines(MetaPathFor(wav));
    if (lines.empty()) {
        return false;
    }

    std::string section;
    for (const std::string& raw : lines) {
        if (const auto header = ini::SectionOf(raw); !header.empty()) {
            section.assign(header);
            continue;
        }
        std::string_view key;
        std::string_view value;
        if (!ini::SplitAssignment(raw, key, value)) {
            continue;
        }

        if (ini::EqualsIgnoreCase(section, "Sfx")) {
            if (ini::EqualsIgnoreCase(key, "Name")) out.name = UnescapeLine(value);
            else if (ini::EqualsIgnoreCase(key, "Note")) out.note = UnescapeLine(value);
            else if (ini::EqualsIgnoreCase(key, "Loops")) out.loops = ToBool(value);
        } else if (ini::EqualsIgnoreCase(section, "Format")) {
            if (ini::EqualsIgnoreCase(key, "SampleRate")) out.sampleRate = ToInt(value);
            else if (ini::EqualsIgnoreCase(key, "Channels")) out.channels = ToInt(value);
            else if (ini::EqualsIgnoreCase(key, "BitsPerSample")) out.bitsPerSample = ToInt(value);
            else if (ini::EqualsIgnoreCase(key, "DurationMs")) out.durationMs = static_cast<float>(ToDouble(value));
        } else if (ini::EqualsIgnoreCase(section, "Measured")) {
            if (ini::EqualsIgnoreCase(key, "PeakDb")) out.peakDb = static_cast<float>(ToDouble(value));
            else if (ini::EqualsIgnoreCase(key, "LeadInMs")) out.leadInMs = static_cast<float>(ToDouble(value));
            else if (ini::EqualsIgnoreCase(key, "UsableMs")) out.usableMs = static_cast<float>(ToDouble(value));
            else if (ini::EqualsIgnoreCase(key, "Decay20Ms")) out.decay20Ms = static_cast<float>(ToDouble(value));
            else if (ini::EqualsIgnoreCase(key, "CentroidHz")) out.centroidHz = static_cast<float>(ToDouble(value));
            else if (ini::EqualsIgnoreCase(key, "TiltDb")) out.tiltDb = static_cast<float>(ToDouble(value));
            else if (ini::EqualsIgnoreCase(key, "DcOffset")) out.dcOffset = static_cast<float>(ToDouble(value));
            else if (ini::EqualsIgnoreCase(key, "LoMidTransients")) out.loMidTransients = ToInt(value);
            else if (ini::EqualsIgnoreCase(key, "SeamDb")) out.seamDb = static_cast<float>(ToDouble(value));
            else if (ini::EqualsIgnoreCase(key, "SteadyDb")) out.steadyDb = static_cast<float>(ToDouble(value));
            else if (ini::EqualsIgnoreCase(key, "GrainsPerSec")) out.grainsPerSec = static_cast<float>(ToDouble(value));
        } else if (ini::EqualsIgnoreCase(section, "Suggested")) {
            if (ini::EqualsIgnoreCase(key, "Slots")) {
                out.suggested.clear();
                for (const std::string& name : ini::SplitList(value)) {
                    for (const SlotDesc& desc : Slots()) {
                        if (ini::EqualsIgnoreCase(desc.name, name)) {
                            out.suggested.push_back(desc.id);
                            break;
                        }
                    }
                }
            }
        } else if (ini::EqualsIgnoreCase(section, "Warnings")) {
            if (ini::EqualsIgnoreCase(key, "Warning")) {
                out.warnings.push_back(ParseWarning(value));
            }
        }
    }
    if (out.name.empty()) {
        out.name = wav.stem().string();
    }
    return true;
}

// ═════════════════════════════════════════════════════════════════════════════
// SfxAssignments
// ═════════════════════════════════════════════════════════════════════════════

SfxAssignments::SfxAssignments() {
    for (const SlotDesc& desc : Slots()) {
        m_slots[Index(desc.id)].looping = desc.isLoop;
    }
}

const SlotAssignment& SfxAssignments::For(SlotId slot) const {
    return m_slots[std::min(Index(slot), Index(SlotId::kCount) - 1)];
}

SlotAssignment& SfxAssignments::For(SlotId slot) {
    return m_slots[std::min(Index(slot), Index(SlotId::kCount) - 1)];
}

bool SfxAssignments::Empty() const { return AssignedSlots() == 0; }

std::size_t SfxAssignments::AssignedSlots() const {
    std::size_t count = 0;
    for (const SlotAssignment& slot : m_slots) {
        if (!slot.Empty()) {
            ++count;
        }
    }
    return count;
}

bool SfxAssignments::IsUsed(std::string_view file) const {
    for (const SlotAssignment& slot : m_slots) {
        for (const std::string& name : slot.files) {
            if (ini::EqualsIgnoreCase(name, file)) {
                return true;
            }
        }
    }
    return false;
}

bool SfxAssignments::operator==(const SfxAssignments& other) const {
    for (std::size_t i = 0; i < Index(SlotId::kCount); ++i) {
        if (m_slots[i].looping != other.m_slots[i].looping ||
            m_slots[i].files != other.m_slots[i].files) {
            return false;
        }
    }
    return true;
}

void SfxAssignments::SeedFromNames(const SfxLibrary& library) {
    // `<slot>_<NN>` is the naming `sfx.py make` writes and the convention the
    // bank used before there was an ini, so an existing pack seeds itself.
    // Ordered by filename, which is what the old scan sorted on - so a seeded
    // ini reproduces exactly the variant order the pack already had.
    struct Candidate {
        SlotId slot;
        std::string file;
    };
    std::vector<Candidate> found;
    for (const SfxEntry& entry : library.Entries()) {
        const std::string stem = entry.Stem();
        const auto underscore = stem.find_last_of('_');
        if (underscore == std::string::npos) {
            continue;
        }
        const std::string_view name{stem.data(), underscore};
        int number = 0;
        if (std::from_chars(stem.data() + underscore + 1, stem.data() + stem.size(), number).ec !=
            std::errc{}) {
            continue;
        }
        for (const SlotDesc& desc : Slots()) {
            if (desc.name == name) {
                found.push_back({desc.id, entry.file});
                break;
            }
        }
    }
    std::ranges::sort(found, [](const Candidate& a, const Candidate& b) { return a.file < b.file; });

    std::size_t filled = 0;
    for (const Candidate& c : found) {
        SlotAssignment& slot = For(c.slot);
        if (!slot.Empty() && std::ranges::find(slot.files, c.file) != slot.files.end()) {
            continue;
        }
        slot.files.push_back(c.file);
        ++filled;
    }
    if (filled != 0) {
        spdlog::info("sfx: seeded {} assignment(s) from <slot>_<NN> filenames", filled);
    }
}

std::size_t SfxAssignments::Load(const fs::path& file) {
    const auto lines = ini::ReadLines(file);
    if (lines.empty()) {
        return 0;
    }

    // Only slots the file mentions are reset; a slot the file has nothing to
    // say about keeps its manifest default rather than being emptied, which is
    // what lets a hand-written file name three slots and leave the rest alone.
    std::string section;
    const SlotDesc* current = nullptr;
    std::size_t assigned = 0;
    for (const std::string& raw : lines) {
        if (const auto header = ini::SectionOf(raw); !header.empty()) {
            section.assign(header);
            current = nullptr;
            for (const SlotDesc& desc : Slots()) {
                if (ini::EqualsIgnoreCase(desc.name, section)) {
                    current = &desc;
                    break;
                }
            }
            if (current == nullptr) {
                spdlog::debug("sfx: ignoring unknown slot section [{}]", section);
            }
            continue;
        }
        std::string_view key;
        std::string_view value;
        if (!ini::SplitAssignment(raw, key, value) || current == nullptr) {
            continue;
        }
        SlotAssignment& slot = For(current->id);
        if (ini::EqualsIgnoreCase(key, "Sfx")) {
            slot.files = ini::SplitList(value);
            if (!slot.files.empty()) {
                ++assigned;
            }
        } else if (ini::EqualsIgnoreCase(key, "Looping")) {
            slot.looping = ToBool(value, current->isLoop);
        }
    }
    return assigned;
}

bool SfxAssignments::Save(const fs::path& file) const {
    std::string out;
    out += "; RagdollSounds_SFX.ini - which sfx each slot plays\n";
    out += ";\n";
    out += "; One section per slot. `Sfx` is a comma-separated list of filenames in the\n";
    out += "; library folder beside this one - the engine picks between them with a shuffle\n";
    out += "; bag, so a slot with three files does not repeat one twice in a row. The order\n";
    out += "; is the variant index, so re-ordering the list changes which file a recorded\n";
    out += "; cue plays; adding and removing is free.\n";
    out += ";\n";
    out += "; A slot left empty falls back to scanning for `<slot>_<NN>.wav` in sounds\\,\n";
    out += "; which is what the mod did before this file existed - so deleting a line here\n";
    out += "; goes back to the shipped pack rather than to silence.\n";
    out += ";\n";
    out += "; `Looping` says the slot's sound is a sustained texture the engine repeats\n";
    out += "; whole rather than an event. Loops are judged as textures: they are never too\n";
    out += "; long, and their seam matters instead of their attack.\n\n";

    for (const SlotDesc& desc : Slots()) {
        const SlotAssignment& slot = For(desc.id);
        out += std::format("; {}\n", desc.character);
        out += std::format("; {} - {:.0f} to {:.0f} ms, {} expected\n", desc.role, desc.minLengthMs,
                           desc.maxLengthMs, desc.expectedVariants);
        out += std::format("[{}]\n", desc.name);
        out += std::format("Sfx = {}\n", ini::JoinList(slot.files));
        out += std::format("Looping = {}\n\n", slot.looping ? 1 : 0);
    }

    return ini::WriteFile(file, out);
}

}  // namespace rds
