#include "rds/ConfigManager.h"

#include <spdlog/spdlog.h>

#include <algorithm>
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
    }
    return nullptr;
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

/// The tooltip, word-wrapped into `; ` comment lines. This is what makes a fresh
/// install ship a file somebody can read rather than ninety numbers.
///
/// Paragraphs first, then wrapping inside each. A tooltip is also rendered by the
/// testbench's slider panel, where a `\n` is just a line break and costs nothing -
/// so the moment one is written the ini writer has to agree, or it emits the rest
/// of the paragraph with no `; ` in front of it and the file stops being an ini.
/// Wrapping the whole string as one run is what did that.
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
        for (const auto& raw : existing) {
            if (const auto sectionName = SectionOf(raw); !sectionName.empty()) {
                section.assign(sectionName);
                flush();
                out += raw;
                out += "\n";
                continue;
            }
            std::string_view key;
            std::string_view value;
            if (SplitAssignment(raw, key, value)) {
                if (const ParamDesc* desc = Find(params, section, key); desc != nullptr) {
                    const auto index = static_cast<std::size_t>(desc - params.data());
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

void ConfigManager::Initialize(const std::filesystem::path& directory) {
    m_directory = directory;
    m_generalPath = directory / "RagdollSounds.ini";
    m_algorithmPath = directory / "RagdollSounds_Algorithm.ini";
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
    LoadInto(m_algorithmPath, &m_algorithm, AlgorithmParams());
    SaveFrom(m_generalPath, &m_general, GeneralParams(), "RagdollSounds.ini - general settings");
    SaveFrom(m_algorithmPath, &m_algorithm, AlgorithmParams(),
             "RagdollSounds_Algorithm.ini - the sound engine");

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
    spdlog::info("config: {}", m_sfxPath.string());
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
    const std::size_t algorithmKeys = LoadInto(m_algorithmPath, &algorithm, AlgorithmParams());
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
    const auto algorithmDeltas = Deltas(&m_algorithm, AlgorithmParams());
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
    SaveFrom(m_algorithmPath, &m_algorithm, AlgorithmParams(),
             "RagdollSounds_Algorithm.ini - the sound engine");
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
