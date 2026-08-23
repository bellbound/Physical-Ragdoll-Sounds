#include "rds/Recording.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstring>
#include <format>
#include <fstream>
#include <unordered_map>

namespace rds {
namespace {

// ── small text helpers ───────────────────────────────────────────────────────

[[nodiscard]] std::string_view Trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() &&
           (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    return text;
}

[[nodiscard]] std::string_view Unquote(std::string_view text) {
    text = Trim(text);
    if (text.size() >= 2 && (text.front() == '"' || text.front() == '\'') &&
        text.back() == text.front()) {
        text.remove_prefix(1);
        text.remove_suffix(1);
    }
    return text;
}

[[nodiscard]] float ToFloat(std::string_view text, float fallback = 0.0f) {
    text = Trim(text);
    if (text.empty() || text == "-") {
        return fallback;
    }
    float value{};
    if (std::from_chars(text.data(), text.data() + text.size(), value).ec != std::errc{}) {
        return fallback;
    }
    return value;
}

[[nodiscard]] double ToDouble(std::string_view text, double fallback = 0.0) {
    text = Trim(text);
    if (text.empty() || text == "-") {
        return fallback;
    }
    double value{};
    if (std::from_chars(text.data(), text.data() + text.size(), value).ec != std::errc{}) {
        return fallback;
    }
    return value;
}

[[nodiscard]] std::int64_t ToInt(std::string_view text, std::int64_t fallback = 0) {
    text = Trim(text);
    if (text.empty() || text == "-") {
        return fallback;
    }
    std::int64_t value{};
    if (std::from_chars(text.data(), text.data() + text.size(), value).ec != std::errc{}) {
        return fallback;
    }
    return value;
}

[[nodiscard]] std::uint64_t ToHex(std::string_view text, std::uint64_t fallback = 0) {
    text = Unquote(text);
    if (text.empty() || text == "-") {
        return fallback;
    }
    std::uint64_t value{};
    if (std::from_chars(text.data(), text.data() + text.size(), value, 16).ec != std::errc{}) {
        return fallback;
    }
    return value;
}

void SplitCsv(std::string_view line, std::vector<std::string_view>& out) {
    out.clear();
    std::size_t start = 0;
    for (std::size_t i = 0; i <= line.size(); ++i) {
        if (i == line.size() || line[i] == ',') {
            out.push_back(line.substr(start, i - start));
            start = i + 1;
        }
    }
}

// ── the enum columns, which the recorder writes as names ─────────────────────
//
// other_material is RE::MaterialIDToString and other_layer is
// RE::CollisionLayerToString, so the file carries names and not numbers. Mapping
// them back rather than storing the string is what lets a FeedEvent stay
// trivially copyable and lets the live and replay paths hold the same value.

struct NamedId {
    std::string_view name;
    std::uint32_t id;
};

constexpr NamedId kMaterialNames[] = {
#include "MaterialNames.inc"
};

[[nodiscard]] std::uint32_t MaterialIdFromName(std::string_view name) {
    name = Trim(name);
    if (name.empty() || name == "-") {
        return 0;
    }
    for (const auto& entry : kMaterialNames) {
        if (entry.name == name) {
            return entry.id;
        }
    }
    // A shape can carry a material ID that no ESM record describes (08 §3), and
    // a modded one can mint its own. Unknown resolves to 0, which the surface
    // resolver reads as "fall back to the collision layer".
    return 0;
}

[[nodiscard]] ColLayer LayerFromName(std::string_view name) {
    name = Trim(name);
    if (name == "Static" || name == "AnimStatic") return ColLayer::kStatic;
    if (name == "Ground" || name == "Terrain") return ColLayer::kGround;
    if (name == "Biped" || name == "BipedNoCC" || name == "CharController") return ColLayer::kBiped;
    if (name == "DeadBip") return ColLayer::kDeadBip;
    if (name == "Props" || name == "Clutter") return ColLayer::kProps;
    if (name == "Trees") return ColLayer::kTrees;
    if (name == "Water") return ColLayer::kWater;
    return ColLayer::kOther;
}

[[nodiscard]] EventKind KindFromName(std::string_view name) {
    name = Trim(name);
    if (name == "impact") return EventKind::kImpact;
    if (name == "touch") return EventKind::kTouch;
    if (name == "separate") return EventKind::kSeparate;
    if (name == "limb_sample") return EventKind::kLimbSample;
    if (name == "listener") return EventKind::kListener;
    return EventKind::kState;
}

[[nodiscard]] ActorPhase PhaseFromName(std::string_view name) {
    name = Trim(name);
    if (name == "ragdoll") return ActorPhase::kRagdoll;
    if (name == "animated") return ActorPhase::kAnimated;
    if (name == "getup") return ActorPhase::kGetUp;
    return ActorPhase::kUnknown;
}

[[nodiscard]] MaterialSource SourceFromName(std::string_view name) {
    name = Trim(name);
    if (name == "shape") return MaterialSource::kShape;
    if (name == "terrain") return MaterialSource::kTerrain;
    return MaterialSource::kNone;
}

// ── the YAML subset ──────────────────────────────────────────────────────────
//
// Hand-rolled rather than a dependency, because what is needed is a flattened
// list of scalars out of a file this project writes itself. It understands
// indentation, `- ` sequences and `{ inline: maps }`, which is the whole of what
// the sidecar uses; folded scalars and lists are read as opaque text and never
// asked for.

class YamlFlat {
public:
    [[nodiscard]] bool Load(const std::filesystem::path& file) {
        std::ifstream in(file, std::ios::binary);
        if (!in) {
            return false;
        }
        struct Frame {
            int indent;
            std::string key;
            int sequenceIndex;
        };
        std::vector<Frame> stack;
        std::string raw;
        while (std::getline(in, raw)) {
            std::string_view line{raw};
            if (!line.empty() && line.back() == '\r') {
                line.remove_suffix(1);
            }
            const std::size_t firstChar = line.find_first_not_of(' ');
            if (firstChar == std::string_view::npos) {
                continue;
            }
            int indent = static_cast<int>(firstChar);
            line.remove_prefix(firstChar);
            if (line.front() == '#') {
                continue;
            }

            bool isSequenceItem = false;
            if (line.size() >= 2 && line[0] == '-' && line[1] == ' ') {
                isSequenceItem = true;
                line.remove_prefix(2);
                indent += 2;
            }

            const auto colon = line.find(':');
            if (colon == std::string_view::npos) {
                continue;  // a folded-scalar continuation line
            }
            const auto key = Trim(line.substr(0, colon));
            const auto value = Trim(line.substr(colon + 1));
            if (key.empty() || key.find(' ') != std::string_view::npos) {
                // "started_real: 2026-..." is fine; a sentence fragment carrying
                // a colon is not a key, and this is what tells them apart.
                if (!isSequenceItem) {
                    continue;
                }
            }

            // A sequence item pops at the indent of its own `- `, not at the
            // indent of the key that follows it: without that, the second
            // `- limb_index:` never closes the first element and every limb after
            // the head of the list lands on a path nobody asks for. The tell is
            // that every contact resolves to site "unknown".
            const int popIndent = isSequenceItem ? indent - 2 : indent;
            while (!stack.empty() && stack.back().indent >= popIndent) {
                stack.pop_back();
            }
            if (isSequenceItem) {
                // The `- ` opens a new element of the sequence its parent named.
                if (!stack.empty()) {
                    if (m_lastSequenceParent != PathOf(stack)) {
                        m_lastSequenceParent = PathOf(stack);
                        m_sequenceIndex = 0;
                    } else {
                        ++m_sequenceIndex;
                    }
                }
                stack.push_back({indent - 2, std::format("[{}]", m_sequenceIndex), 0});
            }

            const std::string prefix = PathOf(stack);
            const std::string path = prefix.empty() ? std::string(key)
                                                    : std::format("{}.{}", prefix, key);
            if (value.empty()) {
                stack.push_back({indent, std::string(key), 0});
                continue;
            }
            if (value.front() == '{') {
                Flatten(path, value);
                continue;
            }
            m_values[path] = std::string(Unquote(value));
        }
        return true;
    }

    [[nodiscard]] std::string_view Get(std::string_view path) const {
        const auto it = m_values.find(std::string(path));
        return it == m_values.end() ? std::string_view{} : std::string_view{it->second};
    }

    [[nodiscard]] bool Has(std::string_view path) const {
        return m_values.contains(std::string(path));
    }

private:
    template <class Frames>
    [[nodiscard]] static std::string PathOf(const Frames& frames) {
        std::string out;
        for (const auto& frame : frames) {
            if (frame.key.starts_with('[')) {
                out += frame.key;
            } else {
                if (!out.empty()) {
                    out += '.';
                }
                out += frame.key;
            }
        }
        return out;
    }

    /// `{ id: "0001A67D", editor_id: "", plugin: "Skyrim.esm" }` -> three entries.
    void Flatten(const std::string& path, std::string_view inlineMap) {
        inlineMap = Trim(inlineMap);
        if (inlineMap.size() >= 2) {
            inlineMap.remove_prefix(1);
            inlineMap.remove_suffix(1);
        }
        std::size_t start = 0;
        int depth = 0;
        bool quoted = false;
        for (std::size_t i = 0; i <= inlineMap.size(); ++i) {
            const char c = i < inlineMap.size() ? inlineMap[i] : ',';
            if (c == '"') {
                quoted = !quoted;
            } else if (!quoted && (c == '[' || c == '{')) {
                ++depth;
            } else if (!quoted && (c == ']' || c == '}')) {
                --depth;
            } else if (!quoted && depth == 0 && c == ',') {
                const auto item = Trim(inlineMap.substr(start, i - start));
                const auto colon = item.find(':');
                if (colon != std::string_view::npos) {
                    m_values[std::format("{}.{}", path, Trim(item.substr(0, colon)))] =
                        std::string(Unquote(item.substr(colon + 1)));
                }
                start = i + 1;
            }
        }
    }

    std::unordered_map<std::string, std::string> m_values;
    std::string m_lastSequenceParent;
    int m_sequenceIndex{};
};

/// Coverage from the `coverage:` map, with the data dictionary's two warnings.
[[nodiscard]] Coverage CoverageFrom(const YamlFlat& yaml, std::string_view site) {
    const auto type = yaml.Get(std::format("armour.coverage.{}.type", site));
    const auto name = yaml.Get(std::format("armour.coverage.{}.name", site));
    const auto weight = yaml.Get(std::format("armour.coverage.{}.weight", site));
    if (type.empty()) {
        return Coverage::kBare;
    }
    if (type == "heavy") return Coverage::kHeavy;
    if (type == "light") return Coverage::kLight;
    if (type == "bare") return Coverage::kBare;
    // TNG's skin is a real TESObjectARMO occupying five slots, so a stripped
    // subject reads `clothing` with an empty name and zero weight on every site.
    // Nameless and weightless is the tell, and it means bare.
    if (name.empty() && ToFloat(weight, 0.0f) <= 0.0f) {
        return Coverage::kBare;
    }
    return Coverage::kCloth;
}

/// The `coverage:` map is per body site and has no row for upper arms or thighs.
/// A vanilla cuirass declares covers_slots [2, 4, 5, 8], so forearms and calves
/// wear whatever the torso does - and the upper arm and thigh sit under the same
/// piece. Borrowing from the neighbour is right, and it is why this reads the
/// map rather than slot occupancy.
[[nodiscard]] std::string_view CoverageSiteName(LimbSite site) {
    switch (site) {
        case LimbSite::kHead:
        case LimbSite::kNeck:
            return "head";
        case LimbSite::kTorso:
            return "torso";
        case LimbSite::kHand:
            return "hands";
        case LimbSite::kForearm:
        case LimbSite::kUpperArm:
            return "forearms";
        case LimbSite::kFoot:
            return "feet";
        case LimbSite::kCalf:
        case LimbSite::kThigh:
            return "calves";
        case LimbSite::kUnknown:
        case LimbSite::kCount:
            break;
    }
    return "torso";
}

[[nodiscard]] std::filesystem::path Sibling(const std::filesystem::path& csv,
                                            std::string_view suffix) {
    return csv.parent_path() / (csv.stem().string() + std::string(suffix));
}

/// video_time_ms = t_ms + offset. Fit through the low-rtt rows rather than
/// taking the first: over a long take the two clocks drift, and the mp4s are
/// cuts of a longer OBS recording whose cut point is recorded nowhere - which is
/// why the testbench still needs a per-take nudge on top of this.
[[nodiscard]] bool ReadSyncOffset(const std::filesystem::path& file, double& offsetMs,
                                  double& driftMsPerSec) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return false;
    }
    struct Row {
        double t;
        double obs;
        double rtt;
    };
    std::vector<Row> rows;
    std::string line;
    std::vector<std::string_view> fields;
    while (std::getline(in, line)) {
        const auto trimmed = Trim(line);
        if (trimmed.empty() || trimmed.front() == '#' || trimmed.starts_with("t_ms")) {
            continue;
        }
        SplitCsv(trimmed, fields);
        if (fields.size() < 3) {
            continue;
        }
        rows.push_back({ToDouble(fields[0]), ToDouble(fields[1]), ToDouble(fields[2])});
    }
    if (rows.empty()) {
        return false;
    }
    double bestRtt = rows.front().rtt;
    for (const auto& row : rows) {
        bestRtt = std::min(bestRtt, row.rtt);
    }

    // Least squares of (obs - t) against t over the rows worth trusting, which
    // gives the intercept and the slope in one pass. A single averaged offset
    // would be the intercept only, and the slope is the half that matters over a
    // long take.
    double sumT = 0.0;
    double sumD = 0.0;
    double sumTT = 0.0;
    double sumTD = 0.0;
    int count = 0;
    for (const auto& row : rows) {
        if (row.rtt > bestRtt + 5.0) {
            continue;
        }
        const double delta = row.obs - row.t;
        sumT += row.t;
        sumD += delta;
        sumTT += row.t * row.t;
        sumTD += row.t * delta;
        ++count;
    }
    if (count == 0) {
        return false;
    }
    const double n = count;
    const double denominator = n * sumTT - sumT * sumT;
    if (count >= 2 && std::fabs(denominator) > 1.0e-6) {
        const double slope = (n * sumTD - sumT * sumD) / denominator;
        offsetMs = (sumD - slope * sumT) / n;
        driftMsPerSec = slope * 1000.0;
    } else {
        // One usable row is an intercept and nothing else. Saying the drift is
        // zero is honest here; guessing one from a single point would not be.
        offsetMs = sumD / n;
        driftMsPerSec = 0.0;
    }
    return true;
}

void FillInfoFromYaml(const YamlFlat& yaml, RecordingInfo& info) {
    info.note = std::string(yaml.Get("recording.note"));
    info.actorName = std::string(yaml.Get("actor.name"));
    info.cell = std::string(yaml.Get("environment.cell_name"));
    info.durationMs = ToDouble(yaml.Get("session.duration_ms"));
    info.impacts = static_cast<std::uint32_t>(ToInt(yaml.Get("session.impacts")));
    info.dropped = static_cast<std::uint32_t>(ToInt(yaml.Get("session.dropped")));
    info.complete = yaml.Get("session.complete") == "true";
}

}  // namespace

// ── the vocabulary, the way out ──────────────────────────────────────────────

std::string_view MaterialName(std::uint32_t materialId) {
    if (materialId == 0) {
        return "-";
    }
    for (const auto& entry : kMaterialNames) {
        if (entry.id == materialId) {
            return entry.name;
        }
    }
    return "-";
}

std::string_view LayerName(ColLayer layer) {
    switch (layer) {
        case ColLayer::kStatic: return "Static";
        case ColLayer::kGround: return "Ground";
        case ColLayer::kBiped: return "Biped";
        case ColLayer::kDeadBip: return "DeadBip";
        case ColLayer::kProps: return "Props";
        case ColLayer::kTrees: return "Trees";
        case ColLayer::kWater: return "Water";
        case ColLayer::kOther: break;
    }
    return "-";
}

std::string_view SourceName(MaterialSource source) {
    switch (source) {
        case MaterialSource::kShape: return "shape";
        case MaterialSource::kTerrain: return "terrain";
        case MaterialSource::kNone: break;
    }
    return "-";
}

std::string_view PhaseName(ActorPhase phase) {
    switch (phase) {
        case ActorPhase::kAnimated: return "animated";
        case ActorPhase::kRagdoll: return "ragdoll";
        case ActorPhase::kGetUp: return "getup";
        case ActorPhase::kUnknown: break;
    }
    return "unknown";
}

bool Recording::Load(const std::filesystem::path& csvPath, std::string& error) {
    m_info = RecordingInfo{};
    m_profile = ActorProfile{};
    m_listener = ListenerState{};
    m_events.clear();
    m_frames.clear();
    m_cursor = 0;
    m_frameTimeSec = 0.0f;
    m_info.stem = csvPath.stem().string();

    // ── the sidecar ──────────────────────────────────────────────────────────
    YamlFlat yaml;
    const auto yamlPath = Sibling(csvPath, ".yaml");
    if (!yaml.Load(yamlPath)) {
        error = std::format("no sidecar beside {}", csvPath.string());
        return false;
    }
    FillInfoFromYaml(yaml, m_info);

    m_profile.name = m_info.actorName;
    m_profile.actorId = static_cast<ActorId>(ToHex(yaml.Get("actor.form.id")));
    m_profile.isPlayer = yaml.Get("actor.is_player") == "true";
    m_profile.scale = ToFloat(yaml.Get("actor.scale"), 1.0f);

    const auto limbCount = static_cast<std::size_t>(ToInt(yaml.Get("ragdoll.limb_count"), 0));
    m_profile.limbs.reserve(limbCount);
    for (std::size_t i = 0; i < limbCount; ++i) {
        const auto prefix = std::format("ragdoll.limbs[{}]", i);
        LimbInfo limb;
        limb.boneName = std::string(yaml.Get(std::format("{}.name", prefix)));
        limb.site = SiteFromBoneName(limb.boneName);
        limb.chain = ChainFromBoneName(limb.boneName);
        limb.coverage = CoverageFrom(yaml, CoverageSiteName(limb.site));
        // Deliberately not the sidecar's mass. BuildYaml runs while the bodies
        // are still keyframed and a keyframed Havok body reports 0, which is
        // what three of twelve takes carry. The CSV's own column is read live in
        // the callback and is right in every take, so it is filled in below.
        limb.havokMass = 0.0f;
        limb.radius = 0.0f;
        limb.bodyId = ToHex(yaml.Get(std::format("{}.body", prefix)));
        m_profile.limbs.push_back(std::move(limb));
    }

    m_profile.reverb.valid = yaml.Has("environment.reverb.decay_time_ms");
    m_profile.reverb.acousticSpace = std::string(yaml.Get("environment.acoustic_space.editor_id"));
    m_profile.reverb.decayTimeMs = static_cast<int>(ToInt(yaml.Get("environment.reverb.decay_time_ms")));
    m_profile.reverb.hfReferenceHz =
        static_cast<int>(ToInt(yaml.Get("environment.reverb.hf_reference_hz")));
    m_profile.reverb.roomFilter = static_cast<int>(ToInt(yaml.Get("environment.reverb.room_filter")));
    m_profile.reverb.roomHfFilter =
        static_cast<int>(ToInt(yaml.Get("environment.reverb.room_hf_filter")));
    m_profile.reverb.diffusionPct =
        static_cast<int>(ToInt(yaml.Get("environment.reverb.diffusion_pct")));
    m_profile.reverb.densityPct = static_cast<int>(ToInt(yaml.Get("environment.reverb.density_pct")));

    // ── the rows ─────────────────────────────────────────────────────────────
    std::ifstream in(csvPath, std::ios::binary);
    if (!in) {
        error = std::format("cannot open {}", csvPath.string());
        return false;
    }

    std::string header;
    if (!std::getline(in, header)) {
        error = std::format("{} is empty", csvPath.string());
        return false;
    }
    std::vector<std::string_view> fields;
    SplitCsv(Trim(header), fields);
    std::unordered_map<std::string, std::size_t> column;
    for (std::size_t i = 0; i < fields.size(); ++i) {
        column[std::string(Trim(fields[i]))] = i;
    }
    // By name rather than by position: the schema has already lost slide_speed
    // and gained tangent_speed once, and an off-by-one over a whole take is not
    // a failure anybody notices until the sound is wrong.
    const auto columnOf = [&](std::string_view name) -> std::size_t {
        const auto it = column.find(std::string(name));
        return it == column.end() ? static_cast<std::size_t>(-1) : it->second;
    };
    const std::size_t cTime = columnOf("t_ms");
    const std::size_t cSeq = columnOf("seq");
    const std::size_t cEvent = columnOf("event");
    if (cTime == static_cast<std::size_t>(-1) || cEvent == static_cast<std::size_t>(-1)) {
        error = std::format("{} has no t_ms or event column", csvPath.string());
        return false;
    }
    const std::size_t cPhase = columnOf("phase");
    const std::size_t cActorId = columnOf("actor_id");
    const std::size_t cLimbIndex = columnOf("limb_index");
    const std::size_t cImpact = columnOf("impact_speed");
    const std::size_t cNormal = columnOf("normal_speed");
    const std::size_t cTangent = columnOf("tangent_speed");
    const std::size_t cBodySpeed = columnOf("body_speed");
    const std::size_t cAngular = columnOf("angular_speed");
    const std::size_t cMass = columnOf("mass");
    const std::size_t cRadius = columnOf("limb_radius");
    const std::size_t cPosX = columnOf("pos_x");
    const std::size_t cNrmX = columnOf("nrm_x");
    const std::size_t cVelX = columnOf("vel_x");
    const std::size_t cLayer = columnOf("other_layer");
    const std::size_t cMaterial = columnOf("other_material");
    const std::size_t cSource = columnOf("material_source");
    const std::size_t cOtherBody = columnOf("other_body");
    const std::size_t cOtherLimb = columnOf("other_limb");
    const std::size_t cFirst = columnOf("manifold_first");
    const std::size_t cLast = columnOf("manifold_last");
    const std::size_t cState = columnOf("state");

    const auto at = [&](std::size_t index) -> std::string_view {
        return index < fields.size() ? fields[index] : std::string_view{};
    };

    std::string line;
    while (std::getline(in, line)) {
        const auto trimmed = Trim(line);
        if (trimmed.empty()) {
            continue;
        }
        SplitCsv(trimmed, fields);
        if (fields.size() < 4) {
            continue;
        }

        FeedEvent event{};
        event.timeMs = ToDouble(at(cTime));
        event.sourceSeq = static_cast<std::uint32_t>(ToInt(at(cSeq)));
        event.kind = KindFromName(at(cEvent));
        event.phase = PhaseFromName(at(cPhase));
        event.actorId = static_cast<ActorId>(ToHex(at(cActorId)));
        event.limbIndex = static_cast<std::uint16_t>(std::max<std::int64_t>(0, ToInt(at(cLimbIndex))));
        event.impactSpeed = ToFloat(at(cImpact));
        event.normalSpeed = ToFloat(at(cNormal));
        event.tangentSpeed = ToFloat(at(cTangent));
        event.bodySpeed = ToFloat(at(cBodySpeed));
        event.angularSpeed = ToFloat(at(cAngular));
        event.mass = ToFloat(at(cMass));
        event.limbRadius = ToFloat(at(cRadius));
        event.position = {ToFloat(at(cPosX)), ToFloat(at(cPosX + 1)), ToFloat(at(cPosX + 2))};
        event.normal = {ToFloat(at(cNrmX)), ToFloat(at(cNrmX + 1)), ToFloat(at(cNrmX + 2))};
        event.velocity = {ToFloat(at(cVelX)), ToFloat(at(cVelX + 1)), ToFloat(at(cVelX + 2))};
        event.otherBody = ToHex(at(cOtherBody));
        event.otherMaterial = MaterialIdFromName(at(cMaterial));
        event.otherLayer = LayerFromName(at(cLayer));
        event.materialSource = SourceFromName(at(cSource));
        const auto otherLimb = Trim(at(cOtherLimb));
        event.otherLimb = (otherLimb.empty() || otherLimb == "-")
                              ? -1
                              : static_cast<std::int32_t>(ToInt(otherLimb, -1));
        event.manifoldFirst = ToInt(at(cFirst)) != 0;
        event.manifoldLast = ToInt(at(cLast)) != 0;
        const auto state = Trim(at(cState));
        const std::size_t stateLength = std::min(state.size(), sizeof(event.text) - 1);
        std::memcpy(event.text, state.data(), stateLength);
        event.text[stateLength] = '\0';

        m_events.push_back(event);
    }

    if (m_events.empty()) {
        error = std::format("{} carries no rows", csvPath.string());
        return false;
    }

    // By t_ms, never by seq. The session_stop row is written out of band and
    // carries seq 0, so sorting by seq puts a take's last row first. Stable, so
    // rows sharing a timestamp keep their write order.
    std::ranges::stable_sort(m_events,
                             [](const FeedEvent& a, const FeedEvent& b) { return a.timeMs < b.timeMs; });

    // The CSV's mass, not the sidecar's, and the CSV's radius while we are here.
    for (const auto& event : m_events) {
        if (event.limbIndex >= m_profile.limbs.size()) {
            continue;
        }
        auto& limb = m_profile.limbs[event.limbIndex];
        if (limb.havokMass <= 0.0f && event.mass > 0.0f) {
            limb.havokMass = event.mass;
        }
        if (limb.radius <= 0.0f && event.limbRadius > 0.0f) {
            limb.radius = event.limbRadius;
        }
    }

    // Frames, from the gaps between contact batches. Measured: 1.0 us inside one
    // frame's callbacks against 20.4 ms between them, so 2 ms separates them with
    // three orders of magnitude to spare.
    constexpr double kFrameGapMs = 2.0;
    double previous = -1.0e9;
    for (const auto& event : m_events) {
        if (!event.IsContact()) {
            continue;
        }
        if (event.timeMs - previous > kFrameGapMs) {
            m_frames.push_back(event.timeMs);
        }
        previous = event.timeMs;
    }
    if (m_frames.empty()) {
        m_frames.push_back(m_events.front().timeMs);
    }
    if (m_frames.back() < m_events.back().timeMs) {
        m_frames.push_back(m_events.back().timeMs);
    }

    // Fill the quiet stretches back in. A batch of callbacks marks a frame that
    // *had* contacts, but the game ran frames in between as well, and the engine
    // does its phase work on the tick - so a replay that only ticks where the
    // solver spoke leaves a fall stuck in PrimaryImpact for however long the body
    // was in the air, and never reaches the settle at all. The step is the take's
    // own median frame interval, so the replay still steps at the rate the game
    // did rather than at one this runner invented.
    std::vector<double> deltas;
    deltas.reserve(m_frames.size());
    for (std::size_t i = 1; i < m_frames.size(); ++i) {
        const double delta = m_frames[i] - m_frames[i - 1];
        if (delta > 0.0 && delta < 60.0) {
            deltas.push_back(delta);
        }
    }
    double step = 16.6;
    if (!deltas.empty()) {
        std::ranges::nth_element(deltas, deltas.begin() + static_cast<std::ptrdiff_t>(deltas.size() / 2));
        step = deltas[deltas.size() / 2];
    }
    m_frameStepMs = step;

    std::vector<TimeMs> dense;
    dense.reserve(m_frames.size() * 2);
    for (std::size_t i = 0; i < m_frames.size(); ++i) {
        if (i > 0) {
            for (TimeMs t = m_frames[i - 1] + step; t < m_frames[i] - step * 0.5; t += step) {
                dense.push_back(t);
            }
        }
        dense.push_back(m_frames[i]);
    }
    m_frames.swap(dense);

    if (m_info.durationMs <= 0.0) {
        m_info.durationMs = m_events.back().timeMs;
    }

    // Video, when there is any.
    const auto video = Sibling(csvPath, ".mp4");
    std::error_code ec;
    if (std::filesystem::exists(video, ec)) {
        m_info.videoPath = video;
    }
    const auto sync = csvPath.parent_path() / (csvPath.stem().string() + "_sync.csv");
    if (std::filesystem::exists(sync, ec)) {
        m_info.hasSync = ReadSyncOffset(sync, m_info.videoOffsetMs, m_info.videoDriftMsPerSec);
    }
    if (!m_info.hasSync) {
        m_info.videoOffsetMs = ToDouble(yaml.Get("obs.offset_ms"));
    }

    // Named limbs are what everything downstream sizes, chains and gates on, so
    // a skeleton that came back nameless is worth saying out loud rather than
    // discovering later as a mod that sounds oddly flat.
    std::size_t named = 0;
    for (const auto& limb : m_profile.limbs) {
        named += limb.site != LimbSite::kUnknown ? 1u : 0u;
    }
    spdlog::info("recording: {} - {} rows, {} frames, {:.0f} ms, actor {} ({}), {}/{} limbs named",
                 m_info.stem, m_events.size(), m_frames.size(), m_info.durationMs,
                 m_info.actorName, m_info.complete ? "complete" : "INCOMPLETE", named,
                 m_profile.limbs.size());
    if (named == 0 && !m_profile.limbs.empty()) {
        spdlog::warn("recording: {} resolved no limb names - every contact will size off radius",
                     m_info.stem);
    }
    if (m_info.hasSync) {
        spdlog::info("recording: {} video offset {:.0f} ms, drift {:+.2f} ms/s", m_info.stem,
                     m_info.videoOffsetMs, m_info.videoDriftMsPerSec);
    }
    if (m_info.dropped != 0) {
        spdlog::warn("recording: {} dropped {} events - the take is incomplete", m_info.stem,
                     m_info.dropped);
    }
    return true;
}

std::vector<RecordingInfo> Recording::Scan(const std::filesystem::path& directory) {
    std::vector<RecordingInfo> out;
    std::error_code ec;
    if (!std::filesystem::is_directory(directory, ec)) {
        return out;
    }
    for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
        if (ec) {
            break;
        }
        const auto& path = entry.path();
        if (path.extension() != ".yaml") {
            continue;
        }
        const auto csv = path.parent_path() / (path.stem().string() + ".csv");
        if (!std::filesystem::exists(csv, ec)) {
            continue;
        }
        YamlFlat yaml;
        if (!yaml.Load(path)) {
            continue;
        }
        RecordingInfo info;
        info.stem = path.stem().string();
        FillInfoFromYaml(yaml, info);
        const auto video = path.parent_path() / (path.stem().string() + ".mp4");
        if (std::filesystem::exists(video, ec)) {
            info.videoPath = video;
        }
        const auto sync = path.parent_path() / (path.stem().string() + "_sync.csv");
        if (std::filesystem::exists(sync, ec)) {
            info.hasSync = ReadSyncOffset(sync, info.videoOffsetMs, info.videoDriftMsPerSec);
        }
        if (!info.hasSync) {
            info.videoOffsetMs = ToDouble(yaml.Get("obs.offset_ms"));
        }
        out.push_back(std::move(info));
    }
    std::ranges::sort(out, [](const RecordingInfo& a, const RecordingInfo& b) {
        return a.stem < b.stem;
    });
    return out;
}

bool Recording::Drain(TimeMs untilMs, std::vector<FeedEvent>& out) {
    // The frame time the engine's windows scale against: how long this batch is
    // after the one before it. Derived rather than measured, because the capture
    // resolves frames and not physics substeps.
    double frameStart = untilMs;
    for (std::size_t i = 0; i < m_frames.size(); ++i) {
        if (m_frames[i] > untilMs) {
            break;
        }
        frameStart = m_frames[i];
        if (i > 0) {
            m_frameTimeSec = static_cast<float>((m_frames[i] - m_frames[i - 1]) / 1000.0);
        }
    }
    (void)frameStart;

    while (m_cursor < m_events.size() && m_events[m_cursor].timeMs <= untilMs) {
        const FeedEvent& event = m_events[m_cursor];
        if (event.kind == EventKind::kListener) {
            m_listener.position = event.position;
            m_listener.facing = event.normal;
            m_listener.timeMs = event.timeMs;
        }
        out.push_back(event);
        ++m_cursor;
    }
    return m_cursor < m_events.size();
}

const ActorProfile* Recording::Profile(ActorId actor) const {
    return m_profile.actorId == actor ? &m_profile : nullptr;
}

const ListenerState& Recording::Listener() const { return m_listener; }

float Recording::FrameTimeSec() const { return m_frameTimeSec; }

void Recording::Rewind() {
    m_cursor = 0;
    m_frameTimeSec = 0.0f;
    m_listener = ListenerState{};
}

}  // namespace rds
