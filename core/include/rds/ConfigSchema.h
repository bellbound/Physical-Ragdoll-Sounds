#pragma once

// One description of every parameter, and everything else reads it: the ini reader
// and writer, the defaults, the clamping, the delta log line and the testbench's
// whole slider panel are all walks over the same table. The sliders exist because
// the parameter was declared, not because somebody also wrote UI for it.
//
// Adding a parameter is two lines: the field in Config.h and its RDS_PARAM row.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "rds/Config.h"

namespace rds {

/// kString is the odd one out: everything else is a number and travels through
/// `double`, which is what lets one slider and one ini line serve four types.
/// A path cannot, so it is stored in a fixed char array inside the config
/// struct - the structs have to stay trivially copyable - and read and written
/// through GetParamString / SetParamString instead. The numeric accessors
/// answer harmlessly for it rather than asserting, because every walk over the
/// table would otherwise need a branch before it could look at a row.
enum class ParamType : std::uint8_t { kBool, kInt, kFloat, kEnum, kString };

/// One tunable. `offset` is the byte offset of the member inside its root
/// config object, which is why those objects have to stay standard-layout.
struct ParamDesc {
    std::string_view section;  ///< ini section, e.g. "Arbitration"
    std::string_view key;      ///< ini key, hungarian-prefixed like the other mods here
    ParamType type{};
    std::size_t offset{};

    double defaultValue{};
    double minValue{};
    double maxValue{};
    double step{};

    std::string_view group;  ///< which stage or strategy owns it - the UI groups on this
    std::string_view label;  ///< short human name for the slider

    /// What it changes *perceptually*. The design asks for this explicitly, and
    /// it is the difference between a panel of ninety numbers and a panel you
    /// can tune by ear. Shown as the slider's tooltip and written into the ini
    /// as the key's comment.
    std::string_view tooltip;

    /// Names for kEnum, indexed by value. Empty otherwise.
    std::span<const std::string_view> enumNames;

    /// Size of the char array for kString, including the terminator. Zero for
    /// every other type. Declared last so the RDS_PARAM macro, which stops at
    /// `enumNames`, leaves it at zero without having to know about it.
    std::size_t capacity{};

    /// Where this parameter used to live, for a file written before it moved.
    /// The ini reader falls back to it when the current section and key are not
    /// in the file, so a rename costs nobody their tuning; the writer drops the
    /// old line and writes the parameter out under its current name, so the
    /// file migrates itself the first time anything is saved.
    ///
    /// Empty for every parameter that has never moved, which is almost all of
    /// them. Set through `Renamed()` rather than by the macro - see
    /// ConfigSchema.cpp.
    std::string_view legacySection;
    std::string_view legacyKey;

    /// This row shares its line in the UI with the next one: a ramp's two ends,
    /// a gain and its trim, a toggle and the window it applies to. Two columns
    /// is the most the panel will do, so the flag is never set on two rows in a
    /// row. Set through `Pairs()`.
    bool pairWithNext{};

    /// A rule is drawn above this row, because it opens a feature of its own inside
    /// a group that holds several. The group header says what the drawer is; the
    /// rule says where one thing inside it stops and the next begins.
    ///
    /// Never on the right-hand half of a pair, and the panel drops it on the first
    /// row a group draws, so a thinned-out group never opens with a rule under its
    /// header. Set by RDS_HRULE / RDS_HPAIR.
    bool ruleBefore{};

    /// The name before *that* one, for a row that has moved twice. The slide keys
    /// were `[Phase]`, then `[Motion]`, and are `[Slide]` today; with one legacy
    /// slot the second move would have cost everybody the tuning from before the
    /// first. `Renamed()` chains and pushes the older name in here.
    ///
    /// Last in the struct on purpose: RDS_PAIRS and RDS_HRULE initialise by
    /// position up to `ruleBefore`, so anything added before that point silently
    /// shifts nine hundred rows one field to the right.
    std::string_view legacySection2;
    std::string_view legacyKey2;

    /// This row is tuned per actor mode: three values, three columns in the panel,
    /// three sections in the ini (`[Ingest]`, `[Ingest.Gameplay]`,
    /// `[Ingest.Combat]`).
    ///
    /// Not set by the macros and not typed per row - it is a property of the
    /// *struct* the row points into, so it is filled in one pass over the table
    /// from `ModeVaryingMembers()`. Adding a member to that list gives every row
    /// beneath it three columns; nothing else has to know.
    bool perMode{};
};

/// The members of `AlgorithmConfig` that are tuned per actor mode, as byte ranges
/// inside it. Anything outside them holds one value that every mode reads.
///
/// A range and not a list of rows because the question is "does this parameter
/// mean the same thing for a body on the floor, one walking, and one fighting?",
/// and that is answered once per struct rather than ninety times per section.
[[nodiscard]] std::span<const std::pair<std::size_t, std::size_t>> ModeVaryingMembers();

/// Whether an offset inside `AlgorithmConfig` falls in one of those ranges.
[[nodiscard]] bool IsModeVarying(std::size_t offset);

/// What a mode's rows are suffixed with in the ini: nothing for the ragdoll
/// column, which is why an ini written before the modes existed still loads as
/// the ragdoll one and every other column starts as its copy.
[[nodiscard]] std::string_view ModeSectionSuffix(ActorMode mode);

/// The section a row is written under for `mode`, e.g. "Ingest.Combat". Empty for
/// a row that is not `perMode` in any mode but the first, since it is written
/// once.
[[nodiscard]] std::string ModeSection(const ParamDesc& p, ActorMode mode);

/// Every parameter of RagdollSounds_Algorithm.ini, in file order.
///
/// Includes the generated per-surface rows, which live in a *second* file. That
/// is deliberate: everything that reasons about parameters - the panel, the
/// filter, the diff, the remote patcher, the export - wants one table, and only
/// the two functions that write files care which file a row belongs to.
[[nodiscard]] std::span<const ParamDesc> AlgorithmParams();

/// The rows that belong in RagdollSounds_Algorithm.ini: everything except the
/// surfaces list. A prefix of `AlgorithmParams()`.
[[nodiscard]] std::span<const ParamDesc> AlgorithmFileParams();

/// The rows that belong in RagdollSounds_Algorithm_Surfaces.ini: eight per
/// surface class, in class order, whether or not the class is opened. A suffix
/// of `AlgorithmParams()`.
[[nodiscard]] std::span<const ParamDesc> SurfaceParams();

/// How many rows each class contributes to `SurfaceParams()`, so a caller can
/// index it by class without knowing the field list.
[[nodiscard]] std::size_t SurfaceRowsPerClass();

/// Which surface a row belongs to, or kCount for the great majority of rows
/// that are not part of the list. Lets the panel hide a closed class's rows
/// without matching on section names itself.
[[nodiscard]] SurfaceClass SurfaceClassOfParam(const ParamDesc& p);

/// Just the opened classes' rows, in class order - what the surfaces file is
/// actually written from. A closed class contributes nothing, which is how an
/// unopened surface costs no ini lines and stays closed on the next load.
[[nodiscard]] std::vector<ParamDesc> OpenedSurfaceParams(const AlgorithmConfig& config);

/// Every parameter of RagdollSounds.ini.
[[nodiscard]] std::span<const ParamDesc> GeneralParams();

// ── typed access through a ParamDesc ─────────────────────────────────────────
//
// `root` is the address of the AlgorithmConfig or GeneralConfig the desc
// belongs to. Get/Set go through double so one UI widget and one ini line can
// serve all four types; the conversion is exact for every range we use.

[[nodiscard]] double GetParam(const void* root, const ParamDesc& p);
void SetParam(void* root, const ParamDesc& p, double value);

/// kString only. The view points into the config object and is valid for as
/// long as it is; an empty view for any other type.
[[nodiscard]] std::string_view GetParamString(const void* root, const ParamDesc& p);
/// Truncates at `capacity - 1` and always terminates. Nothing for other types.
void SetParamString(void* root, const ParamDesc& p, std::string_view text);

/// Clamp into [min, max] and, for kInt/kBool/kEnum, round to an integer.
[[nodiscard]] double CoerceParam(const ParamDesc& p, double value);

/// The value as it is written to the ini: "1" / "0" for bool, an integer for
/// int and enum, a short decimal for float.
[[nodiscard]] std::string FormatParam(const ParamDesc& p, double value);

/// Parse an ini value. Returns the default when the text does not parse, so a
/// hand-mangled file degrades to defaults rather than to zeroes.
[[nodiscard]] double ParseParam(const ParamDesc& p, std::string_view text);

/// "Arbitration:fRateCapMs" - stable identity for logging and for the
/// testbench's saved config files.
[[nodiscard]] std::string QualifiedKey(const ParamDesc& p);

/// The algorithm file's rows over a whole `ConfigSet` rather than one config:
/// every row once under the section it has always had, then the `perMode` rows
/// again under `[<Section>.Gameplay]` and `[<Section>.Combat]`.
///
/// The point of building this rather than teaching the reader and the writer
/// about modes: the three configs sit contiguously inside a `ConfigSet`, so a
/// row's offset from the set's root is `mode * sizeof(AlgorithmConfig) + offset`.
/// Pass `&set` as the root and `LoadInto`, `SaveFrom`, `ToIniText` and `Deltas`
/// all work on three columns without a line of change - including the merge
/// writer, which keeps every hand-written comment in the file.
///
/// Ordered base-column-first, so an ini written before the modes existed is
/// exactly the head of this file and a diff against it shows only the two blocks
/// that were appended.
[[nodiscard]] std::span<const ParamDesc> AlgorithmSetFileParams();

/// The `perMode` rows of one mode, over a `ConfigSet` root. What the log's delta
/// lines walk to say what a column was changed from.
[[nodiscard]] std::span<const ParamDesc> ModeParams(ActorMode mode);

// ── over a whole ConfigSet ───────────────────────────────────────────────────

/// Copy every row that is *not* `perMode` from the ragdoll column into the other
/// two.
///
/// Called after anything writes a config: it is what makes "read the actor's own
/// column" safe for parameters that have only one value. Without it a shared
/// value edited in the ragdoll column would be stale for an upright actor, which
/// is the sort of bug that is inaudible until it is baffling.
void MirrorSharedRows(ConfigSet& set);

/// Copy one row from one column into another - the panel's arrow between two
/// columns. A no-op for a row that is not `perMode`, since those are one value by
/// definition.
void CopyRow(ConfigSet& set, const ParamDesc& p, ActorMode from, ActorMode to);

/// Copy every row of one column into another, for the arrow on a group header.
/// Returns how many rows moved.
std::size_t CopyRows(ConfigSet& set, std::span<const ParamDesc> params, ActorMode from,
                     ActorMode to);

/// How many rows of `params` differ between two columns. What the panel puts on
/// a group header so a collapsed drawer still says whether it holds a difference.
[[nodiscard]] std::size_t CountDiffering(const ConfigSet& set, std::span<const ParamDesc> params,
                                         ActorMode a, ActorMode b);

/// Every param whose current value differs from its default, as
/// "Section:Key=value (default d)". Logged at info on load: it is the single
/// most useful line in a user's log, because it says what they changed.
[[nodiscard]] std::vector<std::string> Deltas(const void* root, std::span<const ParamDesc> params);

}  // namespace rds
