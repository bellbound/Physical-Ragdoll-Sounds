#include "Capture.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <fstream>
#include <sstream>

#include "rds/Pose.h"
#include "rds/VanillaTrack.h"

#include "Video.h"

namespace fs = std::filesystem;

namespace tb {
namespace {

/// The recorder's own column list, in the recorder's own order.
///
/// Order does not matter to the loader - it resolves columns by name, precisely
/// because this schema has moved once already - but it matters to whoever opens
/// one of these in a spreadsheet next to a take the game wrote.
constexpr const char* kCsvHeader =
    "seq,t_ms,game_hour,event,phase,actor,actor_id,limb,limb_index,impact_speed,normal_speed,"
    "tangent_speed,body_speed,angular_speed,mass,limb_radius,pos_x,pos_y,pos_z,nrm_x,nrm_y,nrm_z,"
    "vel_x,vel_y,vel_z,other_layer,other_material,material_source,other_body,other_limb,"
    "manifold_first,manifold_last,dropped,state";

[[nodiscard]] std::string_view Trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r'))
        text.remove_suffix(1);
    return text;
}

[[nodiscard]] double ToDouble(std::string_view text, double fallback = 0.0) {
    text = Trim(text);
    double value{};
    if (text.empty() ||
        std::from_chars(text.data(), text.data() + text.size(), value).ec != std::errc{}) {
        return fallback;
    }
    return value;
}

/// The `t_ms` column of a raw CSV line, without splitting the whole row. The
/// delete path reads this for every line of a file that can be a hundred
/// thousand rows long, and a full split there is most of the work.
[[nodiscard]] bool TimeOfRow(std::string_view line, std::size_t column, double& out) {
    std::size_t start = 0;
    std::size_t index = 0;
    for (std::size_t i = 0; i <= line.size(); ++i) {
        if (i != line.size() && line[i] != ',') continue;
        if (index == column) {
            out = ToDouble(line.substr(start, i - start), -1.0);
            return out >= 0.0;
        }
        ++index;
        start = i + 1;
    }
    return false;
}

/// Which body site a limb's coverage should be written under, matching
/// Recording.cpp's CoverageSiteName - the map it reads on the way back in.
[[nodiscard]] std::string_view CoverageSite(rds::LimbSite site) {
    switch (site) {
        case rds::LimbSite::kHead:
        case rds::LimbSite::kNeck:
            return "head";
        case rds::LimbSite::kTorso:
            return "torso";
        case rds::LimbSite::kHand:
            return "hands";
        case rds::LimbSite::kForearm:
        case rds::LimbSite::kUpperArm:
            return "forearms";
        case rds::LimbSite::kFoot:
            return "feet";
        case rds::LimbSite::kCalf:
        case rds::LimbSite::kThigh:
            return "calves";
        case rds::LimbSite::kUnknown:
        case rds::LimbSite::kCount:
            break;
    }
    return "torso";
}

void WriteRow(std::ostream& out, const rds::FeedEvent& event, std::uint32_t seq, double timeMs,
              const rds::ActorProfile* profile, std::string_view actorName) {
    const rds::LimbInfo* limb =
        profile != nullptr ? profile->Limb(event.limbIndex) : nullptr;
    // A state row has no limb. Anything else does, whether or not a profile ever
    // turned up to give it a name: limbIndex is the game's own index and it is
    // on the event either way. The name and the number used to fall together,
    // which threw away the one of the two that was actually in hand.
    const bool isLimbEvent = event.kind != rds::EventKind::kState;
    const bool hasLimb = limb != nullptr && isLimbEvent;

    out << seq << ',' << std::format("{:.3f}", timeMs) << ",0.0000," << rds::ToString(event.kind)
        << ',' << rds::PhaseName(event.phase) << ',' << actorName << ','
        << std::format("{:08X}", static_cast<std::uint32_t>(event.actorId)) << ','
        << (hasLimb ? limb->boneName : std::string("-")) << ',';
    if (isLimbEvent) {
        out << event.limbIndex;
    } else {
        out << '-';
    }
    out << ',' << std::format("{:.3f}", event.impactSpeed) << ','
        << std::format("{:.3f}", event.normalSpeed) << ','
        << std::format("{:.3f}", event.tangentSpeed) << ','
        << std::format("{:.3f}", event.bodySpeed) << ','
        << std::format("{:.3f}", event.angularSpeed) << ','
        << std::format("{:.4f}", event.mass) << ',' << std::format("{:.3f}", event.limbRadius)
        << ',' << std::format("{:.3f}", event.position.x) << ','
        << std::format("{:.3f}", event.position.y) << ','
        << std::format("{:.3f}", event.position.z) << ','
        << std::format("{:.4f}", event.normal.x) << ',' << std::format("{:.4f}", event.normal.y)
        << ',' << std::format("{:.4f}", event.normal.z) << ','
        << std::format("{:.3f}", event.velocity.x) << ','
        << std::format("{:.3f}", event.velocity.y) << ','
        << std::format("{:.3f}", event.velocity.z) << ',' << rds::LayerName(event.otherLayer)
        << ',' << rds::MaterialName(event.otherMaterial) << ','
        << rds::SourceName(event.materialSource) << ','
        << std::format("{:016X}", event.otherBody) << ',';
    if (event.otherLimb >= 0) {
        out << event.otherLimb;
    } else {
        out << '-';
    }
    out << ',' << (event.manifoldFirst ? 1 : 0) << ',' << (event.manifoldLast ? 1 : 0) << ",0,"
        << (event.text[0] != '\0' ? event.text : "-") << '\n';
}

void WriteSidecar(const fs::path& yaml, const std::string& stem, const TakeSource& source,
                  double durationMs, std::uint32_t impacts, std::size_t poseFrames,
                  std::size_t vanillaRows) {
    std::ofstream out(yaml, std::ios::trunc);
    if (!out) return;

    const auto now = std::chrono::system_clock::now();
    const auto local = std::chrono::current_zone()->to_local(now);

    out << "# Physical Ragdoll Sounds testbench - ragdoll impact recording metadata\n";
    out << "# Written by the devbench, in the same shape QuickModMenuNG writes, so a take made\n";
    out << "# here and a take made in game are the same kind of file.\n\n";

    out << "recording:\n";
    out << "  file_index: 1\n";
    out << "  csv: \"" << stem << ".csv\"\n";
    // Only when there is one. An absent key and a key naming a file that is not
    // there are different things, and the second is the one that wastes an hour.
    if (poseFrames > 0) {
        out << "  pose: \"" << stem << "_pose.bin\"\n";
        out << "  pose_frames: " << poseFrames << "\n";
    }
    if (vanillaRows > 0) {
        out << "  vanilla: \"" << stem << "_vanilla.csv\"\n";
        out << "  vanilla_rows: " << vanillaRows << "\n";
    }
    out << "  note: \"" << source.note << "\"\n";
    out << "  started_real: \"" << std::format("{:%Y-%m-%dT%H:%M:%S}",
                                               std::chrono::floor<std::chrono::seconds>(local))
        << "\"\n";
    out << "  min_impact_speed: 0.0\n";
    out << "  units: \"positions are game units; speeds are game units/second; angular speed is "
           "rad/s\"\n\n";

    out << "actor:\n";
    out << "  name: \"" << source.actorName << "\"\n";
    out << "  form: { id: \"" << (source.formId.empty() ? "00000000" : source.formId)
        << "\", editor_id: \"\", plugin: \"\" }\n";
    out << "  is_player: " << (source.profile != nullptr && source.profile->isPlayer ? "true"
                                                                                    : "false")
        << "\n";
    out << "  scale: " << std::format("{:.4f}", source.profile != nullptr ? source.profile->scale
                                                                          : 1.0f)
        << "\n\n";

    out << "ragdoll:\n";
    const std::size_t limbCount = source.profile != nullptr ? source.profile->limbs.size() : 0;
    out << "  limb_count: " << limbCount << "\n";
    out << "  limbs:\n";
    for (std::size_t i = 0; i < limbCount; ++i) {
        const rds::LimbInfo& limb = source.profile->limbs[i];
        out << "    - limb_index: " << i << "\n";
        out << "      name: \"" << limb.boneName << "\"\n";
        out << "      bone: \"Ragdoll_" << limb.boneName << "\"\n";
        out << "      bone_index: " << i << "\n";
        out << "      mass: " << std::format("{:.4f}", limb.havokMass) << "\n";
        out << "      body: \"" << std::format("{:016X}", limb.bodyId) << "\"\n";
    }
    out << "\n";

    // Only the coverage map, and only because Recording.cpp reads it. The slot
    // list beside it in a game-written sidecar is provenance the engine never
    // consults, and inventing one here would be inventing evidence.
    out << "armour:\n";
    out << "  coverage:\n";
    for (const std::string_view site :
         {"head", "torso", "hands", "forearms", "feet", "calves"}) {
        rds::Coverage coverage = rds::Coverage::kBare;
        for (std::size_t i = 0; i < limbCount; ++i) {
            const rds::LimbInfo& limb = source.profile->limbs[i];
            if (CoverageSite(limb.site) == site) {
                coverage = limb.coverage;
                break;
            }
        }
        const std::string_view name = rds::ToString(coverage);
        out << "    " << site << ": { type: \"" << name << "\", name: \""
            << (coverage == rds::Coverage::kBare ? "" : "as recorded") << "\", weight: "
            << (coverage == rds::Coverage::kBare ? "0.000" : "1.000") << " }\n";
    }
    out << "\n";

    out << "environment:\n";
    out << "  cell_name: \"" << source.cell << "\"\n\n";

    out << "session:\n";
    out << "  duration_ms: " << static_cast<std::int64_t>(durationMs) << "\n";
    out << "  impacts: " << impacts << "\n";
    out << "  dropped: 0\n";
    out << "  complete: true\n";
}

}  // namespace

// ═════════════════════════════════════════════════════════════════════════════
// the enable flags
// ═════════════════════════════════════════════════════════════════════════════

void TakeFlags::Load(const fs::path& file) {
    m_file = file;
    m_disabled.clear();
    std::ifstream in(file);
    if (!in) return;
    std::string line;
    while (std::getline(in, line)) {
        const std::string_view trimmed = Trim(line);
        if (trimmed.empty() || trimmed.front() == '#' || trimmed.front() == '[') continue;
        const auto eq = trimmed.find('=');
        if (eq == std::string_view::npos) continue;
        m_disabled[std::string(Trim(trimmed.substr(0, eq)))] =
            ToDouble(trimmed.substr(eq + 1), 1.0) == 0.0;
    }
}

void TakeFlags::Save() const {
    if (m_file.empty()) return;
    std::error_code ec;
    fs::create_directories(m_file.parent_path(), ec);
    std::ofstream out(m_file, std::ios::trunc);
    if (!out) return;
    out << "# Which takes are in the Num4 / Num6 cycle. 0 excludes one, 1 includes it.\n";
    out << "# A take with no line here is included, so a folder that predates this file is\n";
    out << "# entirely enabled.\n";
    for (const auto& [stem, disabled] : m_disabled) {
        out << stem << "=" << (disabled ? 0 : 1) << "\n";
    }
}

bool TakeFlags::Enabled(const std::string& stem) const {
    const auto it = m_disabled.find(stem);
    return it == m_disabled.end() || !it->second;
}

void TakeFlags::SetEnabled(const std::string& stem, bool enabled) {
    m_disabled[stem] = !enabled;
    Save();
}

void TakeFlags::Erase(const std::string& stem) {
    if (m_disabled.erase(stem) != 0) Save();
}

// ═════════════════════════════════════════════════════════════════════════════
// writing
// ═════════════════════════════════════════════════════════════════════════════

std::string SafeStem(std::string_view name) {
    std::string out;
    bool lastWasUnderscore = false;
    for (const char c : name) {
        const bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                          (c >= '0' && c <= '9');
        if (keep) {
            out += c;
            lastWasUnderscore = false;
        } else if (!lastWasUnderscore && !out.empty()) {
            out += '_';
            lastWasUnderscore = true;
        }
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out.empty() ? std::string("take") : out;
}

std::string NextTakeStem(const fs::path& directory, std::string_view base) {
    std::error_code ec;
    for (int n = 1; n < 9999; ++n) {
        std::string candidate = std::format("{}_{}", base, n);
        if (!fs::exists(directory / (candidate + ".csv"), ec)) return candidate;
    }
    return std::string(base) + "_x";
}

fs::path WriteTake(const fs::path& directory, const std::string& stem, const TakeSource& source,
                   double loMs, double hiMs, std::string& error, TakeWindow* window) {
    std::vector<const rds::FeedEvent*> kept;
    kept.reserve(source.events.size());
    for (const rds::FeedEvent& event : source.events) {
        if (event.timeMs >= loMs && event.timeMs <= hiMs) kept.push_back(&event);
    }
    if (kept.empty()) {
        error = "nothing in that stretch to write";
        return {};
    }
    std::stable_sort(kept.begin(), kept.end(),
                     [](const rds::FeedEvent* a, const rds::FeedEvent* b) {
                         return a->timeMs < b->timeMs;
                     });

    std::error_code ec;
    fs::create_directories(directory, ec);
    const fs::path csv = directory / (stem + ".csv");

    std::ofstream out(csv, std::ios::trunc | std::ios::binary);
    if (!out) {
        error = "cannot write " + csv.string();
        return {};
    }
    out << kCsvHeader << '\n';

    const double origin = kept.front()->timeMs - kLeadInMs;
    std::uint32_t seq = 1;
    std::uint32_t impacts = 0;
    std::vector<rds::FeedEvent> poseSamples;
    std::vector<rds::FeedEvent> vanillaRows;
    for (const rds::FeedEvent* event : kept) {
        // The vanilla track, for the same reason pose has its own file: the
        // impacts CSV is the *recorder's* format, and a take this program writes
        // has to read back identically to one the game wrote. A column added
        // there would break that; a sibling file cannot. See VanillaTrack.h.
        if (event->kind == rds::EventKind::kVanillaSound) {
            vanillaRows.push_back(*event);
            continue;
        }
        // Pose is dense - eighteen limbs a tick - and putting it here would cost
        // this file the one property it has, which is being readable. It goes to
        // its own sidecar instead, re-based against the same origin so the two
        // share a clock. The older captures' two snapshots are not pose and stay
        // exactly where they are - see rds::pose::IsTickSample.
        if (rds::pose::IsTickSample(*event)) {
            rds::FeedEvent sample = *event;
            sample.timeMs = event->timeMs - origin;
            poseSamples.push_back(sample);
            continue;
        }
        if (event->kind == rds::EventKind::kImpact) ++impacts;
        WriteRow(out, *event, seq++, event->timeMs - origin, source.profile, source.actorName);
    }
    const double durationMs = kept.back()->timeMs - origin;

    // A closing row, because the loader's duration and the phase machine's last
    // tick both come off the final timestamp and a take that ends on its last
    // contact never reaches Rest.
    rds::FeedEvent stop{};
    stop.timeMs = durationMs + kLeadInMs;
    stop.kind = rds::EventKind::kState;
    stop.actorId = kept.front()->actorId;
    std::snprintf(stop.text, sizeof(stop.text), "session_stop");
    WriteRow(out, stop, 0, stop.timeMs, source.profile, source.actorName);
    out.close();

    // Already filtered to the window and re-based above, so the range here is
    // deliberately wide open: applying loMs/hiMs a second time against a clock
    // that has since moved would silently drop every frame.
    std::size_t poseFrames = 0;
    std::string poseError;
    const fs::path posePath = directory / (stem + "_pose.bin");
    if (!rds::pose::Write(posePath, poseSamples, 0.0, -1.0e300, 1.0e300, poseFrames, poseError)) {
        spdlog::warn("capture: {} kept its rows but its pose sidecar failed: {}",
                     csv.filename().string(), poseError);
    }

    // Re-based against the same origin as the CSV, and with the window left wide
    // open for the reason the pose write gives: these were filtered to the window
    // above, and applying it again against a clock that has since moved would
    // drop every row.
    std::size_t vanillaWritten = 0;
    std::string vanillaError;
    const fs::path vanillaPath = directory / (stem + "_vanilla.csv");
    if (!rds::vanilla::Write(vanillaPath, vanillaRows, origin, -1.0e300, 1.0e300, vanillaWritten,
                             vanillaError)) {
        spdlog::warn("capture: {} kept its rows but its vanilla track failed: {}",
                     csv.filename().string(), vanillaError);
    }

    WriteSidecar(directory / (stem + ".yaml"), stem, source, durationMs + kLeadInMs, impacts,
                 poseFrames, vanillaWritten);

    // The origin, not loMs. What the caller needs to know is where this take's
    // zero landed in the source, and the answer is the first row it kept.
    if (window != nullptr) {
        window->originMs = origin;
        window->durationMs = durationMs + kLeadInMs;
    }

    spdlog::info(
        "capture: wrote {} ({} rows, {} impacts, {} pose frames, {} vanilla, {:.0f} ms)",
        csv.filename().string(), kept.size() + 1 - poseSamples.size() - vanillaRows.size(), impacts,
        poseFrames, vanillaWritten, durationMs + kLeadInMs);
    return csv;
}

// ═════════════════════════════════════════════════════════════════════════════
// lining a take up with its video
// ═════════════════════════════════════════════════════════════════════════════

bool WriteSyncTrack(const fs::path& directory, const std::string& stem,
                    const std::vector<SyncSample>& samples, double originMs, std::string& error) {
    if (samples.empty()) {
        error = "no clock samples were taken";
        return false;
    }
    std::error_code ec;
    fs::create_directories(directory, ec);
    const fs::path file = directory / (stem + "_sync.csv");
    std::ofstream out(file, std::ios::trunc | std::ios::binary);
    if (!out) {
        error = "cannot write " + file.string();
        return false;
    }

    out << "# Lining the impacts CSV up with the video.\n";
    out << "#\n";
    out << "# t_ms            this take's own clock - the column the impacts CSV also stamps\n";
    out << "# obs_duration_ms OBS's output clock at that instant, in milliseconds\n";
    out << "# rtt_ms          how long the round trip took, and so how much the row is worth\n";
    out << "#\n";
    out << "# video_time_ms = t_ms + offset. Fit the offset through the low-rtt rows rather\n";
    out << "# than taking the first one: over a long take the two clocks drift apart, and a\n";
    out << "# piecewise fit follows that where a single number cannot.\n";
    out << "#\n";
    out << "# Written by the devbench, which drove the recording itself - so the mp4 beside\n";
    out << "# this file is OBS's whole output and the fitted offset needs no cut subtracted\n";
    out << "# out of it. t_ms comes off the game's session clock read across the link, so a\n";
    out << "# millisecond or two of the intercept is the socket rather than the picture; the\n";
    out << "# slope is unaffected, because that error is the same on every row.\n";
    out << "t_ms,obs_duration_ms,rtt_ms\n";
    for (const SyncSample& sample : samples) {
        out << std::format("{:.0f},{:.0f},{:.0f}\n", sample.gameMs - originMs, sample.obsMs,
                           sample.rttMs);
    }
    out.close();

    spdlog::info("capture: wrote {} ({} row(s))", file.filename().string(), samples.size());
    return true;
}

void AppendObsBlock(const fs::path& yaml, const ObsTakeInfo& info) {
    if (info.outputPath.empty()) {
        return;
    }
    std::ofstream out(yaml, std::ios::app);
    if (!out) {
        return;
    }
    // Windows paths, in a double-quoted scalar. The recorder wrote them the same
    // way and the loader unquotes without unescaping, so a lone backslash would
    // come back as a lone backslash either way - but a path ending in one would
    // eat the closing quote, and that is a file nobody can parse.
    const auto quoted = [](std::string_view text) {
        std::string out;
        out.reserve(text.size() + 2);
        for (const char c : text) {
            if (c == '\\' || c == '"') out += '\\';
            out += c;
        }
        return out;
    };
    out << "\nobs:\n";
    out << "  # video_time_ms = t_ms + offset_ms. offset_ms is the sync track's fitted\n";
    out << "  # intercept; sync_csv carries the whole series, which is what to use for a\n";
    out << "  # long take.\n";
    out << "  # OBS was started and stopped for this take, so output_path below is the take's\n";
    out << "  # own whole recording. Nothing was cut off its front and offset_ms therefore\n";
    out << "  # means what it says against the file sitting beside this one.\n";
    out << "  output_path: \"" << quoted(info.outputPath) << "\"\n";
    out << "  sync_csv: \"" << quoted(info.syncCsv) << "\"\n";
    out << "  offset_ms: " << static_cast<std::int64_t>(std::llround(info.offsetMs)) << "\n";
    out << "  obs_version: \"" << quoted(info.obsVersion) << "\"\n";
    out << "  record_directory: \"" << quoted(info.recordDirectory) << "\"\n";
}

TakeSource SourceFromCapture(const std::vector<rds::FeedEvent>& events,
                             const rds::link::ProfileMessage* subject, std::string note) {
    TakeSource source;
    source.events = events;
    source.note = std::move(note);
    if (subject != nullptr) {
        source.profile = &subject->profile;
        source.actorName = subject->profile.name;
        source.formId = subject->formId;
        source.cell = subject->cell;
    }
    if (source.actorName.empty()) source.actorName = "unknown";
    return source;
}

// ═════════════════════════════════════════════════════════════════════════════
// editing
// ═════════════════════════════════════════════════════════════════════════════

bool DeleteEventRange(const fs::path& csv, double loMs, double hiMs, std::size_t& removed,
                      std::string& error) {
    removed = 0;
    std::ifstream in(csv, std::ios::binary);
    if (!in) {
        error = "cannot open " + csv.string();
        return false;
    }

    std::string header;
    if (!std::getline(in, header)) {
        error = csv.string() + " is empty";
        return false;
    }
    // Which column t_ms is, by name. The schema has moved once already and a
    // hardcoded index would silently delete rows by their sequence number.
    std::size_t timeColumn = static_cast<std::size_t>(-1);
    {
        std::size_t index = 0;
        std::size_t start = 0;
        const std::string_view view{header};
        for (std::size_t i = 0; i <= view.size(); ++i) {
            if (i != view.size() && view[i] != ',') continue;
            if (Trim(view.substr(start, i - start)) == "t_ms") {
                timeColumn = index;
                break;
            }
            ++index;
            start = i + 1;
        }
    }
    if (timeColumn == static_cast<std::size_t>(-1)) {
        error = csv.string() + " has no t_ms column";
        return false;
    }

    const fs::path temp = csv.parent_path() / (csv.stem().string() + ".csv.tmp");
    std::ofstream out(temp, std::ios::trunc | std::ios::binary);
    if (!out) {
        error = "cannot write " + temp.string();
        return false;
    }
    out << header << '\n';

    std::string line;
    std::size_t kept = 0;
    while (std::getline(in, line)) {
        const std::string_view trimmed = Trim(line);
        if (trimmed.empty()) continue;
        double timeMs = 0.0;
        if (TimeOfRow(trimmed, timeColumn, timeMs) && timeMs >= loMs && timeMs <= hiMs) {
            ++removed;
            continue;
        }
        out << trimmed << '\n';
        ++kept;
    }
    in.close();
    out.close();

    if (kept == 0) {
        // Deleting every row leaves a file the loader rejects, which is worse
        // than refusing: the take would still be listed and would fail to open
        // for the rest of the session with no way back.
        std::error_code ec;
        fs::remove(temp, ec);
        error = "that would delete every row - delete the take instead";
        removed = 0;
        return false;
    }

    std::error_code ec;
    fs::remove(csv, ec);
    fs::rename(temp, csv, ec);
    if (ec) {
        error = "could not replace " + csv.string();
        return false;
    }
    spdlog::info("capture: dropped {} row(s) from {} between {:.0f} and {:.0f} ms", removed,
                 csv.filename().string(), loMs, hiMs);
    return true;
}

std::size_t DeleteTake(const fs::path& csv, const fs::path& cacheRoot, std::string& error) {
    const std::string stem = csv.stem().string();
    const fs::path directory = csv.parent_path();
    std::error_code ec;
    std::size_t gone = 0;

    for (const fs::path& file : {csv, directory / (stem + ".yaml"),
                                 directory / (stem + "_sync.csv"), directory / (stem + ".mp4"),
                                 directory / (stem + "_pose.bin"),
                                 directory / (stem + "_vanilla.csv")}) {
        if (fs::remove(file, ec)) ++gone;
    }
    ClearFrameCache(cacheRoot, stem);

    if (gone == 0) {
        error = "nothing to delete for " + stem;
    } else {
        spdlog::info("capture: deleted {} ({} file(s) and its frame cache)", stem, gone);
    }
    return gone;
}

fs::path SliceTake(const rds::Recording& recording, const rds::RecordingInfo& info,
                   const fs::path& directory, const std::string& stem, double loMs, double hiMs,
                   std::string& error, TakeWindow* window) {
    TakeSource source;
    source.events = recording.Events();
    // The vanilla track rides along, because `Events()` deliberately does not
    // carry it - see Recording::VanillaTrack. WriteTake splits it back out into
    // the cut's own `_vanilla.csv`, re-based onto the cut's origin, so a slice
    // keeps the reference track a slice of the same fall needs.
    const std::vector<rds::FeedEvent>& vanillaTrack = recording.VanillaTrack();
    source.events.insert(source.events.end(), vanillaTrack.begin(), vanillaTrack.end());
    const rds::ActorId actorId =
        recording.Events().empty() ? rds::ActorId{} : recording.Events().front().actorId;
    source.profile = recording.Profile(actorId);
    // The form id has to come with the limb table. The rows keep their own
    // actor_id, the loader keys the profile off `actor.form.id`, and a cut that
    // left it at the placeholder wrote a take whose profile could never be
    // matched to its own contacts: every limb resolved to null, so the whole
    // take ran with unknown sites, no mirror-drop and no chain merge.
    source.formId = std::format("{:08X}", actorId);
    source.actorName = info.actorName;
    source.cell = info.cell;
    source.note = std::format("cut from {} at {:.0f}-{:.0f} ms{}", info.stem, loMs, hiMs,
                              info.note.empty() ? "" : " - " + info.note);
    return WriteTake(directory, stem, source, loMs, hiMs, error, window);
}

}  // namespace tb
