#pragma once

// The four line-level primitives every ini in this mod is read with.
//
// Promoted out of ConfigManager.cpp when the sound-assignment file and the
// per-sfx metadata files arrived and wanted the same parser. There is no third
// copy to drift: a comment that survives a round trip in RagdollSounds.ini has
// to survive one in RagdollSounds_SFX.ini too, and it does because the same
// four functions decide what a comment is.
//
// Deliberately not a document model. ConfigManager rewrites values in place and
// copies every other line through verbatim, which needs the lines and not a
// parse tree; the sfx files are small enough that a linear walk is the whole
// reader. Anything that wanted a tree would want a different file format.

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace rds::ini {

/// Spaces, tabs and a trailing CR off both ends. CR because these files are
/// written on Windows and read by a testbench opened in binary mode.
[[nodiscard]] std::string_view Trim(std::string_view text);

/// Case-insensitive over ASCII, which is what section and key names are.
[[nodiscard]] bool EqualsIgnoreCase(std::string_view a, std::string_view b);

/// A `[Section]` line's name, or empty when the line is not one.
[[nodiscard]] std::string_view SectionOf(std::string_view line);

/// Splits `key = value` on the first '='. False for comments, blanks and
/// section headers, so a caller can `if (!SplitAssignment(...)) continue;`.
[[nodiscard]] bool SplitAssignment(std::string_view line, std::string_view& key,
                                   std::string_view& value);

/// Every line of `file`, newline-stripped. Empty when the file is missing,
/// which every caller here treats the same way as an empty file.
[[nodiscard]] std::vector<std::string> ReadLines(const std::filesystem::path& file);

/// Write `text` to `file`, creating the directory. Logs at error and returns
/// false rather than throwing - a config we cannot write is a session that
/// loses its edits, not a crash.
bool WriteFile(const std::filesystem::path& file, std::string_view text);

/// `a, b, c` to three trimmed pieces, dropping empties. The list separator for
/// every multi-valued key in these files.
[[nodiscard]] std::vector<std::string> SplitList(std::string_view value);

/// The inverse, so a list written out reads the way one typed by hand does.
[[nodiscard]] std::string JoinList(const std::vector<std::string>& items);

}  // namespace rds::ini
