#pragma once

// The sound bank, declared rather than hardcoded.
//
// Each slot says what it is, how long it should be, how many variants it
// expects, and which axes it varies over. Resolution walks the axes and falls
// back, so a missing file is a quieter mod rather than a broken one - which is
// what lets us ship thirteen files and grow to twenty-nine without touching
// code, and what lets `grunt_impact` and `scream_big` sit declared and unfilled
// until somebody records a voice.
//
// Full set is 29 files. First taste is 13: imp_transient x3, imp_body x3,
// imp_sub x2, limb_tap x3, scrape_loop, foley_cloth. Build the sub layer in the
// first pass; without it none of this will feel like the references.

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "rds/Types.h"

namespace rds {

class SfxAssignments;
class SfxLibrary;

enum class SlotId : std::uint8_t {
    // impact composite - every audible impact is built from these
    kImpTransient = 0,  ///< 60-120 ms, +0 ms. Bright fast attack. The quietest layer
    kImpBody,           ///< 150-250 ms, +10-30 ms. Low-mid flesh and mass
    kImpSub,            ///< 250-400 ms, +55-75 ms. Pitched boom ~150 Hz -> 30 Hz. The loudest layer

    // surface skins - short, layered on the composite
    kSurfWood,   ///< 120-200 ms. Hollow knock
    kSurfStone,  ///< 100-160 ms. Hard, short
    kSurfSoft,   ///< 150-250 ms. Dull. The default for anything unresolved

    // grains and texture
    kLimbTap,     ///< 40-100 ms. Burst filler. Quiet, dry, heavily pitch-scattered
    kCrunchGran,  ///< 250-400 ms. Dense granular crackle in the low-mid
    kGoreWet,     ///< 200-400 ms. Squelch. Obliterate tier only

    // loops
    kScrapeLoop,  ///< 1.5-3 s. Low-tilted grinding rumble with grain. NOT a hiss
    kFoleyCloth,  ///< 1.5-3 s. Cloth rustle, no transients
    kAirWhoosh,   ///< 1-2 s. Low airy movement

    // accents
    kHeadImpact,  ///< 300-500 ms. Dull skull thud, granular edge, slight ring
    kSettleRest,  ///< 200-400 ms. Soft final flop. Closes the event

    // declared but unfilled - fallback resolution skips them silently, so adding
    // voice later is a config change and not a code change
    kGruntImpact,  ///< 300-600 ms
    kScreamBig,    ///< 800-1500 ms

    kCount
};

/// The axes a slot's files may vary over. Resolution drops them right to left.
enum class SlotAxis : std::uint8_t {
    kNone = 0,
    kSurface,   ///< surf_* pick on SurfaceClass
    kCoverage,  ///< bare / cloth / light / heavy - timbre only, never physics
    kSize,      ///< small / medium / large, from the limb site
};

struct SlotDesc {
    SlotId id{};
    std::string_view name;  ///< "imp_sub" - the filename stem and the ini key
    std::string_view role;
    std::uint8_t expectedVariants{};
    float minLengthMs{};
    float maxLengthMs{};
    bool isLoop{};
    std::string_view character;  ///< the asset brief, one line
};

[[nodiscard]] std::span<const SlotDesc> Slots();
[[nodiscard]] const SlotDesc& Slot(SlotId id);
[[nodiscard]] std::string_view ToString(SlotId id);

/// A concrete sound the renderer can play: one wav on disk, or a procedural
/// stand-in when there is no file.
///
/// No authored files exist yet, so the resolver synthesises from the slot's own
/// brief - the sub sweep in particular is fully specified by 01 §1 and can be
/// generated exactly. That makes the timing, the layer balance and the gates
/// audible today. Dropping a wav into the bank overrides its slot with no code
/// change, which is the point of resolving through a manifest at all.
struct ResolvedSound {
    SlotId slot{};
    std::uint8_t variant{};
    std::string path;      ///< empty when this is a procedural stand-in
    bool procedural{true};
    /// The real length: the wav's own for a file, the synthesised length for a
    /// stand-in. A renderer sizes its mix buffer from this, so a wrong value
    /// truncates the tail rather than merely mislabelling it.
    float lengthMs{};
};

/// Which files exist, and what to play when they do not.
class SoundBank {
public:
    /// Scan `directory` for `<slot name>_<NN>.wav`. Logs at info what it found
    /// and what fell back, once, at load: "imp_sub: 0/2 files, procedural" is
    /// exactly the line that explains why somebody's mod sounds thin.
    void Load(const std::string& directory);

    /// Fill from an explicit slot-to-file table instead of from filenames.
    ///
    /// This is what RagdollSounds_SFX.ini drives, and it is the only way a slot
    /// can play something not named after it. A slot the table leaves empty
    /// falls back to the convention scan of `fallbackDirectory`, so an ini that
    /// only reassigns `imp_body` leaves the other twenty-eight files exactly
    /// where they were - which is what makes this safe to ship on top of a pack
    /// somebody has already tuned against.
    ///
    /// Variant index is position in the list, not filename order: the list is
    /// the user's stated order and re-ordering it is a deliberate change to
    /// which file a given cue plays.
    void LoadAssigned(const SfxLibrary& library, const SfxAssignments& assignments,
                      const std::string& fallbackDirectory);

    /// Whether this slot's sound is a sustained texture the engine repeats.
    /// The manifest's own answer unless an assignment overrode it.
    [[nodiscard]] bool IsLooping(SlotId slot) const;

    /// Pick a variant for this slot along the axes. Never fails for a slot with
    /// a procedural stand-in; returns false only for the declared-and-unfilled
    /// voice slots, which callers skip silently.
    ///
    /// Selection is a shuffle bag, not random: random repeats immediately, and
    /// immediate repeats are what people notice.
    [[nodiscard]] bool Resolve(SlotId slot, SurfaceClass surface, Coverage coverage, LimbSite site,
                               ResolvedSound& out);

    /// The exact sound a cue names. Unlike Resolve, this is a pure lookup - it
    /// does not touch the shuffle bag - so a renderer can turn a cue's
    /// (slot, variant) back into the file the engine actually chose. Calling
    /// Resolve again instead would advance the bag and hand back a *different*
    /// variant, so the audio would not be the cue list the arbitrator emitted.
    ///
    /// False only where the slot has no variant at that index: the
    /// declared-and-unfilled voice slots, and nothing else.
    [[nodiscard]] bool Get(SlotId slot, std::uint8_t variant, ResolvedSound& out) const;

    /// Deterministic given the seed, so an A/B between two configs compares the
    /// configs and not two dice rolls.
    void Seed(std::uint32_t seed);

    [[nodiscard]] std::size_t FileCount(SlotId slot) const;

private:
    /// Clear every slot, then fill from `<slot name>_<NN>.wav` under
    /// `directory`. `only` limits it to the slots that are still empty, which
    /// is how LoadAssigned falls back for the slots the ini did not name.
    void ScanDirectory(const std::string& directory, bool onlyEmptySlots);

    /// The one "imp_sub: 0/2 files, procedural" pass, run at the end of either
    /// load path so both report the bank the same way.
    void LogContents(const char* source);

    struct SlotFiles {
        std::vector<ResolvedSound> variants;
        std::vector<std::uint8_t> bag;
        std::size_t bagCursor{};
        bool looping{};
        /// True when this slot's files came from the filename convention rather
        /// than from an assignment. Only those get re-sorted by path - the ini's
        /// order is the user's and must survive.
        bool scannedByName{true};
    };
    SlotFiles m_slots[static_cast<std::size_t>(SlotId::kCount)];
    std::uint64_t m_rng{};
};

/// Which surface skin a surface class asks for. kMetal, kWater and kBody have no
/// slot of their own yet and land on the nearest thing that exists.
[[nodiscard]] SlotId SurfaceSlot(SurfaceClass surface);

/// MATERIAL_ID to surface class. 90 materials collapse onto six, which is still
/// twice what vanilla's ragdoll path manages. Anything unrecognised - including
/// the ten engine IDs with no MATT record at all, of which Trap turned up in a
/// capture - lands on kSoft.
[[nodiscard]] SurfaceClass SurfaceFromMaterial(std::uint32_t materialId);

/// When there is no material: the collision layer is the reliable input
/// (07 §8). DeadBip and Biped are bodies, Ground is terrain, Static is world.
[[nodiscard]] SurfaceClass SurfaceFromLayer(ColLayer layer);

}  // namespace rds
