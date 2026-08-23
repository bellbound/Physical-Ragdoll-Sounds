#include "rds/Ini.h"

#include <spdlog/spdlog.h>

#include <fstream>

namespace rds::ini {

std::string_view Trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() &&
           (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    return text;
}

bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    const auto lower = [](char c) {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
    };
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (lower(a[i]) != lower(b[i])) {
            return false;
        }
    }
    return true;
}

std::string_view SectionOf(std::string_view line) {
    line = Trim(line);
    if (line.size() < 2 || line.front() != '[' || line.back() != ']') {
        return {};
    }
    return Trim(line.substr(1, line.size() - 2));
}

bool SplitAssignment(std::string_view line, std::string_view& key, std::string_view& value) {
    const auto trimmed = Trim(line);
    if (trimmed.empty() || trimmed.front() == ';' || trimmed.front() == '#' ||
        trimmed.front() == '[') {
        return false;
    }
    const auto eq = trimmed.find('=');
    if (eq == std::string_view::npos) {
        return false;
    }
    key = Trim(trimmed.substr(0, eq));
    value = Trim(trimmed.substr(eq + 1));
    return !key.empty();
}

std::vector<std::string> ReadLines(const std::filesystem::path& file) {
    std::vector<std::string> lines;
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return lines;
    }
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(std::move(line));
    }
    return lines;
}

bool WriteFile(const std::filesystem::path& file, std::string_view text) {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    if (!stream) {
        spdlog::error("ini: cannot write {} - the change will be lost", file.string());
        return false;
    }
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!stream.good()) {
        spdlog::error("ini: writing {} failed part way through", file.string());
        return false;
    }
    return true;
}

std::vector<std::string> SplitList(std::string_view value) {
    std::vector<std::string> out;
    std::size_t cursor = 0;
    while (cursor <= value.size()) {
        const auto comma = value.find(',', cursor);
        const auto piece = Trim(value.substr(
            cursor, comma == std::string_view::npos ? std::string_view::npos : comma - cursor));
        if (!piece.empty()) {
            out.emplace_back(piece);
        }
        if (comma == std::string_view::npos) {
            break;
        }
        cursor = comma + 1;
    }
    return out;
}

std::string JoinList(const std::vector<std::string>& items) {
    std::string out;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i != 0) {
            out += ", ";
        }
        out += items[i];
    }
    return out;
}

}  // namespace rds::ini
