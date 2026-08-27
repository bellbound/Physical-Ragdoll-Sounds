#include "rds/ConfigManager.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <string>
#include <vector>

#include "rds/Ini.h"
#include "rds/Log.h"

// Plain fstream rather than GetPrivateProfileString, for two reasons that are
// both load-bearing. The testbench has to parse these files with no Windows
// profile API involved and get byte-identical results, and a user's own comments
// have to survive a round trip - the ini is self-documenting by design, and an
// API that rewrites the whole file from its own idea of the format throws that
// away the first time anything is saved.

namespace rds {
namespace {

constexpr std::size_t kCommentWrapColumn = 96;

// The line-level reading lives in Ini.h now, shared with the sfx files. Pulled
// in by name rather than qualified at every call site: this file is a hundred
// lines of "is this line a section, a key or a comment" and the qualification
// would be most of what you read.
using ini::EqualsIgnoreCase;
using ini::ReadLines;
using ini::SectionOf;
using ini::SplitAssignment;
using ini::Trim;

[[nodiscard]] const ParamDesc* Find(std::span<const ParamDesc> params, std::string_view section,
                                    std::string_view key) {
    for (const auto& p : params) {
        if (EqualsIgnoreCase(p.section, section) && EqualsIgnoreCase(p.key, key)) {
            return &p;
        }
    }
    return nullptr;
}

/// The same, against the name a parameter used to have. Kept apart from Find so
/// that every caller has to decide what a legacy hit means: the reader takes the
/// value, the writer takes the line out.
[[nodiscard]] const ParamDesc* FindLegacy(std::span<const ParamDesc> params,
                                          std::string_view section, std::string_view key) {
    for (const auto& p : params) {
        if (!p.legacyKey.empty() && EqualsIgnoreCase(p.legacySection, section) &&
            EqualsIgnoreCase(p.legacyKey, key)) {
            return &p;
        }
        // The name before that one. A row that has moved twice has to answer to
        // both, or the second move silently discards the tuning of everybody who
        // never saved a file in between - which is most of them.
        if (!p.legacyKey2.empty() && EqualsIgnoreCase(p.legacySection2, section) &&
            EqualsIgnoreCase(p.legacyKey2, key)) {
            return &p;
        }
    }
    return nullptr;
}

/// Keys that no longer exist anywhere and should be taken out of a file rather
/// than copied through it. Different from a legacy name, which is a key that
/// *moved*: these six became thirteen blocks in a second file, which
/// `MigrateSurfaces` has already read. Left alone they would sit in every upgraded
/// install's ini looking editable and doing nothing.
[[nodiscard]] bool IsRetiredKey(std::string_view section, std::string_view key) {
    struct Retired {
        std::string_view section;
        std::string_view key;
    };
    static constexpr Retired kRetired[] = {
        {"Surfaces", "fWoodTrimDb"}, {"Surfaces", "fStoneTrimDb"}, {"Surfaces", "fSoftTrimDb"},
        {"Surfaces", "bWood"},       {"Surfaces", "bStone"},       {"Surfaces", "bSoft"},
        {"SlotGain", "fSurfWood"},   {"SlotGain", "fSurfStone"},   {"SlotGain", "fSurfSoft"},
        {"Layers", "bSurfWood"},     {"Layers", "bSurfStone"},     {"Layers", "bSurfSoft"},
    };
    for (const Retired& r : kRetired) {
        if (EqualsIgnoreCase(r.section, section) && EqualsIgnoreCase(r.key, key)) {
            return true;
        }
    }
    return false;
}

/// One paragraph of a tooltip, word-wrapped into `; ` comment lines.
void WriteTooltipParagraph(std::string& out, std::string_view text) {
    std::size_t cursor = 0;
    while (cursor < text.size()) {
        std::size_t take = std::min(kCommentWrapColumn, text.size() - cursor);
        if (cursor + take < text.size()) {
            const auto slice = text.substr(cursor, take + 1);
            const auto space = slice.find_last_of(' ');
            if (space != std::string_view::npos && space > 0) {
                take = space;
            }
        }
        out += "; ";
        out += text.substr(cursor, take);
        out += "\n";
        cursor += take;
        while (cursor < text.size() && text[cursor] == ' ') {
            ++cursor;
        }
    }
}

/// The tooltip, word-wrapped into `; ` comment lines - what makes a fresh install
/// ship a file somebody can read rather than ninety numbers.
///
/// Paragraphs first, then wrapping inside each. A tooltip is also rendered by the
/// slider panel, where a `\n` costs nothing, so the ini writer has to agree or it
/// emits the rest of the paragraph with no `; ` in front and the file stops being
/// an ini.
void WriteTooltip(std::string& out, std::string_view tooltip) {
    std::size_t start = 0;
    while (start <= tooltip.size()) {
        const auto brk = tooltip.find('\n', start);
        const auto line = tooltip.substr(start, brk == std::string_view::npos ? brk : brk - start);
        if (line.empty()) {
            // A blank line between paragraphs stays blank, but as a comment: an
            // actually empty line would end the block and orphan the key from
            // the half of its description below it.
            out += ";\n";
        } else {
            WriteTooltipParagraph(out, line);
        }
        if (brk == std::string_view::npos) {
            break;
        }
        start = brk + 1;
    }
}

void WriteParam(std::string& out, const void* root, const ParamDesc& p) {
    WriteTooltip(out, p.tooltip);
    if (p.type == ParamType::kEnum && !p.enumNames.empty()) {
        out += ";   ";
        for (std::size_t i = 0; i < p.enumNames.size(); ++i) {
            if (i != 0) {
                out += ", ";
            }
            out += std::format("{}={}", i, p.enumNames[i]);
        }
        out += "\n";
    }
    if (p.type == ParamType::kString) {
        // Unquoted, and running to the end of the line. Nothing here needs
        // escaping, and a Windows path with spaces in it reads back exactly as
        // it was typed.
        out += std::format("{} = {}\n\n", p.key, GetParamString(root, p));
        return;
    }
    out += std::format("{} = {}\n\n", p.key, FormatParam(p, GetParam(root, p)));
}

}  // namespace

ConfigManager& ConfigManager::Get() {
    static ConfigManager instance;
    return instance;
}

std::size_t ConfigManager::LoadInto(const std::filesystem::path& file, void* root,
                                    std::span<const ParamDesc> params) {
    const auto lines = ReadLines(file);
    if (lines.empty()) {
        return 0;
    }

    std::size_t found = 0;
    std::string section;
    for (const auto& raw : lines) {
        if (const auto header = SectionOf(raw); !header.empty()) {
            section.assign(header);
            continue;
        }
        std::string_view key;
        std::string_view value;
        if (!SplitAssignment(raw, key, value)) {
            continue;
        }
        const ParamDesc* desc = Find(params, section, key);
        if (desc == nullptr) {
            // ...or the name it had before it moved. A rename must not silently
            // cost somebody a value they tuned by ear, and the next save writes
            // the key out where it lives now.
            desc = FindLegacy(params, section, key);
            if (desc != nullptr) {
                spdlog::debug("config: [{}] {} is now {}", section, key, QualifiedKey(*desc));
            }
        }
        if (desc == nullptr) {
            // A key we removed should not cost a user their whole file, so it is
            // left exactly where it is and only mentioned at debug.
            spdlog::debug("config: ignoring unknown key [{}] {}", section, key);
            continue;
        }
        if (desc->type == ParamType::kString) {
            SetParamString(root, *desc, value);
            ++found;
            continue;
        }
        if (desc->type == ParamType::kString) {
            // No range to clamp and no number to log, so this is the whole of
            // the string path: take the text as it stands.
            SetParamString(root, *desc, value);
            ++found;
            continue;
        }
        const double parsed = ParseParam(*desc, value);
        const double coerced = CoerceParam(*desc, parsed);
        if (std::fabs(parsed - coerced) > 1e-9) {
            spdlog::warn("config: {} = {} is out of range, clamped to {}", QualifiedKey(*desc),
                         FormatParam(*desc, parsed), FormatParam(*desc, coerced));
        }
        SetParam(root, *desc, coerced);
        ++found;
    }
    return found;
}

std::string ConfigManager::ToIniText(const void* root, std::span<const ParamDesc> params,
                                     std::string_view header) {
    std::string out;
    if (!header.empty()) {
        out += std::format("; {}\n", header);
        out += "; Written by Physical Ragdoll Sounds. Every key carries the comment that says\n";
        out += "; what it changes perceptually; delete a key to go back to its default.\n\n";
    }
    std::string_view lastSection;
    for (const ParamDesc& p : params) {
        if (p.section != lastSection) {
            lastSection = p.section;
            out += std::format("\n[{}]\n\n", p.section);
        }
        WriteParam(out, root, p);
    }
    return out;
}

bool ConfigManager::SaveFrom(const std::filesystem::path& file, const void* root,
                             std::span<const ParamDesc> params, std::string_view header) {
    // Merge rather than regenerate. Every line the schema does not own - a
    // user's own comment, a key from a future version, a blank they put there on
    // purpose - is copied through verbatim, and only the value on a line we
    // recognise is rewritten. Anything missing is appended under its section
    // with its tooltip, which is what makes a partial file complete itself.
    const auto existing = ReadLines(file);
    if (existing.empty()) {
        // Nothing to merge with, so this is the whole file.
        return ini::WriteFile(file, ToIniText(root, params, header));
    }

    std::vector<bool> written(params.size(), false);
    std::string out;

    {
        std::string section;
        // Every key the schema puts in `name` that the file has not carried yet,
        // written at the end of that section's existing block.
        //
        // Without this the append pass at the bottom was the only thing writing a
        // missing key, and it opens a `[Section]` header per group - so a key added
        // to a section the file already had arrived under a *second* copy of that
        // header. Three rounds of adding keys to `[Damage]` produced three
        // `[Damage]` blocks, and the reader takes the last value it sees. Nothing
        // detected it: every key was present, every value right, round-trip passed.
        //
        // A genuinely new section still falls through to the append pass. `limit`
        // bounds this to the params before a given schema index, which puts a
        // re-added key back in its proper place: schema order is the ini's key
        // order by design (01 §7.3).
        const auto emitMissingFor = [&](std::string_view name, std::size_t limit) {
            if (name.empty()) {
                return;
            }
            for (std::size_t i = 0; i < limit && i < params.size(); ++i) {
                if (written[i] || params[i].section != name) {
                    continue;
                }
                written[i] = true;
                WriteParam(out, root, params[i]);
            }
        };
        // Comments and blank lines are held back rather than copied as they are
        // read. A key that has since moved to another section takes the tooltip
        // above it with it, and a paragraph left behind describing a key that is
        // no longer under that header is worse than no comment at all.
        std::vector<std::string> pending;
        const auto flush = [&] {
            for (const std::string& line : pending) {
                out += line;
                out += "\n";
            }
            pending.clear();
        };
        // A `[Surface.x]` block for a class `params` does not carry - one the user
        // closed - is dropped whole: header, keys and comments.
        //
        // Every other unrecognised line here is copied through, which is right for
        // a removed key and wrong for a list, where closing an entry is exactly
        // "these keys are no longer recognised". Dropping the *header* too is what
        // makes it safe: an empty `[Surface.wood]` still reads as opened on the
        // next load and would come back holding defaults.
        bool droppingSection = false;
        const auto isClosedSurfaceSection = [&](std::string_view name) {
            constexpr std::string_view kPrefix = "Surface.";
            if (name.size() <= kPrefix.size() ||
                !EqualsIgnoreCase(name.substr(0, kPrefix.size()), kPrefix)) {
                return false;
            }
            for (const auto& p : params) {
                if (EqualsIgnoreCase(p.section, name)) {
                    return false;
                }
            }
            return true;
        };
        for (const auto& raw : existing) {
            if (const auto sectionName = SectionOf(raw); !sectionName.empty()) {
                // Before the held-back comments, so a new key lands directly
                // under the last key of the block it belongs to and the blank
                // line that separated the sections stays next to the header it
                // was separating.
                emitMissingFor(section, params.size());
                section.assign(sectionName);
                droppingSection = isClosedSurfaceSection(section);
                if (droppingSection) {
                    pending.clear();
                    continue;
                }
                flush();
                out += raw;
                out += "\n";
                continue;
            }
            if (droppingSection) {
                continue;
            }
            std::string_view key;
            std::string_view value;
            if (SplitAssignment(raw, key, value)) {
                if (const ParamDesc* desc = Find(params, section, key); desc != nullptr) {
                    const auto index = static_cast<std::size_t>(desc - params.data());
                    // Anything the schema puts earlier in this section that the
                    // file has not carried yet goes in ahead of it, so a key
                    // added in a later version lands where it belongs instead of
                    // at the end of the block.
                    emitMissingFor(section, index);
                    written[index] = true;
                    flush();
                    out += desc->type == ParamType::kString
                               ? std::format("{} = {}\n", desc->key, GetParamString(root, *desc))
                               : std::format("{} = {}\n", desc->key,
                                             FormatParam(*desc, GetParam(root, *desc)));
                    continue;
                }
                if (FindLegacy(params, section, key) != nullptr) {
                    // The parameter has moved. Drop the old line and the comment
                    // that introduced it; the append pass below writes it out
                    // under the section it lives in now with a current tooltip.
                    // Nothing is lost - the load that fed this save read the
                    // value through the same legacy name.
                    pending.clear();
                    continue;
                }
                if (IsRetiredKey(section, key)) {
                    // Gone rather than moved, and already read by whatever
                    // migration replaced it. Same treatment: the line and the
                    // comment above it both go.
                    pending.clear();
                    continue;
                }
            } else if (const std::string_view text = Trim(raw);
                       text.empty() || text.front() == ';' || text.front() == '#') {
                // A comment or a blank. Held until the line it introduces says
                // whether it is still about anything.
                pending.emplace_back(raw);
                continue;
            }
            flush();
            out += raw;
            out += "\n";
        }
        // The last section in the file has no header after it to trigger the
        // emit, so it gets one here.
        emitMissingFor(section, params.size());
        flush();
        if (!out.empty() && out.back() != '\n') {
            out += "\n";
        }
    }

    // Whatever the file did not already carry, in schema order, grouped by
    // section so an appended block still reads like the rest of the file.
    std::string_view lastSection;
    for (std::size_t i = 0; i < params.size(); ++i) {
        if (written[i]) {
            continue;
        }
        const ParamDesc& p = params[i];
        if (p.section != lastSection) {
            lastSection = p.section;
            out += std::format("\n[{}]\n\n", p.section);
        }
        WriteParam(out, root, p);
    }

    return ini::WriteFile(file, out);
}

void ConfigManager::ReadOpenedSurfaces(const std::filesystem::path& file,
                                       AlgorithmConfig& config) {
    for (bool& open : config.surfaces.opened) {
        open = false;
    }
    for (const auto& raw : ReadLines(file)) {
        const auto header = SectionOf(raw);
        constexpr std::string_view kPrefix = "Surface.";
        if (header.size() <= kPrefix.size() ||
            !EqualsIgnoreCase(header.substr(0, kPrefix.size()), kPrefix)) {
            continue;
        }
        // Lowercased before the lookup because `ToString(SurfaceClass)` is all
        // lower and a hand-typed `[Surface.Ice]` should still open ice.
        std::string name{header.substr(kPrefix.size())};
        for (char& c : name) {
            c = (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
        }
        const SurfaceClass surface = SurfaceClassFrom(name);
        if (surface == SurfaceClass::kCount) {
            spdlog::debug("config: [{}] is not a surface class - ignored", header);
            continue;
        }
        config.surfaces.opened[static_cast<std::size_t>(surface)] = true;
    }
}

std::size_t ConfigManager::MigrateSurfaces(const std::filesystem::path& algorithmFile,
                                           AlgorithmConfig& config) {
    // The six keys the list replaced, with the names they had before the
    // surface section gathered them and the names they had inside it. Read by
    // hand rather than through the schema because the schema no longer has rows
    // for them - that is what makes this a migration and not a rename.
    struct Legacy {
        SurfaceClass surface;
        std::string_view section;
        std::string_view key;
        bool isTrim;
    };
    static constexpr Legacy kLegacy[] = {
        {SurfaceClass::kWood, "Surfaces", "fWoodTrimDb", true},
        {SurfaceClass::kStone, "Surfaces", "fStoneTrimDb", true},
        {SurfaceClass::kSoft, "Surfaces", "fSoftTrimDb", true},
        {SurfaceClass::kWood, "SlotGain", "fSurfWood", true},
        {SurfaceClass::kStone, "SlotGain", "fSurfStone", true},
        {SurfaceClass::kSoft, "SlotGain", "fSurfSoft", true},
        {SurfaceClass::kWood, "Surfaces", "bWood", false},
        {SurfaceClass::kStone, "Surfaces", "bStone", false},
        {SurfaceClass::kSoft, "Surfaces", "bSoft", false},
        {SurfaceClass::kWood, "Layers", "bSurfWood", false},
        {SurfaceClass::kStone, "Layers", "bSurfStone", false},
        {SurfaceClass::kSoft, "Layers", "bSurfSoft", false},
    };

    std::size_t opened = 0;
    std::string section;
    for (const auto& raw : ReadLines(algorithmFile)) {
        if (const auto header = SectionOf(raw); !header.empty()) {
            section.assign(header);
            continue;
        }
        std::string_view key;
        std::string_view value;
        if (!SplitAssignment(raw, key, value)) {
            continue;
        }
        for (const Legacy& l : kLegacy) {
            if (!EqualsIgnoreCase(section, l.section) || !EqualsIgnoreCase(key, l.key)) {
                continue;
            }
            const auto index = static_cast<std::size_t>(l.surface);
            SurfaceSkinConfig& skin = config.surfaces.skins[index];
            const std::string text{Trim(value)};
            if (l.isTrim) {
                float parsed = 0.0f;
                const auto* first = text.data();
                const auto* last = first + text.size();
                if (std::from_chars(first, last, parsed).ec != std::errc{} ||
                    parsed == SurfaceSkinConfig{}.trimDb) {
                    break;  // absent, unparseable, or the default: nothing to carry
                }
                skin.trimDb = parsed;
            } else {
                // Anything that is not a plain 0 is on, which is how every other
                // bool in these files is read.
                const bool on = !(text == "0" || EqualsIgnoreCase(text, "false"));
                if (on == SurfaceSkinConfig{}.enabled) {
                    break;
                }
                skin.enabled = on;
            }
            // Only now, so a file that mentions the key at its default leaves the
            // class closed. Opening on the mention rather than on the *value*
            // would give everybody three blocks of zeroes on first launch and
            // kill the inheritance before they ever saw it.
            if (!config.surfaces.opened[index]) {
                config.surfaces.opened[index] = true;
                ++opened;
            }
            break;
        }
    }
    return opened;
}

void ConfigManager::Initialize(const std::filesystem::path& directory) {
    m_directory = directory;
    m_generalPath = directory / "RagdollSounds.ini";
    m_algorithmPath = directory / "RagdollSounds_Algorithm.ini";
    m_surfacePath = directory / "RagdollSounds_Algorithm_Surfaces.ini";
    m_sfxPath = directory / "RagdollSounds_SFX.ini";

    std::error_code ec;
    std::filesystem::create_directories(m_directory, ec);
    if (ec) {
        spdlog::error("config: cannot create {} - running on defaults, nothing will be saved",
                      m_directory.string());
    }

    // Read what is there, then write it straight back. Round-tripping is what
    // fills in every key a partial file is missing, comment and all, so a fresh
    // install ends up with a complete self-documenting ini rather than an empty
    // one somebody has to guess at.
    m_general = GeneralConfig{};
    m_algorithm = AlgorithmConfig{};
    LoadInto(m_generalPath, &m_general, GeneralParams());
    LoadInto(m_algorithmPath, &m_algorithm, AlgorithmFileParams());

    // The surfaces list. If the file does not exist yet, this install predates
    // it, so the three trims and three mutes that used to live in the algorithm
    // file are folded into the list before anything is written back - and the
    // very next line writes the algorithm file *without* them, which is what
    // retires the old keys. There is no second migration to worry about: once
    // the surfaces file exists it is the only thing consulted.
    if (!std::filesystem::exists(m_surfacePath)) {
        const std::size_t opened = MigrateSurfaces(m_algorithmPath, m_algorithm);
        if (opened > 0) {
            spdlog::info("config: migrated {} surface(s) into {}", opened,
                         m_surfacePath.filename().string());
        }
    } else {
        ReadOpenedSurfaces(m_surfacePath, m_algorithm);
        LoadInto(m_surfacePath, &m_algorithm, SurfaceParams());
    }
    m_algorithm.surfaces.Resolve();

    SaveFrom(m_generalPath, &m_general, GeneralParams(), "RagdollSounds.ini - general settings");
    SaveFrom(m_algorithmPath, &m_algorithm, AlgorithmFileParams(),
             "RagdollSounds_Algorithm.ini - the sound engine");
    SaveSurfaces();

    // The sfx table round-trips the same way, and for the same reason: an
    // install with no file gets one listing every slot with its brief, which is
    // the difference between "you can reassign these" and "you would have to
    // know the slot names to try".
    m_sfx = SfxAssignments{};
    m_sfx.Load(m_sfxPath);
    m_sfx.Save(m_sfxPath);

    m_initialized = true;
    spdlog::info("config: {}", m_generalPath.string());
    spdlog::info("config: {}", m_algorithmPath.string());
    spdlog::info("config: {}", m_surfacePath.string());
    spdlog::info("config: {}", m_sfxPath.string());
}

bool ConfigManager::SaveSurfaces() {
    // A header even when nothing is opened, so the file exists and says how to
    // use it. An empty surfaces file is the ordinary state and should not look
    // like a failed write.
    const auto rows = OpenedSurfaceParams(m_algorithm);
    std::size_t count = 0;
    for (bool open : m_algorithm.surfaces.opened) {
        count += open ? 1 : 0;
    }
    const std::string header = std::format(
        "RagdollSounds_Algorithm_Surfaces.ini - one block per floor you have opened\n"
        ";\n"
        "; {} of {} surfaces have a block here. A surface with no block inherits from its\n"
        "; parent - metal, glass and ice from stone; dirt, gravel, snow, water and body from\n"
        "; soft; a puddle from water and bone from body - and a root with no block takes the\n"
        "; [Surfaces] section of RagdollSounds_Algorithm.ini.\n"
        ";\n"
        "; Delete a block to go back to inheriting. Add one by hand - the section name is\n"
        "; Surface.<name>, all lower case - or press + in the testbench's surface panel,\n"
        "; which opens it holding whatever it was already inheriting.",
        count, SurfaceConfig::kClasses);
    // Written from scratch rather than through SaveFrom, which is the one place
    // this file differs from the other two. SaveFrom copies any line it does not
    // recognise through verbatim - exactly right for a file where every key is
    // always present, and exactly wrong here, because closing a surface makes
    // its keys unrecognised and they would survive every save. A list has to be
    // able to lose an entry.
    return ini::WriteFile(m_surfacePath,
                          ToIniText(&m_algorithm, std::span<const ParamDesc>{rows}, header));
}

void ConfigManager::Load() {
    if (!m_initialized) {
        spdlog::error("config: Load before Initialize; using defaults");
        return;
    }

    GeneralConfig general{};
    AlgorithmConfig algorithm{};
    SfxAssignments sfx{};
    const std::size_t generalKeys = LoadInto(m_generalPath, &general, GeneralParams());
    std::size_t algorithmKeys = LoadInto(m_algorithmPath, &algorithm, AlgorithmFileParams());
    if (!std::filesystem::exists(m_surfacePath)) {
        MigrateSurfaces(m_algorithmPath, algorithm);
    } else {
        ReadOpenedSurfaces(m_surfacePath, algorithm);
        algorithmKeys += LoadInto(m_surfacePath, &algorithm, SurfaceParams());
    }
    // After both, and after the migration: it is what turns thirteen classes'
    // worth of "no block" into thirteen classes' worth of usable numbers, and
    // every read of `surfaces.skins` downstream assumes it has run.
    algorithm.surfaces.Resolve();
    const std::size_t sfxSlots = sfx.Load(m_sfxPath);

    {
        std::lock_guard lock{m_mutex};
        m_general = general;
        m_algorithm = algorithm;
        m_sfx = sfx;
    }

    spdlog::info("config: read {} general keys and {} algorithm keys", generalKeys, algorithmKeys);
    // Zero is the ordinary case on a fresh install and means the bank falls back
    // to the filename convention, so it is worth saying plainly rather than
    // leaving somebody to wonder why their ini appears to do nothing.
    spdlog::info("config: {} slot(s) have an sfx assignment{}", sfxSlots,
                 sfxSlots == 0 ? " - the bank will scan sounds/ by filename instead" : "");
    log::SetLevel(m_general.logLevel);

    // The single most useful line in a user's log, because it says what they
    // changed. Nothing is a valid answer and worth printing too.
    const auto generalDeltas = Deltas(&m_general, GeneralParams());
    auto algorithmDeltas = Deltas(&m_algorithm, AlgorithmFileParams());
    // Only the *opened* surfaces. A closed class is holding its parent's values,
    // so reporting it would turn one edit to stone into four lines about ice and
    // glass having changed - which is true of what they play and false about
    // what the user set.
    {
        const auto opened = OpenedSurfaceParams(m_algorithm);
        const auto surfaceDeltas = Deltas(&m_algorithm, std::span<const ParamDesc>{opened});
        algorithmDeltas.insert(algorithmDeltas.end(), surfaceDeltas.begin(), surfaceDeltas.end());
    }
    if (generalDeltas.empty() && algorithmDeltas.empty()) {
        spdlog::info("config: every value is at its default");
    }
    for (const auto& line : generalDeltas) {
        spdlog::info("config: {}", line);
    }
    for (const auto& line : algorithmDeltas) {
        spdlog::info("config: {}", line);
    }
}

void ConfigManager::Save() {
    if (!m_initialized) {
        spdlog::error("config: Save before Initialize; nothing written");
        return;
    }
    std::lock_guard lock{m_mutex};
    SaveFrom(m_generalPath, &m_general, GeneralParams(), "RagdollSounds.ini - general settings");
    SaveFrom(m_algorithmPath, &m_algorithm, AlgorithmFileParams(),
             "RagdollSounds_Algorithm.ini - the sound engine");
    SaveSurfaces();
    m_sfx.Save(m_sfxPath);
    spdlog::info("config: saved");
}

SfxAssignments ConfigManager::Sfx() const {
    std::lock_guard lock{m_mutex};
    return m_hasSfxOverride ? m_sfxOverride : m_sfx;
}

void ConfigManager::SaveSfx(const SfxAssignments& assignments) {
    if (!m_initialized) {
        spdlog::error("config: SaveSfx before Initialize; nothing written");
        return;
    }
    std::size_t slots = 0;
    {
        std::lock_guard lock{m_mutex};
        m_sfx = assignments;
        slots = m_sfx.AssignedSlots();
    }
    if (m_sfx.Save(m_sfxPath)) {
        spdlog::info("config: wrote {} slot assignment(s) to {}", slots, m_sfxPath.string());
    }
}

void ConfigManager::PushSfxOverride(const SfxAssignments& assignments) {
    std::size_t slots = 0;
    {
        std::lock_guard lock{m_mutex};
        m_sfxOverride = assignments;
        m_hasSfxOverride = true;
        slots = m_sfxOverride.AssignedSlots();
    }
    spdlog::info("config: sfx override pushed, {} slot(s) assigned", slots);
}

void ConfigManager::ClearSfxOverride() {
    {
        std::lock_guard lock{m_mutex};
        m_hasSfxOverride = false;
    }
    spdlog::info("config: sfx override cleared, back to the ini");
}

bool ConfigManager::HasSfxOverride() const {
    std::lock_guard lock{m_mutex};
    return m_hasSfxOverride;
}

AlgorithmConfig ConfigManager::Algorithm() const {
    std::lock_guard lock{m_mutex};
    return m_hasOverride ? m_override : m_algorithm;
}

void ConfigManager::PushOverride(const AlgorithmConfig& config) {
    std::vector<std::string> deltas;
    {
        std::lock_guard lock{m_mutex};
        m_override = config;
        // Belt and braces: the testbench resolves as it edits, but a config
        // arriving over the wire from `tune.py` has been through a schema walk
        // that knows nothing about inheritance, so a closed class could be
        // holding whatever the sender's copy had.
        m_override.surfaces.Resolve();
        m_hasOverride = true;
        deltas = Deltas(&m_override, AlgorithmParams());
    }
    // Logged with its deltas, so a log from a testbench session says what was
    // actually being auditioned rather than only that something was.
    spdlog::info("config: override pushed, {} values differ from default", deltas.size());
    for (const auto& line : deltas) {
        spdlog::debug("config: override {}", line);
    }
}

void ConfigManager::ClearOverride() {
    {
        std::lock_guard lock{m_mutex};
        m_hasOverride = false;
    }
    spdlog::info("config: override cleared, back to the ini");
}

bool ConfigManager::HasOverride() const {
    std::lock_guard lock{m_mutex};
    return m_hasOverride;
}

void ConfigManager::CommitOverrideToIni() {
    {
        std::lock_guard lock{m_mutex};
        if (m_hasOverride) {
            m_algorithm = m_override;
        }
    }
    Save();
}

}  // namespace rds
