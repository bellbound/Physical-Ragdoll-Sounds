#include "VanillaLibrary.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>

namespace tb {
namespace {

namespace fs = std::filesystem;

[[nodiscard]] bool IsUpper(char c) { return c >= 'A' && c <= 'Z'; }
[[nodiscard]] bool IsLower(char c) { return c >= 'a' && c <= 'z'; }

[[nodiscard]] std::string Lower(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

/// The extensions the game ships sound in. `xwm` is here because a body impact
/// could in principle be one; miniaudio will not decode it, and a file that
/// cannot be decoded reads as an empty buffer further down, which is the same
/// as a missing one - so indexing it costs nothing and leaving it out would hide
/// the reason a descriptor resolved to a file and still made no sound.
[[nodiscard]] bool IsAudio(const fs::path& path) {
    const std::string ext = Lower(path.extension().string());
    return ext == ".wav" || ext == ".xwm" || ext == ".ogg" || ext == ".mp3" || ext == ".flac";
}

/// `phy_body_medium_dirt_l_01` -> `phy_body_medium_dirt_l`.
///
/// Only a trailing run of digits behind an underscore. Not any trailing digits:
/// a descriptor could legitimately end in one, and cutting `..._l2` down to
/// `..._l` would merge two groups that are different sounds.
[[nodiscard]] std::string GroupKey(std::string_view stem) {
    std::size_t end = stem.size();
    while (end > 0 && stem[end - 1] >= '0' && stem[end - 1] <= '9') --end;
    if (end == stem.size() || end == 0 || stem[end - 1] != '_') {
        return std::string{stem};
    }
    return std::string{stem.substr(0, end - 1)};
}

const std::vector<std::string> kNoFiles;

}  // namespace

std::string SnakeFromEditorId(std::string_view editorId) {
    std::string out;
    out.reserve(editorId.size() + 8);
    for (std::size_t i = 0; i < editorId.size(); ++i) {
        const char c = editorId[i];
        if (i > 0 && IsUpper(c)) {
            const char prev = editorId[i - 1];
            const bool afterLower = IsLower(prev) || (prev >= '0' && prev <= '9');
            // The last upper of a run, when a lower follows: `PHYBody` breaks
            // before the B, not before the H, so the acronym survives.
            const bool endOfRun =
                IsUpper(prev) && i + 1 < editorId.size() && IsLower(editorId[i + 1]);
            if (afterLower || endOfRun) {
                out += '_';
            }
        }
        out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

void VanillaLibrary::SetRoot(const fs::path& root) {
    if (root == m_root) {
        return;
    }
    m_root = root;
    m_groups.clear();
    m_misses.clear();
    m_files = 0;

    std::error_code ec;
    if (m_root.empty() || !fs::is_directory(m_root, ec)) {
        if (!m_root.empty()) {
            spdlog::warn("vanilla library: {} is not a directory - the vanilla side will be silent",
                         m_root.string());
        }
        return;
    }

    for (fs::recursive_directory_iterator it{m_root, fs::directory_options::skip_permission_denied,
                                             ec};
         it != fs::recursive_directory_iterator{}; it.increment(ec)) {
        if (ec) {
            break;
        }
        if (!it->is_regular_file(ec) || !IsAudio(it->path())) {
            continue;
        }
        const std::string key = GroupKey(Lower(it->path().stem().string()));
        m_groups[key].push_back(it->path().string());
        ++m_files;
    }

    // Sorted so the variant a seed picks is the same file on two machines. An
    // unsorted directory walk is stable per filesystem and not across them, which
    // is exactly the kind of difference that makes an A/B irreproducible.
    for (auto& [key, paths] : m_groups) {
        std::ranges::sort(paths);
    }

    spdlog::info("vanilla library: {} file(s) in {} group(s) under {}", m_files, m_groups.size(),
                 m_root.string());
}

const std::vector<std::string>& VanillaLibrary::Files(const std::string& editorId) const {
    const std::string key = SnakeFromEditorId(editorId);
    const auto it = m_groups.find(key);
    if (it != m_groups.end()) {
        return it->second;
    }
    // Counted rather than logged per row: one fall is dozens of contacts on the
    // same descriptor, and forty identical warnings is a log nobody reads.
    if (m_misses[editorId]++ == 0) {
        spdlog::warn("vanilla library: nothing under {} matches '{}' (looked for '{}_NN')",
                     m_root.empty() ? std::string{"<no root set>"} : m_root.string(), editorId, key);
    }
    return kNoFiles;
}

std::vector<std::string> VanillaLibrary::Misses() const {
    std::vector<std::string> out;
    out.reserve(m_misses.size());
    for (const auto& [name, count] : m_misses) {
        out.push_back(name);
    }
    return out;
}

void VanillaLibrary::ClearMisses() { m_misses.clear(); }

}  // namespace tb
