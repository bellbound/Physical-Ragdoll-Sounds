#pragma once

// The sound bank, declared rather than hardcoded.
//
// Each slot says what it is, how long it should be, how many variants it
// expects, what family of layer it belongs to, and what to play instead when it
// has no file. Resolution walks the fallbacks, so a missing file is a quieter
// mod rather than a broken one - which is what lets us ship twelve files and
// grow without touching code, and what lets `grunt_impact`, `scream_big` and
// the four armour skins sit declared and unfilled until somebody records them.
//
// A slot with no files and none expected resolves to nothing at all, silently.
// That is the whole mechanism behind "additive": the armour feature is four
// declared slots and an install without the assets is byte-identical to an
// install without the feature.
//
// First taste is 12: imp_transient x3, imp_body x3, imp_sub x2, limb_tap x3,
// scrape_loop. Build the sub layer in the first pass; without it none of this
// will feel like the references.

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
    // nothing recorded for ice plays the stone skin and sounds exactly like it
    // did before the class existed. Drop `surf_ice_01.wav` into the pack and it
    // starts playing. Same door the armour skins sit behind.
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

    // armour skins - the second colour axis, built exactly like the first.
    // All four ship empty, so an install with no armour files sounds like an
    // install without the feature. See SlotDesc::expectedVariants.
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

    // Appended rather than filed with `kImpBody` where it belongs, and that is
    // worth a line because it looks like carelessness. A slot's *number* is an
    // input to `StableHash`, which is what decides which variant of a slot a
    // contact plays - so inserting a value in the middle renumbers every slot
    // below it and silently re-rolls the variant of every cue in the mod. The
    // whole point of deriving the draw from the contact was that adding a layer
    // leaves the rest of a take bit-identical, and an enum that is grouped
    // prettily gives that up.
    //
    // **New slots go here, at the end.** The manifest table below is indexed by
    // this enum and must be in the same order, so its row is at the end too, and
    // the character line is what says where it belongs.
    kImpBodyLimb,  ///< 120-200 ms. `imp_body` out on a limb: drier, tighter, less mass. Falls back to it

    /// 1.5-3 s. The garment under a falling body: fabric and armour shifting as
    /// the limbs thrash and the body turns over.
    ///
    /// **One slot, not four.** Cloth, leather and mail are genuinely different
    /// spectra, but a slot can now carry *conditional variants* - a file tagged
    /// `heavy` wins over the plain files when the actor is in plate and is
    /// invisible when they are not - so the four classes are four placements on
    /// this one slot rather than four slots. `SoundBank::Resolve` already takes
    /// the `Coverage`; this is the first loop to have it mean something.
    ///
    /// `expectedVariants = 0`, so an install with nothing recorded resolves to
    /// nothing and the loop never starts. The same door the armour skins sit
    /// behind.
    kClothRustle,

    /// 1.5-3 s. The mass under a slide: the floor being loaded by a body moving
    /// across it, with none of the grit that rides on top of it.
    ///
    /// **The one thing generation reliably cannot give us and a script reliably
    /// can.** Measured against GTA 4's slide events, ours were 35-45 dB out on
    /// the bass-to-hiss balance and in the opposite direction: theirs are
    /// bass-led with the sub band loudest and a hard rolloff over 8 kHz, ours
    /// broadband noise flat to 20 kHz with the sub 40 dB down. Grain rates match
    /// - density was never the problem, the spectrum was - and no EQ rescues a
    /// file with nothing under the shelf to boost.
    ///
    /// So it is a layer rather than a better grind file. Two attempts at "both
    /// in one file" have failed in opposite directions (the old `scrape_loop` was
    /// rumble with no grit, the limb exports grit with no rumble), and the two
    /// halves want different things from the runtime: the grit's pitch tracks
    /// speed because friction transit rate does, and the bed's must not, because
    /// floor resonance does not move with speed and pitching bass down at a crawl
    /// is flubby, blooms on a sub and vanishes on a laptop speaker.
    ///
    /// **No surface variants, deliberately.** Boards and flagstone change the
    /// grit; mass sounds the same under any floor. That is what keeps this at one
    /// file while `scrape_loop` grows six.
    ///
    /// **No fallback either**, which is the other deliberate gap: a bed that fell
    /// back to `scrape_loop` would play the grind twice. `expectedVariants = 1`
    /// and no fallback, so an install with nothing recorded resolves to nothing,
    /// the bed voice never starts, and the slide is exactly what it was before
    /// this slot existed.
    kScrapeLoopRumble,

    kCount
};

/// What kind of layer a slot is.
///
/// This was a `role` string, which said the same thing in a form nothing could
/// switch on - so every question about a slot's kind was answered by a
/// hand-written switch somewhere else, and the four of them had to agree by
/// discipline. The role trim is derived from this now, and adding a slot no
/// longer means remembering which of them wanted a new arm.
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

    /// What to play when this slot has no file of its own, or `id` for the slots
    /// that are the end of the line.
    ///
    /// This is what makes a surface variant a *file drop*: `scrape_body_stone`
    /// is declared, resolves to `scrape_loop` until somebody records one, and
    /// starts sounding like flagstone the moment they do - with no code change
    /// and no ini edit.
    ///
    /// It is also the only thing standing between an unrecorded slot and
    /// silence. Nothing is synthesised any more: a sound this mod makes is a
    /// sound somebody recorded, so a slot with no file and no fallback does not
    /// sound at all.
    ///
    /// `kCount` for the slots that are the end of the line, which is almost all
    /// of them - and it is the default so a row in the manifest that says
    /// nothing about falling back does not accidentally fall back to slot zero.
    SlotId fallback{SlotId::kCount};

    /// Whose mute silences this slot, and whose per-file trim scales it.
    ///
    /// `kCount` - the default - means "its own", which is the ordinary case. A
    /// slot that names another is a *variant* of it: `scrape_body_stone` is the
    /// same layer on a different floor, so it answers to `scrape_loop`'s mute
    /// and `scrape_loop`'s trim rather than carrying a second pair of controls
    /// over one thing. That fact used to be written out four times, once in each
    /// switch that needed it, and the four had to be kept in agreement by hand.
    ///
    /// Kept as two fields rather than one because they genuinely differ: the
    /// three crunches share a mute (silencing damage is one decision) and have a
    /// trim each (three separate recordings arrive at three different levels).
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

/// A concrete sound the renderer can play: one wav on disk.
///
/// Always a real recording. There was a procedural stand-in here once, built
/// from the slot's brief so an empty pack still made a noise; it is gone,
/// because a mod that invents a sound when it cannot find one is a mod whose
/// mix you cannot trust - the tuning is then against something that will never
/// ship. A slot with nothing recorded resolves to its fallback, and a slot with
/// no fallback either does not resolve at all and its layer is skipped.
struct ResolvedSound {
    SlotId slot{};
    std::uint8_t variant{};
    std::string path;
    /// The wav's own length. A renderer sizes its mix buffer from this, so a
    /// wrong value truncates the tail rather than merely mislabelling it.
    ///
    /// `pitch` is already in it: a file corrected to 1.09x plays 8 % shorter and
    /// a buffer sized off the container length would leave a tail nothing fills.
    float lengthMs{};

    // ── the library's corrections, carried through to Stage 5 ────────────────
    //
    // Copied off the `SfxEntry` when the bank is filled from an assignment, so
    // the engine never has to hold the library and `Emit` never has to look
    // anything up. Both are identities by default, and the by-name fallback scan
    // leaves them that way: corrections belong to a library file, and a wav
    // found by the `<slot>_<NN>.wav` convention in the pack has no metadata to
    // read them from.
    float pitch{1.0f};   ///< multiplies the pitch the engine chose. See SfxEntry::pitch
    float trimDb{};      ///< level only, added at Emit with the other trims
};

/// Which files exist, and what to play when they do not.
class SoundBank {
public:
    /// Scan `directory` for `<slot name>_<NN>.wav`. Logs at info what it found
    /// and what fell back, once, at load: "imp_sub: 0/2 files, nothing to play"
    /// is exactly the line that explains why somebody's mod sounds thin.
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

    /// Pick a variant for this slot along the axes. False when nothing along the
    /// slot's fallback chain has a recording - the declared-and-unfilled voice
    /// slots, and anything else nobody has recorded yet. Callers skip the layer
    /// silently, which is the only honest answer once nothing is synthesised.
    ///
    /// Selection is a shuffle bag, not random: random repeats immediately, and
    /// immediate repeats are what people notice.
    /// `token` identifies the contact this cue came from. Non-zero with
    /// `SlotResolutionConfig::stableVariants` on, the variant is derived from it
    /// instead of from the shuffle bag's position, so a config change re-rolls
    /// only the cues it actually altered. Zero keeps the bag.
    [[nodiscard]] bool Resolve(SlotId slot, SurfaceClass surface, Coverage coverage, LimbSite site,
                               ResolvedSound& out, std::uint32_t token = 0);

    /// The exact sound a cue names. Unlike Resolve, this is a pure lookup - it
    /// does not touch the shuffle bag - so a renderer can turn a cue's
    /// (slot, variant) back into the file the engine actually chose. Calling
    /// Resolve again instead would advance the bag and hand back a *different*
    /// variant, so the audio would not be the cue list the arbitrator emitted.
    ///
    /// False where the slot has no variant at that index, which now includes
    /// every slot with no recording of its own - `Get` does not walk the
    /// fallback chain, so ask it about the slot a cue carries.
    [[nodiscard]] bool Get(SlotId slot, std::uint8_t variant, ResolvedSound& out) const;

    /// Deterministic given the seed, so an A/B between two configs compares the
    /// configs and not two dice rolls.
    void Seed(std::uint32_t seed);

    [[nodiscard]] std::size_t FileCount(SlotId slot) const;

    /// Whether this slot can produce anything at all: a file of its own, or a
    /// fallback that has one.
    ///
    /// `Resolve` answers the same question, but it is the wrong place to ask it
    /// from - it advances the shuffle bag, and a caller that only wants to know
    /// whether to bother would be changing which variant the next real cue
    /// plays. This is const and free of side effects, so a strategy can decide
    /// not to propose a layer that could never be heard.
    [[nodiscard]] bool HasSound(SlotId slot) const;

    /// Whose files `slot` actually plays: itself when it has any, and the end of
    /// its declared fallback chain when it has none.
    ///
    /// The same walk `Resolve` makes before it picks, minus the picking - so
    /// anything that only wants to *say* what a slot plays can ask without
    /// advancing the shuffle bag. "0 files, nothing to play" and "0 files, plays
    /// scrape_limb" are different states of the pack and only one of them needs
    /// a recording; a label that cannot tell them apart sends somebody looking
    /// for a bug in a fallback that is working exactly as declared.
    [[nodiscard]] SlotId PlaysAs(SlotId slot) const;

    // ── overrides ────────────────────────────────────────────────────────────
    //
    // Two levers over what a slot picks: pin one variant so every cue plays it,
    // and suspend variants so no cue does. Both are cleared by a load, because a
    // variant index is a position in a list a load can renumber - whatever set
    // one owns putting it back.
    //
    // They differ in who sets them and how long they last.
    //
    //  - A **pin** is a way to listen and nothing else. Nothing reads or writes
    //    it, the plugin never sets it, and it dies with the process.
    //  - A **mute** is a decision about the pack. `LoadAssigned` sets it from
    //    `SlotAssignment::muted`, which is saved in RagdollSounds_SFX.ini and
    //    read by the game, so a file muted in the testbench is a file the mod
    //    does not play. The setter is still here for anything that wants a mute
    //    that outlives no load at all.

    /// No variant: what ForcedVariant says when a slot is resolving normally.
    static constexpr std::uint8_t kNoVariant = 0xFF;

    /// Pin `variant` on this slot, or kNoVariant to let it resolve again. A
    /// forced variant beats a mute on the same file: pinning something you have
    /// muted is asking to hear it.
    void ForceVariant(SlotId slot, std::uint8_t variant);
    [[nodiscard]] std::uint8_t ForcedVariant(SlotId slot) const;

    /// Suspend one variant. The slot carries on with what is left, and a slot
    /// with every variant suspended goes silent rather than playing a stand-in.
    void MuteVariant(SlotId slot, std::uint8_t variant, bool muted);
    [[nodiscard]] bool VariantMuted(SlotId slot, std::uint8_t variant) const;

    /// Drop every force and every mute, on every slot. Called at the top of
    /// LoadAssigned, which then puts the assignment's own mutes back.
    void ClearOverrides();

private:
    /// Clear every slot, then fill from `<slot name>_<NN>.wav` under
    /// `directory`. `only` limits it to the slots that are still empty, which
    /// is how LoadAssigned falls back for the slots the ini did not name.
    void ScanDirectory(const std::string& directory, bool onlyEmptySlots);

    /// The one "imp_sub: 0/2 files, nothing to play" pass, run at the end of
    /// either load path so both report the bank the same way.
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
        /// The last variant this slot played, so the stable picker can still
        /// avoid an immediate repeat - which is the one thing the shuffle bag
        /// was there for. 0xFF means nothing has played yet.
        std::uint8_t lastVariant{0xFF};
        /// The pinned variant, or kNoVariant. See "session overrides" above.
        std::uint8_t forced{0xFF};
        /// One flag per variant, or empty while nothing here is suspended.
        std::vector<std::uint8_t> muted;
        /// One condition per variant, parallel to `variants`. Empty while every
        /// file on this slot is unconditional, which is the shipping case and
        /// the one that costs nothing to check.
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

    /// Whether Resolve narrows on conditions at all, and on which halves.
    ///
    /// Three switches rather than one because "is the stone-specific set
    /// actually earning its files?" is the question this feature will be asked
    /// most often, and turning off one half while the other stays is the only
    /// clean way to hear which half of a condition did the work.
    void SetConditions(bool on, bool surface, bool armor) {
        m_conditions = on;
        m_surfaceConditions = surface;
        m_armorConditions = armor;
    }
};

/// Which surface skin a surface class asks for.
///
/// Total: thirteen classes, thirteen slots. Whether a slot has a *file* is the
/// separate question, and the one that decides what is heard - ten of the
/// thirteen ship with `expectedVariants = 0` and a `fallback`, so they play
/// their parent's skin until somebody records one.
[[nodiscard]] SlotId SurfaceSlot(SurfaceClass surface);

/// The inverse: which class owns this slot, or kCount if it is not a surface
/// skin at all. What the mute and trim lookups use to find a class's config.
[[nodiscard]] SurfaceClass SurfaceOfSlot(SlotId slot);

/// Which body layer a contact site asks for: the torso's, or the limb variant.
///
/// The head takes the torso's - it has the mass to sound like it, and the thing
/// that makes a faceplant read as one is `head_impact` on top. Binned through
/// `DamageSiteFor` so there is one site->part mapping in the engine and not two.
[[nodiscard]] SlotId BodySlot(LimbSite site);

/// Which armour skin a coverage class asks for.
///
/// Total, unlike `SurfaceSlot`'s collapse of six classes onto three: there are
/// exactly four coverage classes and each has a slot. Whether that slot has a
/// *file* is a different question, and the one that decides whether anything is
/// heard - all four ship with `expectedVariants = 0`.
[[nodiscard]] SlotId ArmorSlot(Coverage coverage);

/// The surface-coloured variant of a scrape loop, or `base` where there is none.
///
/// Unlike the impact skins - which are an extra layer stacked on the composite -
/// a loop's surface is a different *file*, because a grind on stone is not a
/// grind on carpet with a knock added to it. So this picks the slot rather than
/// adding one, and `SoundBank::Resolve` walks the declared fallback from there,
/// which is what makes an unrecorded surface play the default instead of
/// nothing.
[[nodiscard]] SlotId ScrapeSurfaceSlot(SlotId base, SurfaceClass surface);

/// MATERIAL_ID to surface class. 90 materials collapse onto six, which is still
/// twice what vanilla's ragdoll path manages. Anything unrecognised - including
/// the ten engine IDs with no MATT record at all, of which Trap turned up in a
/// capture - lands on kSoft.
[[nodiscard]] SurfaceClass SurfaceFromMaterial(std::uint32_t materialId);

/// When there is no material: the collision layer is the reliable input
/// (07 §8). DeadBip and Biped are bodies, Ground is terrain, Static is world.
[[nodiscard]] SurfaceClass SurfaceFromLayer(ColLayer layer);

}  // namespace rds
