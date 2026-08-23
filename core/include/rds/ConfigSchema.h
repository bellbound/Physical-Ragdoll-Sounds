#pragma once

// One description of every parameter, and everything else reads it.
//
// The ini reader, the ini writer, the defaults, the clamping, the "which values
// differ from default" log line, and the testbench's whole slider panel are all
// walks over the same table. That is the design's manifest idea applied to the
// config: tuning is sliders rather than ini edits, and the sliders exist because
// the parameter was declared, not because somebody also wrote UI for it.
//
// Adding a parameter is two lines: the field in Config.h, and its RDS_PARAM row
// in ConfigSchema.cpp.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
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
};

/// Every parameter of RagdollSounds_Algorithm.ini, in file order.
[[nodiscard]] std::span<const ParamDesc> AlgorithmParams();

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

/// Every param whose current value differs from its default, as
/// "Section:Key=value (default d)". Logged at info on load: it is the single
/// most useful line in a user's log, because it says what they changed.
[[nodiscard]] std::vector<std::string> Deltas(const void* root, std::span<const ParamDesc> params);

}  // namespace rds
