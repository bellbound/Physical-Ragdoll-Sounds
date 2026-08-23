#pragma once

// Owns the two config objects, the two ini files, and the testbench override.
//
// The override is the whole of the "let the testbench push config" requirement:
// there is one AlgorithmConfig type, the ini fills one instance of it, and the
// testbench hands over another. Nothing downstream knows which it is holding,
// so there is no second code path to keep in step - which is the same trick
// Feed.h plays for the input side.
//
// Path, matching the other mods here:
//   Data\SKSE\Plugins\RagdollSounds\RagdollSounds.ini
//   Data\SKSE\Plugins\RagdollSounds\RagdollSounds_Algorithm.ini
//   Data\SKSE\Plugins\RagdollSounds\RagdollSounds_SFX.ini
// The testbench points it at its own directory instead.
//
// The third file is not a schema walk like the other two: it is a list of
// filenames per slot rather than a table of numbers, so it carries its own
// reader in Sfx.h. It is here anyway because it is config - it is loaded at the
// same moment, saved by the same button, and pushed by the same seam.

#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <string>

#include "rds/Config.h"
#include "rds/ConfigSchema.h"
#include "rds/Sfx.h"

namespace rds {

class ConfigManager {
public:
    static ConfigManager& Get();

    /// `directory` holds both ini files. Creates it, and writes each file with
    /// every key at its default plus the tooltip as a comment if it is absent -
    /// so a fresh install ships a self-documenting, complete ini rather than an
    /// empty one somebody has to guess at.
    void Initialize(const std::filesystem::path& directory);

    [[nodiscard]] bool IsInitialized() const { return m_initialized; }

    /// Read both files. Unknown keys are left alone and logged once at debug -
    /// a key we removed should not cost a user their whole file. Out-of-range
    /// values are clamped and logged at warn with both numbers.
    void Load();

    /// Write both files, preserving the comments. Only called on an explicit
    /// save; nothing in the engine writes config behind the user's back.
    void Save();

    [[nodiscard]] const GeneralConfig& General() const { return m_general; }

    /// Which sfx each slot plays. Read from RagdollSounds_SFX.ini, or the
    /// override if the testbench pushed one.
    ///
    /// By value for the same reason Algorithm() is: the caller may be reloading
    /// the bank while the testbench swaps the table under it.
    [[nodiscard]] SfxAssignments Sfx() const;

    /// The ini's own copy, ignoring any override. What SaveSfx() writes.
    [[nodiscard]] const SfxAssignments& SfxFromIni() const { return m_sfx; }

    /// Replace the ini's table and write it. The testbench's Save for the sfx
    /// panel, and the only thing that writes this file.
    void SaveSfx(const SfxAssignments& assignments);

    /// Where the library lives: `<config directory>/sounds/library` in the
    /// game, and wherever `--sounds` points beside the testbench.
    [[nodiscard]] const std::filesystem::path& SfxPath() const { return m_sfxPath; }

    /// The algorithm config the engine should use right now: the override if one
    /// is pushed, otherwise the ini's.
    ///
    /// Returned by value, cheap and trivially copyable, because the caller may
    /// be an audio thread and a reference into a struct somebody is about to
    /// swap is how that thread reads half of one config and half of another.
    [[nodiscard]] AlgorithmConfig Algorithm() const;

    /// The ini's own copy, ignoring any override. What Save() writes.
    [[nodiscard]] const AlgorithmConfig& AlgorithmFromIni() const { return m_algorithm; }

    // ── the testbench seam ───────────────────────────────────────────────────

    /// Use this instead of the ini's until ClearOverride(). Logged at info with
    /// the deltas against the ini, so a log from a testbench session says what
    /// was actually being auditioned.
    void PushOverride(const AlgorithmConfig& config);

    void ClearOverride();

    [[nodiscard]] bool HasOverride() const;

    /// Copy the override - or the ini's config if there is none - into the ini's
    /// and write it. This is the testbench's "keep this one" button.
    void CommitOverrideToIni();

    /// Same seam for the sfx table: the testbench auditions an assignment
    /// without writing it, and the engine reloads the bank from whatever
    /// Sfx() hands back.
    void PushSfxOverride(const SfxAssignments& assignments);
    void ClearSfxOverride();
    [[nodiscard]] bool HasSfxOverride() const;

    // ── free functions over the schema, for anything not holding a manager ───

    /// Read one ini file into `root` using `params`. Returns how many keys were
    /// found; 0 means the file was missing or empty.
    static std::size_t LoadInto(const std::filesystem::path& file, void* root,
                                std::span<const ParamDesc> params);

    /// Write `root` out through `params`, one section per group, each key
    /// preceded by its tooltip as a comment.
    static bool SaveFrom(const std::filesystem::path& file, const void* root,
                         std::span<const ParamDesc> params, std::string_view header);

private:
    ConfigManager() = default;

    bool m_initialized{};
    std::filesystem::path m_directory;
    std::filesystem::path m_generalPath;
    std::filesystem::path m_algorithmPath;
    std::filesystem::path m_sfxPath;

    GeneralConfig m_general{};
    AlgorithmConfig m_algorithm{};
    SfxAssignments m_sfx{};

    mutable std::mutex m_mutex;
    bool m_hasOverride{};
    AlgorithmConfig m_override{};
    bool m_hasSfxOverride{};
    SfxAssignments m_sfxOverride{};
};

}  // namespace rds
