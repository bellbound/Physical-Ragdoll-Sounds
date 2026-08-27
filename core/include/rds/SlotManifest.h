#pragma once

// The sound bank, declared rather than hardcoded.
//
// Each slot says what it is, how long it should be, how many variants it expects,
// what family of layer it belongs to, and what to play instead when it has no
// file. Resolution walks the fallbacks, so a missing file is a quieter mod rather
// than a broken one - which is what lets `grunt_impact`, `scream_big` and the four
// armour skins sit declared and unfilled until somebody records them.
//
// A slot with no files and none expected resolves to nothing at all, silently.
// That is the whole mechanism behind "additive".
//
// First taste is 12: imp_transient x3, imp_body x3, imp_sub x2, limb_tap x3,
// scrape_loop. Build the sub layer first; without it none of this will feel like
// the references.

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
    kImpBody,           ///< 150-250 ms, +10-30 ms. Low-mid flesh and mass. The torso's, and the head's
    kImpSub,            ///< 250-400 ms, +55-75 ms. Pitched boom ~150 Hz -> 30 Hz. The loudest layer

    // surface skins - short, layered on the composite.
    //
    // Thirteen slots for thirteen classes, but only these three have files: the
    // rest ship with `expectedVariants = 0` and a `fallback`, so an install with
    // nothing recorded for ice plays the stone skin. Drop `surf_ice_01.wav` into
    // the pack and it starts playing.
    kSurfWood,   ///< 120-200 ms. Hollow knock
    kSurfStone,  ///< 100-160 ms. Hard, short
    kSurfSoft,   ///< 150-250 ms. Dull. The default for anything unresolved
    kSurfMetal,        ///< falls back to stone - the hard half, without the ring
    kSurfWater,        ///< falls back to soft
    kSurfBody,         ///< falls back to soft
    kSurfDirt,         ///< falls back to soft
    kSurfGravel,       ///< falls back to soft
    kSurfSnow,         ///< falls back to soft
    kSurfIce,          ///< falls back to stone
    kSurfGlass,        ///< falls back to stone
    kSurfWaterPuddle,  ///< falls back to water, and through it to soft
    kSurfBone,         ///< falls back to body, and through it to soft

    // armour skins - the second colour axis, built like the first. All four ship
    // empty, so an install with no armour files sounds like one without the
    // feature. See SlotDesc::expectedVariants.
    kArmorBare,   ///< 80-200 ms. Flat skin slap. Nothing equipped
    kArmorCloth,  ///< 100-250 ms. Soft cloth thump. The default, and anything unresolved
    kArmorLight,  ///< 100-250 ms. Leather creak with a buckle jingle
    kArmorHeavy,  ///< 120-300 ms. Plate rattle. Metallic, short, no pitched ring

    // grains and texture
    kLimbTap,     ///< 40-100 ms. Burst filler. Quiet, dry, heavily pitch-scattered
    kCrunchGran,  ///< 250-400 ms. Dense granular crackle in the low-mid. The head's, and the fallback for the other two
    kSpineCrunch, ///< 250-400 ms. The column going. Falls back to kCrunchGran
    kLimbCrunch,  ///< 200-350 ms. One bone out on a limb. Falls back to kCrunchGran
    kGoreWet,     ///< 200-400 ms. Squelch. Every part shares this one
    kScrapeGrain, ///< 60-180 ms. One catch. The irregularity a real slide is made of

    // loops
    kScrapeLoop,  ///< 1.5-3 s. Low-tilted grinding rumble with grain. NOT a hiss
    kScrapeBodyWood,   ///< the same grind on boards. Falls back to kScrapeLoop
    kScrapeBodyStone,  ///< and on flagstone
    kScrapeLimb,       ///< 1.5-3 s. Light, dry, small contact patch. One dragging foot
    kScrapeLimbWood,   ///< falls back to kScrapeLimb
    kScrapeLimbStone,
    kAirWhoosh,   ///< 1-2 s. Low airy movement

    // accents
    kHeadImpact,  ///< 300-500 ms. Dull skull thud, granular edge, slight ring
    kSettleRest,  ///< 200-400 ms. Soft final flop. Closes the event

    // declared but unfilled - fallback resolution skips them silently, so adding
    // voice later is a config change and not a code change
    kGruntImpact,  ///< 300-600 ms
    kScreamBig,    ///< 800-1500 ms

    // Appended rather than filed with `kImpBody` where it belongs, because a
    // slot's *number* is an input to `StableHash`: inserting a value in the middle
    // renumbers every slot below it and silently re-rolls the variant of every cue
    // in the mod.
    //
    // **New slots go here, at the end.** The manifest table below is indexed by
    // this enum and must be in the same order.
    kImpBodyLimb,  ///< 120-200 ms. `imp_body` out on a limb: drier, tighter, less mass. Falls back to it

    /// 1.5-3 s. The garment under a falling body: fabric and armour shifting as the
    /// limbs thrash and the body turns over.
    ///
    /// **One slot, not four.** A slot can carry *conditional variants* - a file
    /// tagged `heavy` wins over the plain files when the actor is in plate and is
    /// invisible when they are not - so the four classes are four placements on
    /// this slot. `expectedVariants = 0`, so an install with nothing recorded
    /// resolves to nothing and the loop never starts.
    kClothRustle,

    /// 1.5-3 s. The mass under a slide: the floor being loaded by a body moving
    /// across it, with none of the grit that rides on top.
    ///
    /// Measured against GTA 4's slide events, ours were 35-45 dB out on the
    /// bass-to-hiss balance and in the opposite direction. Grain rates match - the
    /// spectrum was the problem - and no EQ rescues a file with nothing under the
    /// shelf to boost.
    ///
    /// So it is a layer rather than a better grind file. Two attempts at "both in
    /// one file" failed in opposite directions, and the halves want different
    /// things from the runtime: the grit's pitch tracks speed because friction
    /// transit rate does, and the bed's must not, because pitching bass down at a
    /// crawl is flubby.
    ///
    /// **No surface variants, deliberately** - boards and flagstone change the
    /// grit, and mass sounds the same under any floor. **No fallback either**: one
    /// to `scrape_loop` would play the grind twice, so an install with nothing
    /// recorded resolves to nothing and the slide is what it was before this slot.
    kScrapeLoopRumble,

    kCount
};

/// What kind of layer a slot is. This was a `role` string nothing could switch on,
/// so every question about a slot's kind was a hand-written switch somewhere else
/// and the four of them had to agree by discipline.
enum class SlotFamily : std::uint8_t {
    kImpact = 0,  ///< the composite's own layers: transient, body, sub
    kSurface,     ///< what it hit
    kArmor,       ///< what it was wearing
    kGrain,       ///< texture and damage - taps, crunches, gore, scrape grain
    kLoop,        ///< sustained: the grinds and the whoosh
    kAccent,      ///< a one-off on top of a composite
    kVoice,       ///< declared and unfilled
};

[[nodiscard]] std::string_view ToString(SlotFamily family);

struct SlotDesc {
    SlotId id{};
    std::string_view name;  ///< "imp_sub" - the filename stem and the ini key
    SlotFamily family{};
    std::uint8_t expectedVariants{};
    float minLengthMs{};
    float maxLengthMs{};
    bool isLoop{};
    std::string_view character;  ///< the asset brief, one line

    /// What to play when this slot has no file of its own.
    ///
    /// This is what makes a surface variant a *file drop*: `scrape_body_stone` is
    /// declared, resolves to `scrape_loop` until somebody records one, and starts
    /// sounding like flagstone the moment they do.
    ///
    /// It is also the only thing between an unrecorded slot and silence - nothing
    /// is synthesised, so a slot with no file and no fallback does not sound.
    ///
    /// `kCount` for the end of a line, which is almost all of them, and the default
    /// so a manifest row that says nothing does not fall back to slot zero.
    SlotId fallback{SlotId::kCount};

    /// Whose mute silences this slot, and whose per-file trim scales it.
    ///
    /// `kCount` - the default - means "its own". A slot that names another is a
    /// *variant* of it: `scrape_body_stone` is the same layer on a different floor,
    /// so it answers to `scrape_loop`'s mute and trim.
    ///
    /// Two fields rather than one because they genuinely differ: the three crunches
    /// share a mute (silencing damage is one decision) and have a trim each (three
    /// recordings at three levels).
    SlotId mutesWith{SlotId::kCount};
    SlotId trimsWith{SlotId::kCount};
};

/// The slot whose mute governs `id`, following `mutesWith` to its end. `id`
/// itself when it owns its mute.
[[nodiscard]] SlotId MuteOwner(SlotId id);

/// The slot whose per-file trim governs `id`. See `MuteOwner`.
[[nodiscard]] SlotId TrimOwner(SlotId id);

[[nodiscard]] std::span<const SlotDesc> Slots();
[[nodiscard]] const SlotDesc& Slot(SlotId id);
[[nodiscard]] std::string_view ToString(SlotId id);

/// A concrete sound the renderer can play: one wav on disk, always a real
/// recording. The procedural stand-in that used to fill an empty pack is gone: a
/// mod that invents a sound when it cannot find one is a mod whose mix you cannot
/// trust, since the tuning is then against something that will never ship.
struct ResolvedSound {
    SlotId slot{};
    std::uint8_t variant{};
    std::string path;
    /// The wav's own length. A renderer sizes its mix buffer from this, so a wrong
    /// value truncates the tail rather than merely mislabelling it. `pitch` is
    /// already in it: a file corrected to 1.09x plays 8% shorter.
    float lengthMs{};

    // ── the library's corrections, carried through to Stage 5 ────────────────
    //
    // Copied off the `SfxEntry` when the bank is filled from an assignment, so the
    // engine never holds the library and `Emit` never looks anything up. Identities
    // by default: a wav found by the `<slot>_<NN>.wav` convention has no metadata
    // to read them from.
    float pitch{1.0f};   ///< multiplies the pitch the engine chose. See SfxEntry::pitch
    float trimDb{};      ///< level only, added at Emit with the other trims
};

/// Which files exist, and what to play when they do not.
class SoundBank {
public:
    /// Scan `directory` for `<slot name>_<NN>.wav`. Logs what it found and what
    /// fell back once at load: "imp_sub: 0/2 files, nothing to play" is the line
    /// that explains why somebody's mod sounds thin.
    void Load(const std::string& directory);

    /// Fill from an explicit slot-to-file table instead of from filenames - what
    /// RagdollSounds_SFX.ini drives, and the only way a slot can play something not
    /// named after it. A slot the table leaves empty falls back to the convention
    /// scan of `fallbackDirectory`, so an ini that only reassigns `imp_body` leaves
    /// the other twenty-eight files where they were.
    ///
    /// Variant index is position in the list, not filename order: the list is the
    /// user's stated order.
    void LoadAssigned(const SfxLibrary& library, const SfxAssignments& assignments,
                      const std::string& fallbackDirectory);

    /// Whether this slot's sound is a sustained texture the engine repeats.
    /// The manifest's own answer unless an assignment overrode it.
    [[nodiscard]] bool IsLooping(SlotId slot) const;

    /// Pick a variant for this slot along the axes. False when nothing along the
    /// fallback chain has a recording; callers skip the layer silently.
    ///
    /// Selection is a shuffle bag, not random: random repeats immediately, and
    /// immediate repeats are what people notice.
    ///
    /// `token` identifies the contact this cue came from. Non-zero with
    /// `SlotResolutionConfig::stableVariants` on, the variant is derived from it
    /// rather than from the bag's position, so a config change re-rolls only the
    /// cues it altered. Zero keeps the bag.
    [[nodiscard]] bool Resolve(SlotId slot, SurfaceClass surface, Coverage coverage, LimbSite site,
                               ResolvedSound& out, std::uint32_t token = 0);

    /// The exact sound a cue names. A pure lookup that does not touch the shuffle
    /// bag, so a renderer can turn a cue's (slot, variant) back into the file the
    /// engine chose; calling Resolve again would advance the bag and hand back a
    /// different variant.
    ///
    /// False where the slot has no variant at that index. `Get` does not walk the
    /// fallback chain, so ask it about the slot a cue carries.
    [[nodiscard]] bool Get(SlotId slot, std::uint8_t variant, ResolvedSound& out) const;

    /// Deterministic given the seed, so an A/B between two configs compares the
    /// configs and not two dice rolls.
    void Seed(std::uint32_t seed);

    [[nodiscard]] std::size_t FileCount(SlotId slot) const;

    /// Whether this slot can produce anything at all: a file of its own, or a
    /// fallback that has one. `Resolve` answers the same question but advances the
    /// shuffle bag, so a caller that only wants to know whether to bother would
    /// change which variant the next real cue plays.
    [[nodiscard]] bool HasSound(SlotId slot) const;

    /// Whose files `slot` actually plays: itself when it has any, the end of its
    /// declared fallback chain when it has none. The same walk `Resolve` makes,
    /// minus the picking. "0 files, nothing to play" and "0 files, plays
    /// scrape_limb" are different states of the pack, and only one needs a
    /// recording.
    [[nodiscard]] SlotId PlaysAs(SlotId slot) const;

    // ── overrides ────────────────────────────────────────────────────────────
    //
    // Two levers over what a slot picks: pin one variant so every cue plays it, and
    // suspend variants so no cue does. Both are cleared by a load, because a
    // variant index is a position in a list a load can renumber.
    //
    //  - A **pin** is a way to listen and nothing else; it dies with the process.
    //  - A **mute** is a decision about the pack. `LoadAssigned` sets it from
    //    `SlotAssignment::muted`, saved in RagdollSounds_SFX.ini and read by the
    //    game, so a file muted in the testbench is one the mod does not play.

    /// No variant: what ForcedVariant says when a slot is resolving normally.
    static constexpr std::uint8_t kNoVariant = 0xFF;

    /// Pin `variant` on this slot, or kNoVariant to let it resolve again. A forced
    /// variant beats a mute on the same file: pinning something you muted is asking
    /// to hear it.
    void ForceVariant(SlotId slot, std::uint8_t variant);
    [[nodiscard]] std::uint8_t ForcedVariant(SlotId slot) const;

    /// Suspend one variant. The slot carries on with what is left; every variant
    /// suspended goes silent rather than playing a stand-in.
    void MuteVariant(SlotId slot, std::uint8_t variant, bool muted);
    [[nodiscard]] bool VariantMuted(SlotId slot, std::uint8_t variant) const;

    /// Drop every force and every mute, on every slot. Called at the top of
    /// LoadAssigned, which then puts the assignment's own mutes back.
    void ClearOverrides();

private:
    /// Clear every slot, then fill from `<slot name>_<NN>.wav` under `directory`.
    /// `onlyEmptySlots` is how LoadAssigned falls back for slots the ini did not
    /// name.
    void ScanDirectory(const std::string& directory, bool onlyEmptySlots);

    /// The one "imp_sub: 0/2 files, nothing to play" pass, run at the end of
    /// either load path so both report the bank the same way.
    void LogContents(const char* source);

    struct SlotFiles {
        std::vector<ResolvedSound> variants;
        std::vector<std::uint8_t> bag;
        std::size_t bagCursor{};
        bool looping{};
        /// True when this slot's files came from the filename convention. Only
        /// those get re-sorted by path - the ini's order is the user's.
        bool scannedByName{true};
        /// The last variant this slot played, so the stable picker can avoid an
        /// immediate repeat - the one thing the shuffle bag was there for. 0xFF
        /// means nothing has played yet.
        std::uint8_t lastVariant{0xFF};
        /// The pinned variant, or kNoVariant. See "session overrides" above.
        std::uint8_t forced{0xFF};
        /// One flag per variant, or empty while nothing here is suspended.
        std::vector<std::uint8_t> muted;
        /// One condition per variant, parallel to `variants`. Empty while every file
        /// on this slot is unconditional, which is the shipping case.
        std::vector<VariantCondition> conditions;
        /// How many of those say anything, so the ordinary path is one compare.
        std::size_t conditionCount{};
        /// How many of those are set, so the ordinary path costs one compare.
        std::size_t mutedCount{};
    };
    SlotFiles m_slots[static_cast<std::size_t>(SlotId::kCount)];
    std::uint64_t m_rng{};
    /// The seed as given, kept apart from the running state so the stable picker
    /// has something that does not advance.
    std::uint32_t m_seed{1};
    bool m_stableVariants{true};
    bool m_conditions{true};
    bool m_surfaceConditions{true};
    bool m_armorConditions{true};

public:
    /// Whether Resolve honours a token. Off, it always uses the shuffle bag.
    void SetStableVariants(bool on) { m_stableVariants = on; }

    /// Whether Resolve narrows on conditions at all, and on which halves. Three
    /// switches rather than one, because turning off one half while the other stays
    /// is the only clean way to hear which half of a condition did the work.
    void SetConditions(bool on, bool surface, bool armor) {
        m_conditions = on;
        m_surfaceConditions = surface;
        m_armorConditions = armor;
    }
};

/// Which surface skin a surface class asks for. Total: thirteen classes, thirteen
/// slots. Whether a slot has a *file* is the separate question - ten of the
/// thirteen ship with `expectedVariants = 0` and play their parent's skin.
[[nodiscard]] SlotId SurfaceSlot(SurfaceClass surface);

/// The inverse: which class owns this slot, or kCount if it is not a surface
/// skin at all. What the mute and trim lookups use to find a class's config.
[[nodiscard]] SurfaceClass SurfaceOfSlot(SlotId slot);

/// Which body layer a contact site asks for: the torso's, or the limb variant. The
/// head takes the torso's - it has the mass, and what makes a faceplant read as
/// one is `head_impact` on top. Binned through `DamageSiteFor`.
[[nodiscard]] SlotId BodySlot(LimbSite site);

/// Which armour skin a coverage class asks for. Four classes, four slots - though
/// all four ship with `expectedVariants = 0`, so whether anything is heard is a
/// different question.
[[nodiscard]] SlotId ArmorSlot(Coverage coverage);

/// The surface-coloured variant of a scrape loop, or `base` where there is none.
/// Unlike the impact skins, a loop's surface is a different *file* - a grind on
/// stone is not a grind on carpet with a knock added - so this picks the slot
/// rather than adding one, and `Resolve` walks the fallback from there.
[[nodiscard]] SlotId ScrapeSurfaceSlot(SlotId base, SurfaceClass surface);

/// MATERIAL_ID to surface class. 90 materials collapse onto six, twice what
/// vanilla's ragdoll path manages. Anything unrecognised - including the ten engine
/// IDs with no MATT record, of which Trap turned up in a capture - lands on kSoft.
[[nodiscard]] SurfaceClass SurfaceFromMaterial(std::uint32_t materialId);

/// When there is no material: the collision layer is the reliable input
/// (07 §8). DeadBip and Biped are bodies, Ground is terrain, Static is world.
[[nodiscard]] SurfaceClass SurfaceFromLayer(ColLayer layer);

}  // namespace rds
