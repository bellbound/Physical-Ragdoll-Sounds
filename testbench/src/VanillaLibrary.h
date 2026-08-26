#pragma once

// Finding the wav behind a vanilla sound descriptor.
//
// A vanilla track row names the descriptor that fired - `PHYBodyMediumDirtL` -
// and nothing else. It cannot name the file: `BGSStandardSoundDef::soundFiles`
// holds `BSResource::ID`, which is a hash of the path with no way back, so the
// game half has a name and never has a path. The path has to be found here.
//
// **By convention, and the convention holds.** Bethesda names the files after
// the descriptor: `PHYBodyMediumDirtL` is
// `sound/fx/phy/body/medium/dirt/l/phy_body_medium_dirt_l_01.wav` and its two
// siblings. Splitting the editor id on its camel humps gives
// `phy_body_medium_dirt_l`, which is the filename with the variant number cut
// off - checked against every path in `assets/sfx/skyrim/manifest.csv`, which
// carries the originals for the files already lifted for reference.
//
// So: index the root once by *group* - the filename with a trailing `_NN`
// removed - and look a descriptor up by its snake-cased name. Directory layout
// is irrelevant, because the index is a recursive walk; an extract that is one
// flat folder works as well as one that mirrors the game's tree.
//
// **Nothing here ships, and nothing here can.** These are Bethesda's files. The
// root is a dev-only path the testbench is pointed at, it is never read by the
// game DLL, and `deploy-pack.ps1` cannot reach it - the same three mechanisms
// that keep `assets/sfx/skyrim/` out of a release, set out in that folder's
// README. Keep it that way.
//
// A miss is reported once per descriptor and then stays quiet. The failure it
// guards against is a session spent wondering why the vanilla side is silent
// when the answer is that the root was never set.

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace tb {

/// `PHYBodyMediumDirtL` -> `phy_body_medium_dirt_l`.
///
/// Splits on a hump - an upper-case letter after a lower-case one, or the last
/// of an upper-case run that is followed by a lower-case one, which is what keeps
/// the `PHY` prefix together instead of spelling it `p_h_y`.
[[nodiscard]] std::string SnakeFromEditorId(std::string_view editorId);

class VanillaLibrary {
public:
    /// Point it at an extract of the game's `sound/fx` tree. Re-indexes only when
    /// the path actually changes, because the walk is a few thousand files and
    /// the checkbox that drives this is toggled a lot.
    void SetRoot(const std::filesystem::path& root);

    [[nodiscard]] const std::filesystem::path& Root() const { return m_root; }

    /// Files indexed, and groups they fall into. Both shown in the UI: "0 files"
    /// and "files but no group matched" are different problems.
    [[nodiscard]] std::size_t FileCount() const { return m_files; }
    [[nodiscard]] std::size_t GroupCount() const { return m_groups.size(); }

    /// Every wav for a descriptor, sorted by name so variant order is stable
    /// across machines. Empty when nothing matched.
    [[nodiscard]] const std::vector<std::string>& Files(const std::string& editorId) const;

    /// Which descriptors were asked for and not found, for the UI to show. Sorted
    /// and deduped, so a take with forty misses of one name reads as one line.
    [[nodiscard]] std::vector<std::string> Misses() const;

    void ClearMisses();

private:
    std::filesystem::path m_root;
    std::size_t m_files{};
    /// group key -> paths. The group key is the snake-cased descriptor name.
    std::map<std::string, std::vector<std::string>> m_groups;
    mutable std::map<std::string, int> m_misses;
};

}  // namespace tb
