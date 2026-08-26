#include "rds/Sfx.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <chrono>
#include <ctime>
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

[[nodiscard]] std::int64_t ToInt64(std::string_view text, std::int64_t fallback = 0) {
    std::int64_t value = fallback;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} ? value : fallback;
}

/// A file's last-write time in seconds since the unix epoch, or 0 when it cannot
/// be read. What an entry with no recorded import date falls back to.
[[nodiscard]] std::int64_t FileTimeSeconds(const fs::path& file) {
    std::error_code ec;
    const auto stamp = fs::last_write_time(file, ec);
    if (ec) {
        return 0;
    }
    const auto sys = std::chrono::clock_cast<std::chrono::system_clock>(stamp);
    return std::chrono::duration_cast<std::chrono::seconds>(sys.time_since_epoch()).count();
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

/// A value on one line. Newlines become `\n` so the ini stays line-oriented -
/// these are names and warning sentences, and a multi-line ini value is a parser
/// nobody wants to own.
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
    // A second bar carries the severity, which is absent on almost every warning
    // and so is not worth a key of its own. `1` is the original blocking flag
    // and is still written that way; `dead` is the second class, and sidecars
    // that predate it simply have neither.
    const auto second = rest.find('|');
    if (second != std::string_view::npos) {
        const std::string_view flag = ini::Trim(rest.substr(second + 1));
        if (ini::EqualsIgnoreCase(flag, "dead")) {
            w.dead = true;
        } else {
            w.blocking = ToBool(flag);
        }
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

bool SfxEntry::Dead() const {
    return std::ranges::any_of(warnings, [](const SfxWarning& w) { return w.dead; });
}

std::string SfxEntry::Stem() const { return fs::path(file).stem().string(); }

std::string FormatImportTime(std::int64_t unixSeconds) {
    if (unixSeconds <= 0) {
        return {};
    }
    const auto stamp = static_cast<std::time_t>(unixSeconds);
    std::tm local{};
#ifdef _WIN32
    if (localtime_s(&local, &stamp) != 0) {
        return {};
    }
#else
    if (localtime_r(&stamp, &local) == nullptr) {
        return {};
    }
#endif
    // Local time, because the question it answers is "was this before or after
    // dinner", and to the minute, because the second an import landed on has
    // never been the thing anybody wanted to know.
    return std::format("{:04}-{:02}-{:02} {:02}:{:02}", local.tm_year + 1900, local.tm_mon + 1,
                       local.tm_mday, local.tm_hour, local.tm_min);
}

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

        // A sidecar written before there was an import date, or no sidecar at
        // all. The file's own modification time is the honest answer: for
        // anything the importer copied in it *is* the import time, and for a
        // file dropped into the folder by hand it is when it got here.
        if (entry.importedAt == 0) {
            entry.importedAt = FileTimeSeconds(path);
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

bool SfxLibrary::Remove(std::string_view file) {
    const auto it = std::ranges::find_if(
        m_entries, [&](const SfxEntry& e) { return ini::EqualsIgnoreCase(e.file, file); });
    if (it == m_entries.end()) {
        return false;
    }
    m_entries.erase(it);
    return true;
}

bool SfxLibrary::SaveMeta(const SfxEntry& entry) const {
    std::string out;
    out += "; Physical Ragdoll Sounds - sfx metadata\n";
    out += "; Written by the testbench when this file was imported or edited. Everything under\n";
    out += "; [Measured] is what the importer measured; Name, Disabled, Pitch and TrimDb\n";
    out += "; are yours. Deleting this file loses the measurements, the import date, the\n";
    out += "; mute and the corrections - not the sound.\n\n";

    out += "[Sfx]\n";
    out += std::format("Name = {}\n", EscapeLine(entry.name));
    // Seconds since the epoch, because that is what sorts. The readable form
    // goes above it as a comment rather than into a second key: two keys saying
    // the same thing is two keys that can disagree.
    if (const std::string when = FormatImportTime(entry.importedAt); !when.empty()) {
        out += std::format("; imported {}\n", when);
    }
    out += std::format("Imported = {}\n", entry.importedAt);
    out += std::format("Loops = {}\n", entry.loops ? 1 : 0);
    out += std::format("Disabled = {}\n", entry.disabled ? 1 : 0);
    // The corrections. Written every time rather than only when set, because a
    // key that appears when it is non-default is a key nobody knows exists.
    out += "; Corrections to this recording, applied wherever it is used. Pitch is a playback\n";
    out += "; rate (1 = as recorded) and it changes a one-shot's length; TrimDb is level only,\n";
    out += "; applied after arbitration, so it can never change which cue was chosen.\n";
    out += std::format("Pitch = {:.4f}\n", entry.pitch);
    out += std::format("TrimDb = {:.2f}\n", entry.trimDb);
    if (entry.pitch != 1.0f) {
        out += std::format("; plays for {:.1f} ms at this pitch\n", entry.EffectiveDurationMs());
    }
    out += "\n";

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

    out += "; The technical rule-outs, on tools/triage_batch.py's thresholds: hiss under 30 dB\n";
    out += "; down, a squared-off waveform, a second contact riding under the hero, and a top\n";
    out += "; octave that is not there. Hash is over the samples, so the same sound under two\n";
    out += "; names has the same one.\n";
    out += "[Technical]\n";
    out += std::format("NoiseFloorDb = {:.1f}\n", entry.noiseFloorDb);
    out += std::format("ClipPct = {:.3f}\n", entry.clipPct);
    out += std::format("ClipRuns = {}\n", entry.clipRuns);
    out += std::format("Contacts = {}\n", entry.contacts);
    out += std::format("SatelliteDb = {:.1f}\n", entry.satelliteDb);
    out += std::format("SatelliteAtMs = {:.0f}\n", entry.satelliteAtMs);
    out += std::format("TopOctaveDb = {:.1f}\n", entry.topOctaveDb);
    out += std::format("ContentHash = {:016x}\n\n", entry.contentHash);

    out += "; Slots this suits, best first. A suggestion the browser sorts on, never a rule -\n";
    out += "; anything can be assigned to anything.\n";
    out += "[Suggested]\n";
    std::vector<std::string> names;
    names.reserve(entry.suggested.size());
    for (const SlotId slot : entry.suggested) {
        names.emplace_back(ToString(slot));
    }
    out += std::format("Slots = {}\n\n", ini::JoinList(names));

    out += "; What the file breaks, as `code|detail` - `code|detail|1` for the one class that\n";
    out += "; cannot be played at all, and `code|detail|dead` for the ones that play and cannot\n";
    out += "; be repaired. None of these stop it being assigned.\n";
    out += "[Warnings]\n";
    for (const SfxWarning& w : entry.warnings) {
        const char* severity = w.blocking ? "|1" : (w.dead ? "|dead" : "");
        out += std::format("Warning = {}|{}{}\n", EscapeLine(w.code), EscapeLine(w.detail),
                           severity);
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
            else if (ini::EqualsIgnoreCase(key, "Imported")) out.importedAt = ToInt64(value);
            else if (ini::EqualsIgnoreCase(key, "Loops")) out.loops = ToBool(value);
            else if (ini::EqualsIgnoreCase(key, "Disabled")) out.disabled = ToBool(value);
            // Clamped on the way in, not on the way out: a hand-edited 0 would
            // divide by zero in EffectiveDurationMs and a hand-edited 50 would
            // resample a 240 ms impact into a 5 ms tick. The bounds are the
            // browser's slider range, so a file edited by hand and a file edited
            // in the panel cannot end up in different places.
            else if (ini::EqualsIgnoreCase(key, "Pitch"))
                out.pitch = std::clamp(static_cast<float>(ToDouble(value)), 0.5f, 2.0f);
            else if (ini::EqualsIgnoreCase(key, "TrimDb"))
                out.trimDb = std::clamp(static_cast<float>(ToDouble(value)), -24.0f, 12.0f);
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
        } else if (ini::EqualsIgnoreCase(section, "Technical")) {
            if (ini::EqualsIgnoreCase(key, "NoiseFloorDb")) out.noiseFloorDb = static_cast<float>(ToDouble(value));
            else if (ini::EqualsIgnoreCase(key, "ClipPct")) out.clipPct = static_cast<float>(ToDouble(value));
            else if (ini::EqualsIgnoreCase(key, "ClipRuns")) out.clipRuns = ToInt(value);
            else if (ini::EqualsIgnoreCase(key, "Contacts")) out.contacts = ToInt(value);
            else if (ini::EqualsIgnoreCase(key, "SatelliteDb")) out.satelliteDb = static_cast<float>(ToDouble(value));
            else if (ini::EqualsIgnoreCase(key, "SatelliteAtMs")) out.satelliteAtMs = static_cast<float>(ToDouble(value));
            else if (ini::EqualsIgnoreCase(key, "TopOctaveDb")) out.topOctaveDb = static_cast<float>(ToDouble(value));
            else if (ini::EqualsIgnoreCase(key, "ContentHash")) {
                // Hex, and 16 digits of it overflow every signed parser in this
                // file - so it is the one field read on its own.
                const std::string_view hex = ini::Trim(value);
                std::uint64_t parsed = 0;
                if (std::from_chars(hex.data(), hex.data() + hex.size(), parsed, 16).ec ==
                    std::errc{}) {
                    out.contentHash = parsed;
                }
            }
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

VariantCondition SlotAssignment::ConditionAt(std::size_t index) const {
    return index < conditions.size() ? conditions[index] : VariantCondition{};
}

void SlotAssignment::SetConditionAt(std::size_t index, VariantCondition condition) {
    if (index >= files.size()) {
        return;
    }
    NormalizeConditions();
    conditions[index] = condition;
}

void SlotAssignment::Add(std::string file, VariantCondition condition) {
    files.push_back(std::move(file));
    NormalizeConditions();
    conditions.back() = condition;
}

void SlotAssignment::ReplaceAt(std::size_t index, std::string file) {
    if (index >= files.size()) {
        return;
    }
    const std::string was = files[index];
    files[index] = std::move(file);
    // The mute went with the sound, not with the position - unless another
    // placement of that sound is still here, in which case the mute is still
    // about something on the slot and stays.
    if (std::ranges::none_of(files, [&](const std::string& name) {
            return ini::EqualsIgnoreCase(name, was);
        })) {
        Unmute(was);
    }
    NormalizeConditions();
}

void SlotAssignment::RemoveAt(std::size_t index) {
    if (index >= files.size()) {
        return;
    }
    const std::string gone = files[index];
    NormalizeConditions();
    files.erase(files.begin() + static_cast<std::ptrdiff_t>(index));
    conditions.erase(conditions.begin() + static_cast<std::ptrdiff_t>(index));
    // Unless the same file is still on the slot elsewhere, in which case the
    // mute is still about a sound that is here.
    if (std::ranges::none_of(files, [&](const std::string& name) {
            return ini::EqualsIgnoreCase(name, gone);
        })) {
        Unmute(gone);
    }
}

void SlotAssignment::NormalizeConditions() { conditions.resize(files.size()); }

std::string SlotAssignment::PlacementTag(std::size_t index) const {
    if (index >= files.size()) {
        return {};
    }
    const std::string& name = files[index];
    std::size_t seen = 0;
    std::size_t total = 0;
    for (std::size_t i = 0; i < files.size(); ++i) {
        if (ini::EqualsIgnoreCase(files[i], name)) {
            ++total;
            if (i <= index) {
                ++seen;
            }
        }
    }
    return total > 1 ? std::format("{}#{}", name, seen) : name;
}

int SlotAssignment::PlacementOf(std::string_view tag) const {
    std::string_view name = ini::Trim(tag);
    // The whole tag as a filename first, because a wav is allowed to have a `#`
    // in its name and the suffix must not eat one. Only a tag that names
    // nothing on the slot is read as `name#N`.
    for (std::size_t i = 0; i < files.size(); ++i) {
        if (ini::EqualsIgnoreCase(files[i], name)) {
            return static_cast<int>(i);
        }
    }
    std::size_t want = 1;
    if (const std::size_t hash = name.rfind('#'); hash != std::string_view::npos) {
        const std::string_view digits = ini::Trim(name.substr(hash + 1));
        std::size_t parsed = 0;
        if (!digits.empty() &&
            std::ranges::all_of(digits, [](unsigned char c) { return std::isdigit(c) != 0; })) {
            for (const char c : digits) {
                parsed = parsed * 10 + static_cast<std::size_t>(c - '0');
            }
            if (parsed >= 1) {
                want = parsed;
                name = ini::Trim(name.substr(0, hash));
            }
        }
    }
    std::size_t seen = 0;
    for (std::size_t i = 0; i < files.size(); ++i) {
        if (ini::EqualsIgnoreCase(files[i], name) && ++seen == want) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool SlotAssignment::Muted(std::string_view file) const {
    return std::ranges::any_of(
        muted, [&](const std::string& name) { return ini::EqualsIgnoreCase(name, file); });
}

void SlotAssignment::Unmute(std::string_view file) {
    std::erase_if(muted,
                  [&](const std::string& name) { return ini::EqualsIgnoreCase(name, file); });
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

std::size_t SfxAssignments::UseCount(std::string_view file) const {
    std::size_t slots = 0;
    for (const SlotAssignment& slot : m_slots) {
        if (std::ranges::any_of(slot.files, [&](const std::string& name) {
                return ini::EqualsIgnoreCase(name, file);
            })) {
            ++slots;
        }
    }
    return slots;
}

std::size_t SfxAssignments::Forget(std::string_view file) {
    std::size_t slots = 0;
    for (SlotAssignment& slot : m_slots) {
        const std::size_t was = slot.files.size();
        // Backwards, so each removal takes its own condition with it and the
        // ones still to come keep the index they are being visited at. Every
        // placement of the file goes: Forget is the sound leaving the library.
        for (std::size_t i = slot.files.size(); i-- > 0;) {
            if (ini::EqualsIgnoreCase(slot.files[i], file)) {
                slot.RemoveAt(i);
            }
        }
        // The mute goes whether or not the slot named the file: a mute that
        // outlives its sound is exactly the thing Unmute exists to prevent, and
        // a slot can only be carrying one here if something already went wrong.
        slot.Unmute(file);
        if (slot.files.size() != was) {
            ++slots;
        }
    }
    return slots;
}

bool SfxAssignments::operator==(const SfxAssignments& other) const {
    for (std::size_t i = 0; i < Index(SlotId::kCount); ++i) {
        if (m_slots[i].looping != other.m_slots[i].looping ||
            m_slots[i].files != other.m_slots[i].files ||
            m_slots[i].muted != other.m_slots[i].muted) {
            return false;
        }
        // Per placement rather than as two vectors: a short `conditions` means
        // the rest are plain, so an untagged slot that has been normalised and
        // one that has not are the same assignment and must not light the
        // unsaved marker.
        for (std::size_t f = 0; f < m_slots[i].files.size(); ++f) {
            if (!(m_slots[i].ConditionAt(f) == other.m_slots[i].ConditionAt(f))) {
                return false;
            }
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
        slot.Add(c.file);
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
    // Held back until the whole file is read, because a condition names a
    // placement and the placements are `Sfx` - which a hand-written section is
    // free to put second. Resolving as we go would make the order of two lines
    // in a text file the difference between a tag landing and vanishing.
    struct PendingConditions {
        bool present{};
        std::vector<std::string> entries;
    };
    std::array<PendingConditions, static_cast<std::size_t>(SlotId::kCount)> pending{};
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
            // The tags named positions in the list this line just replaced.
            slot.conditions.clear();
            if (!slot.files.empty()) {
                ++assigned;
            }
        } else if (ini::EqualsIgnoreCase(key, "Muted")) {
            slot.muted = ini::SplitList(value);
        } else if (ini::EqualsIgnoreCase(key, "Conditions")) {
            PendingConditions& hold = pending[Index(current->id)];
            hold.present = true;
            hold.entries = ini::SplitList(value);
        } else if (ini::EqualsIgnoreCase(key, "Looping")) {
            slot.looping = ToBool(value, current->isLoop);
        }
    }

    for (const SlotDesc& desc : Slots()) {
        const PendingConditions& hold = pending[Index(desc.id)];
        if (!hold.present) {
            continue;
        }
        SlotAssignment& slot = For(desc.id);
        slot.conditions.assign(slot.files.size(), VariantCondition{});
        for (const std::string& entry : hold.entries) {
            // `plate_stone.wav : stone / heavy`, or `plate_stone.wav#2 : ...`
            // where the slot plays that file twice. A malformed entry is
            // skipped with a debug line rather than failing the load: one bad
            // condition must not cost somebody their whole pack.
            const std::size_t colon = entry.find(':');
            if (colon == std::string::npos) {
                spdlog::debug("sfx: {} has a condition with no ':' - '{}'", desc.name, entry);
                continue;
            }
            const std::string_view tag = ini::Trim(std::string_view{entry}.substr(0, colon));
            const std::string_view rest = ini::Trim(std::string_view{entry}.substr(colon + 1));
            const std::size_t slash = rest.find('/');
            const std::string_view left =
                slash == std::string_view::npos ? rest : rest.substr(0, slash);
            const std::string_view right =
                slash == std::string_view::npos ? std::string_view{} : rest.substr(slash + 1);
            VariantCondition condition{};
            condition.surface = SurfaceMatchFrom(ini::Trim(left));
            condition.coverage = CoverageMatchFrom(ini::Trim(right));
            if (condition.Unconditional()) {
                // Nothing to say. Dropped rather than stored, so the saved file
                // does not grow a line that means "no condition".
                continue;
            }
            const int index = slot.PlacementOf(tag);
            if (index < 0) {
                // A tag for a file the slot no longer plays - hand-edited, or
                // `Sfx` shortened without the `Conditions` line following it.
                // Dropped rather than guessed at: a condition that landed on
                // whichever file happened to be nearby would be worse than one
                // that is gone.
                spdlog::debug("sfx: {} tags '{}', which it does not play", desc.name, tag);
                continue;
            }
            slot.conditions[static_cast<std::size_t>(index)] = condition;
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
    out += "; `Muted` is a subset of `Sfx` that stays on the slot and never gets picked.\n";
    out += "; The file keeps its place in the list, so it keeps its variant index and\n";
    out += "; unmuting puts a recorded take back exactly as it was - which is the whole\n";
    out += "; difference from deleting it from `Sfx`. A slot with every file muted goes\n";
    out += "; silent rather than falling back to anything. It is by name, so a file\n";
    out += "; listed twice on a slot is muted in both places - a mute is about the sound,\n";
    out += "; not about one entry in the list.\n";
    out += ";\n";
    out += "; `Conditions` narrows when a file is a candidate: `file.wav : <surface> / <armour>`,\n";
    out += "; comma-separated, where either half may be `any`. A tagged file wins over the\n";
    out += "; plain ones when the contact matches, and is invisible when it does not - so\n";
    out += "; `imp_body` can carry one recording made for plate on flagstone without that\n";
    out += "; recording turning up anywhere else. Surfaces are soft, wood, stone, metal,\n";
    out += "; water, body; armour is bare, cloth, light, heavy.\n";
    out += ";\n";
    out += "; A tag is about one entry in `Sfx`, not about the sound: the same file may be\n";
    out += "; listed twice, once plain and once tagged, and then it is a candidate for\n";
    out += "; everything *and* the preferred one where the tag matches. Where a name is\n";
    out += "; listed more than once, `file.wav#2` says which entry is meant - counting from\n";
    out += "; 1, in `Sfx` order. A bare name is the first entry of it.\n";
    out += ";\n";
    out += "; A condition is a preference, never a mute. If nothing on the slot satisfies\n";
    out += "; one - a slot whose only file is tagged `stone`, on wood - the slot plays its\n";
    out += "; full set rather than going silent. Use `Muted` to silence something.\n";
    out += ";\n";
    out += "; `Looping` says the slot's sound is a sustained texture the engine repeats\n";
    out += "; whole rather than an event. Loops are judged as textures: they are never too\n";
    out += "; long, and their seam matters instead of their attack.\n\n";

    for (const SlotDesc& desc : Slots()) {
        const SlotAssignment& slot = For(desc.id);
        out += std::format("; {}\n", desc.character);
        out += std::format("; {} - {:.0f} to {:.0f} ms, {} expected\n", ToString(desc.family),
                           desc.minLengthMs,
                           desc.maxLengthMs, desc.expectedVariants);
        out += std::format("[{}]\n", desc.name);
        out += std::format("Sfx = {}\n", ini::JoinList(slot.files));
        out += std::format("Muted = {}\n", ini::JoinList(slot.muted));
        std::vector<std::string> conds;
        conds.reserve(slot.conditions.size());
        for (std::size_t i = 0; i < slot.files.size(); ++i) {
            const VariantCondition cond = slot.ConditionAt(i);
            if (cond.Unconditional()) {
                continue;
            }
            conds.push_back(std::format("{} : {} / {}", slot.PlacementTag(i),
                                        ToString(cond.surface), ToString(cond.coverage)));
        }
        out += std::format("Conditions = {}\n", ini::JoinList(conds));
        out += std::format("Looping = {}\n\n", slot.looping ? 1 : 0);
    }

    return ini::WriteFile(file, out);
}

}  // namespace rds
