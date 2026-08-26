#include "rds/VanillaTrack.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <charconv>
#include <cstring>
#include <format>
#include <fstream>
#include <string_view>

namespace rds::vanilla {
namespace {

constexpr std::string_view kHeader =
    "seq,t_ms,descriptor,descriptor_id,impact_id,branch,heard,sound_level,static_atten_db,"
    "db_variance,freq_shift,freq_variance,priority,files,pos_x,pos_y,pos_z";

/// The preamble. Long, and deliberately: this file is read by a person far more
/// often than by a program, and the thing it must not do is let somebody believe
/// it says which wav played.
constexpr std::string_view kPreamble =
    "# What vanilla's own impact system played during this take.\n"
    "#\n"
    "# One row per collision the ragdoll path resolved to a sound, observed at the\n"
    "# moment of the play rather than modelled afterwards. Times share this take's\n"
    "# clock: the same t_ms the impacts CSV stamps.\n"
    "#\n"
    "# descriptor      the SNDR that fired - the name its wav files are found under\n"
    "# branch          light = SNAM, heavy = NAM1. Vanilla's own loudness decision,\n"
    "#                 which it makes on (NAM1 exists && magnitude >= threshold)\n"
    "# heard           1 = it sounded, 0 = we dropped it and played ours instead\n"
    "# static_atten_db the descriptor's fixed attenuation, in dB\n"
    "# db_variance     +/- dB the audio engine rolls per play. NOT the roll itself\n"
    "# files           how many wavs the descriptor picks between, uniformly\n"
    "#\n"
    "# What is deliberately absent: which of `files` was drawn, and what\n"
    "# db_variance and freq_variance actually rolled. Both happen inside\n"
    "# BSAudioManager after the sound handle is built and are not readable from\n"
    "# outside it. A row says \"one of N files, at this level, plus or minus this\n"
    "# much\" - it does not say which one, and anything reading it must not pretend\n"
    "# otherwise.\n";

[[nodiscard]] std::string_view Trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r'))
        text.remove_suffix(1);
    return text;
}

[[nodiscard]] double ToDouble(std::string_view text, double fallback = 0.0) {
    text = Trim(text);
    if (text.empty() || text == "-") return fallback;
    double value{};
    if (std::from_chars(text.data(), text.data() + text.size(), value).ec != std::errc{}) {
        return fallback;
    }
    return value;
}

[[nodiscard]] std::uint32_t ToHex(std::string_view text) {
    text = Trim(text);
    if (text.starts_with("0x") || text.starts_with("0X")) text.remove_prefix(2);
    std::uint32_t value{};
    if (std::from_chars(text.data(), text.data() + text.size(), value, 16).ec != std::errc{}) {
        return 0;
    }
    return value;
}

[[nodiscard]] std::uint32_t ToUint(std::string_view text) {
    text = Trim(text);
    if (text.empty() || text == "-") return 0;
    std::uint32_t value{};
    if (std::from_chars(text.data(), text.data() + text.size(), value).ec != std::errc{}) {
        return 0;
    }
    return value;
}

/// Split on commas. The descriptor column is an editor id and cannot contain
/// one, so nothing here needs to understand quoting - and a parser that did
/// would be pretending the format is richer than it is.
void Split(std::string_view line, std::vector<std::string_view>& out) {
    out.clear();
    std::size_t start = 0;
    while (true) {
        const std::size_t comma = line.find(',', start);
        if (comma == std::string_view::npos) {
            out.push_back(line.substr(start));
            return;
        }
        out.push_back(line.substr(start, comma - start));
        start = comma + 1;
    }
}

enum Column : std::size_t {
    kSeq = 0,
    kTimeMs,
    kDescriptor,
    kDescriptorId,
    kImpactId,
    kBranch,
    kHeard,
    kSoundLevel,
    kAttenDb,
    kDbVariance,
    kFreqShift,
    kFreqVariance,
    kPriority,
    kFiles,
    kPosX,
    kPosY,
    kPosZ,
    kColumnCount
};

}  // namespace

std::string_view BranchName(const VanillaSoundInfo& info) {
    return info.Has(VanillaSoundFlag::kHeavy) ? "heavy" : "light";
}

float AttenuationDb(const VanillaSoundInfo& info) {
    return static_cast<float>(info.staticAttenuation) / 100.0f;
}

bool Write(const std::filesystem::path& file, const std::vector<FeedEvent>& events, double originMs,
           double loMs, double hiMs, std::size_t& rowsOut, std::string& error) {
    rowsOut = 0;

    std::vector<const FeedEvent*> kept;
    for (const FeedEvent& event : events) {
        if (event.kind != EventKind::kVanillaSound) continue;
        if (event.timeMs < loMs || event.timeMs > hiMs) continue;
        kept.push_back(&event);
    }
    if (kept.empty()) {
        return true;  // no track is not a failure; see the header
    }

    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::trunc | std::ios::binary);
    if (!out) {
        error = "cannot write " + file.string();
        return false;
    }

    out << kPreamble << kHeader << '\n';

    std::uint32_t seq = 1;
    for (const FeedEvent* event : kept) {
        const VanillaSoundInfo& v = event->vanilla;
        const bool heard = !v.Has(VanillaSoundFlag::kSuppressed);
        out << seq++ << ',' << std::format("{:.3f}", event->timeMs - originMs) << ','
            << (event->text[0] != '\0' ? event->text : "-") << ','
            << std::format("{:08X}", v.descriptorFormId) << ','
            << std::format("{:08X}", v.impactFormId) << ',' << BranchName(v) << ','
            << (heard ? 1 : 0) << ',' << static_cast<int>(v.soundLevel) << ','
            << std::format("{:.2f}", AttenuationDb(v)) << ',' << static_cast<int>(v.dbVariance)
            << ',' << static_cast<int>(v.frequencyShift) << ','
            << static_cast<int>(v.frequencyVariance) << ',' << static_cast<int>(v.priority) << ','
            << static_cast<int>(v.fileCount) << ',' << std::format("{:.3f}", event->position.x)
            << ',' << std::format("{:.3f}", event->position.y) << ','
            << std::format("{:.3f}", event->position.z) << '\n';
    }

    rowsOut = kept.size();
    return true;
}

bool Read(const std::filesystem::path& file, std::vector<FeedEvent>& out, std::string& error) {
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) {
        return true;  // a take with no vanilla track, which is most of them
    }

    std::ifstream in(file, std::ios::binary);
    if (!in) {
        error = "cannot read " + file.string();
        return false;
    }

    std::string line;
    bool sawHeader = false;
    std::vector<std::string_view> columns;
    std::size_t lineNumber = 0;

    while (std::getline(in, line)) {
        ++lineNumber;
        const std::string_view text = Trim(line);
        if (text.empty() || text.front() == '#') continue;
        if (!sawHeader) {
            // The first non-comment line is the column header. Checked rather
            // than skipped: a file whose columns moved would otherwise be read
            // as data with everything one place out, which is worse than an
            // error because it looks like a working take that sounds wrong.
            if (!text.starts_with("seq,t_ms,descriptor")) {
                error = std::format("{}:{}: not a vanilla track - the header line reads '{}'",
                                    file.filename().string(), lineNumber, text.substr(0, 40));
                return false;
            }
            sawHeader = true;
            continue;
        }

        Split(text, columns);
        if (columns.size() < kColumnCount) {
            error = std::format("{}:{}: {} columns, expected {}", file.filename().string(),
                                lineNumber, columns.size(), static_cast<int>(kColumnCount));
            return false;
        }

        FeedEvent event{};
        event.kind = EventKind::kVanillaSound;
        event.timeMs = ToDouble(columns[kTimeMs]);
        event.sourceSeq = ToUint(columns[kSeq]);
        event.position = Vec3{static_cast<float>(ToDouble(columns[kPosX])),
                              static_cast<float>(ToDouble(columns[kPosY])),
                              static_cast<float>(ToDouble(columns[kPosZ]))};

        const std::string_view name = Trim(columns[kDescriptor]);
        if (name != "-") {
            std::snprintf(event.text, sizeof(event.text), "%.*s",
                          static_cast<int>(std::min<std::size_t>(name.size(), sizeof(event.text) - 1)),
                          name.data());
        }

        VanillaSoundInfo& v = event.vanilla;
        v.descriptorFormId = ToHex(columns[kDescriptorId]);
        v.impactFormId = ToHex(columns[kImpactId]);
        v.soundLevel = static_cast<std::uint8_t>(ToUint(columns[kSoundLevel]));
        v.staticAttenuation =
            static_cast<std::uint16_t>(ToDouble(columns[kAttenDb]) * 100.0 + 0.5);
        v.dbVariance = static_cast<std::uint8_t>(ToUint(columns[kDbVariance]));
        v.frequencyShift = static_cast<std::uint8_t>(ToUint(columns[kFreqShift]));
        v.frequencyVariance = static_cast<std::uint8_t>(ToUint(columns[kFreqVariance]));
        v.priority = static_cast<std::uint8_t>(ToUint(columns[kPriority]));
        v.fileCount = static_cast<std::uint8_t>(ToUint(columns[kFiles]));
        if (Trim(columns[kBranch]) == "heavy") {
            v.flags |= static_cast<std::uint8_t>(VanillaSoundFlag::kHeavy);
        }
        if (ToUint(columns[kHeard]) == 0) {
            v.flags |= static_cast<std::uint8_t>(VanillaSoundFlag::kSuppressed);
        }

        out.push_back(event);
    }

    return true;
}

bool Probe(const std::filesystem::path& file, std::size_t& rowsOut) {
    rowsOut = 0;
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) {
        return false;
    }
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return false;
    }
    std::string line;
    bool sawHeader = false;
    while (std::getline(in, line)) {
        const std::string_view text = Trim(line);
        if (text.empty() || text.front() == '#') continue;
        if (!sawHeader) {
            sawHeader = true;
            continue;
        }
        ++rowsOut;
    }
    return sawHeader;
}

}  // namespace rds::vanilla
