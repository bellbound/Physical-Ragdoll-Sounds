#include "rds/ConfigSchema.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <format>

namespace rds {
namespace {

// One row per field. `offsetof` on a nested member is what lets the engine read
// `cfg.arb.rateCapMs` while the file, the clamp, the delta log and the slider
// panel all go through one table - and it is why Config.h's structs have to stay
// standard-layout.
//
// The default is read out of a value-initialised config rather than repeated
// here, so Config.h stays the single place a default is written down and the two
// can never drift apart.
#define RDS_PARAM(ROOT, SECTION, KEY, TYPE, MEMBER, MINV, MAXV, STEP, GROUP, LABEL, TOOLTIP) \
    ParamDesc {                                                                              \
        SECTION, KEY, ParamType::TYPE, offsetof(ROOT, MEMBER),                               \
            static_cast<double>(ROOT{}.MEMBER), MINV, MAXV, STEP, GROUP, LABEL, TOOLTIP, {}  \
    }

/// The same row, drawn on the same line in the panel as the row after it - a
/// ramp's two ends, a gain and its trim, a phase's trim and its grain count. Two
/// columns is all the panel will do, so this is never set on two rows running.
///
/// A macro of its own rather than a wrapper around RDS_PARAM, and named to the
/// same width on purpose: the table is read in columns, and a wrapper would move
/// every continuation line of the row it marks.
#define RDS_PAIRS(ROOT, SECTION, KEY, TYPE, MEMBER, MINV, MAXV, STEP, GROUP, LABEL, TOOLTIP)  \
    ParamDesc {                                                                               \
        SECTION, KEY, ParamType::TYPE, offsetof(ROOT, MEMBER),                                \
            static_cast<double>(ROOT{}.MEMBER), MINV, MAXV, STEP, GROUP, LABEL, TOOLTIP, {},  \
            0, {}, {}, true                                                                   \
    }

/// The same row again, with a rule drawn above it: it opens a feature of its
/// own inside a group that holds several - the second `bEnabled` in a drawer
/// and everything under it, a block of trims that answers a different question
/// from the block above. The header says what the drawer is; the rule says
/// where one thing inside it stops and the next begins.
///
/// Named to the same width as RDS_PARAM for the reason RDS_PAIRS is: marking a
/// row is retyping nine characters, and nothing below it moves. Never put one
/// on the right-hand half of a pair - there is no line to break there - which
/// is what RDS_HPAIR is for: a rule above a row that also opens a pair.
#define RDS_HRULE(ROOT, SECTION, KEY, TYPE, MEMBER, MINV, MAXV, STEP, GROUP, LABEL, TOOLTIP)  \
    ParamDesc {                                                                               \
        SECTION, KEY, ParamType::TYPE, offsetof(ROOT, MEMBER),                                \
            static_cast<double>(ROOT{}.MEMBER), MINV, MAXV, STEP, GROUP, LABEL, TOOLTIP, {},  \
            0, {}, {}, false, true                                                            \
    }

#define RDS_HPAIR(ROOT, SECTION, KEY, TYPE, MEMBER, MINV, MAXV, STEP, GROUP, LABEL, TOOLTIP)  \
    ParamDesc {                                                                               \
        SECTION, KEY, ParamType::TYPE, offsetof(ROOT, MEMBER),                                \
            static_cast<double>(ROOT{}.MEMBER), MINV, MAXV, STEP, GROUP, LABEL, TOOLTIP, {},  \
            0, {}, {}, true, true                                                             \
    }

/// Same idea for a char array. There is no default to read out of a
/// value-initialised config - it is always the empty string - and no range, so
/// the numeric columns carry the buffer size instead of a min and a max.
#define RDS_PARAM_STR(ROOT, SECTION, KEY, MEMBER, GROUP, LABEL, TOOLTIP)                        ParamDesc {                                                                                     SECTION, KEY, ParamType::kString, offsetof(ROOT, MEMBER), 0, 0, 0, 0, GROUP, LABEL,             TOOLTIP, {}, sizeof(ROOT{}.MEMBER)                                                  }

/// Where this row used to live, so an ini written before it moved still loads.
/// A wrapper rather than a twelfth macro argument, because a handful of rows in
/// nine hundred have ever moved: `Renamed(RDS_PARAM(...), "HeadImpact", "fLeadClearMs")`.
///
/// Chains, innermost first, for a row that has moved twice: the name it had
/// before this call is pushed into the second slot rather than being overwritten,
/// so `Renamed(Renamed(row, "Phase", ...), "Motion", ...)` reads in the order the
/// moves happened and both old files still load. Two deep is all there is room
/// for, which is one more than anything has needed.
[[nodiscard]] constexpr ParamDesc Renamed(ParamDesc p, std::string_view section,
                                          std::string_view key) {
    p.legacySection2 = p.legacySection;
    p.legacyKey2 = p.legacyKey;
    p.legacySection = section;
    p.legacyKey = key;
    return p;
}

constexpr std::string_view kLogLevelNames[] = {"trace", "debug", "info", "warn", "error", "off"};

const ParamDesc kGeneralParams[] = {
    RDS_PARAM(GeneralConfig, "General", "bEnabled", kBool, enabled, 0, 1, 1, "General", "Enabled",
              "Master switch. Off means the plugin loads, says so in the log and hooks nothing, so "
              "the mod can be disabled without uninstalling it."),
    ParamDesc{"General", "iLogLevel", ParamType::kEnum, offsetof(GeneralConfig, logLevel),
              static_cast<double>(GeneralConfig{}.logLevel), 0, 5, 1, "General", "Log level",
              "info is the shipping level and is meant to be liberal - it should be enough to tell "
              "whether the mod ran and whether it heard the knockdown. debug is the per-contact "
              "firehose and will fill a log fast.",
              std::span<const std::string_view>{kLogLevelNames}},
    RDS_PARAM(GeneralConfig, "General", "bEnableLogRotation", kBool, enableLogRotation, 0, 1, 1,
              "General", "Rotate logs",
              "Rotate the log on startup instead of truncating it, so the log from the session "
              "that crashed survives the next launch."),
    RDS_PARAM(GeneralConfig, "General", "iMaxLogFiles", kInt, maxLogFiles, 0, 64, 1, "General",
              "Logs kept", "How many rotated logs to keep. 0 keeps all of them."),
    RDS_PARAM(GeneralConfig, "Suppression", "bSuppressVanillaBodyImpacts", kBool,
              suppression.suppressVanillaBodyImpacts, 0, 1, 1, "Suppression", "Suppress vanilla",
              "Silences vanilla's own ragdoll body impacts so ours are the whole mix, wherever we "
              "are the ones playing the collision - an actor ragdolling, or in the get-up blend. "
              "Turn it off and every contact doubles, with half of it playing vanilla's dirt "
              "sample whatever the actual surface was."),
    RDS_PARAM(GeneralConfig, "Suppression", "fSuppressionRadius", kFloat,
              suppression.suppressionRadius, 0, 1000, 5, "Suppression", "Suppression radius",
              "How far from a downed actor one of vanilla's body impacts still counts as theirs, "
              "in world units. The impact tells us where it happened and not who it was, so this "
              "is what matches the two up. Too small and part of a sprawled body doubles; too "
              "large and an NPC standing over the body loses their own. 150 is a little over two "
              "metres."),
    RDS_PARAM(GeneralConfig, "Audio", "iOutputModelFormID", kInt, audio.outputModelFormId, 0,
              2147483647, 1, "Audio", "Output model",
              "The sound output model every voice is opened with, in decimal. This is what gives a "
              "sound its distance falloff, its reverb send and its VR spatialisation; without one "
              "everything plays flat and follows you around the room. We add no reverb of our own, "
              "so this record's send is the whole of how wet we sound. 522252 (0x0007F80C) is "
              "SOMMono02000 - 150 to 2000 units at 30% send - and is the default; 930379 "
              "(0x000E324B) is the same range at 85%, which is what vanilla's own heavy body "
              "impacts use and is a long tail on a composite."),
    RDS_PARAM(GeneralConfig, "Audio", "iTapOutputModelFormID", kInt, audio.tapOutputModelFormId, 0,
              2147483647, 1, "Audio", "Tap output model",
              "The model taps are opened with instead, or 0 to give them the same one as "
              "everything else. Reverb send belongs to the record rather than to the voice, so "
              "this is the only way to say 'drier than an impact': 522252 (0x0007F80C) is "
              "SOMMono02000 at 30% send and 930379 (0x000E324B) is the same distance range wet at "
              "85%, which is the dry/wet pair vanilla's own light and heavy body impacts use. "
              "It applies to a composite and not to a layer, so a tap's surface and armour colour "
              "follow the tap. 0 while the impacts are on the dry model themselves - there is "
              "nothing left for it to say until they move."),
    RDS_PARAM(GeneralConfig, "Audio", "iTestCueKey", kInt, audio.testCueKey, 0, 255, 1, "Audio",
              "Test cue key",
              "DirectInput scancode that plays one canned impact where you stand, for checking the "
              "mod can make a sound at all without needing to knock anybody over. 0 is off. 88 is "
              "F12."),
    RDS_PARAM(GeneralConfig, "Devbench", "bEnableDevbench", kBool, devbench.enabled, 0, 1, 1,
              "Devbench", "Enable devbench link",
              "The testbench link. Off in a shipping install and deliberately so: it opens a "
              "loopback socket, streams every contact out of the process, and lets another program "
              "replace the algorithm config and the sound assignments while the game is running. "
              "With it off not a single socket call is made, and with it on a testbench that is "
              "not running is simply never found - the failure is always silent."),
    RDS_PARAM(GeneralConfig, "Devbench", "iDevbenchPort", kInt, devbench.port, 1, 65535, 1,
              "Devbench", "Devbench port",
              "Loopback TCP port the testbench listens on. Only ever 127.0.0.1 - nothing here is "
              "reachable from another machine. Change it only if something else on this box "
              "already wants 27860."),
    RDS_PARAM_STR(GeneralConfig, "Devbench", "sObsPath", devbench.obsPath, "Devbench", "OBS path",
                  "Where OBS Studio lives, so the testbench can start it when it is not already "
                  "running. The game never launches anything; this key is here because the "
                  "testbench reads the same file and there is nowhere else for it to be true. "
                  "Empty means only drive an OBS that is already up."),
    RDS_PARAM(GeneralConfig, "Benchmark", "bEnabled", kBool, benchmark.enabled, 0, 1, 1,
              "Benchmark", "Measure frame cost",
              "Time what the mod costs the frame it runs on, over the first few knockdowns of the "
              "session, and write the result to the log. It measures the three things the "
              "testbench's own benchmark cannot: publishing the ragdoll's limbs, the engine tick, "
              "and handing cues to the game's sound manager. Off in a shipping install - with it "
              "off no clock is read at all - and it reports once and then stops."),
    RDS_PARAM(GeneralConfig, "Benchmark", "iKnockdowns", kInt, benchmark.knockdowns, 1, 64, 1,
              "Benchmark", "Knockdowns measured",
              "How many knockdowns to measure before reporting. Only frames inside a knockdown are "
              "sampled - from the frame after the first body goes down to the frame the last one "
              "is released. Idle frames are left out because the engine early-outs on them, and "
              "averaging thousands of those in would report a number that flatters the mod. What "
              "comes out is the cost of a busy frame, which is the only one that can miss a "
              "deadline."),
    RDS_PARAM(GeneralConfig, "Benchmark", "iMaxFrames", kInt, benchmark.maxFrames, 100, 1000000, 100,
              "Benchmark", "Frame ceiling",
              "A ceiling on frames sampled, so a ragdoll that never lets go cannot hold the report "
              "for ever. If it is hit the report still comes, and says it was cut short. 20000 is "
              "about three and a half minutes of continuous ragdoll at 90 Hz."),
    RDS_PARAM(GeneralConfig, "Benchmark", "fSlowFrameUs", kFloat, benchmark.slowFrameUs, 10, 20000,
              10, "Benchmark", "Slow frame threshold",
              "Frames costing more than this many microseconds are counted separately, and that "
              "count is the line worth reading - a mean says what the mod costs and this says "
              "whether it ever spiked. 250 us is a fortieth of a 90 Hz frame: far above anything "
              "measured, far below anything anyone could feel."),
};

const ParamDesc kAlgorithmParams[] = {
    // -- Game integration -----------------------------------------------------
    RDS_PARAM(AlgorithmConfig, "GameIntegration", "bAnimatedMode", kBool, game.animatedMode, 0, 1,
              1, "Game integration", "Animated mode",
              "Bypasses every ragdoll check in the mod, so an actor is heard while it is walking "
              "around and not only while it is on the floor.\n"
              "The ragdoll bodies collide the whole time an actor is animated, so this is a great "
              "deal more sound than a knockdown: footfalls, a hand brushing a hip, a shoulder in a "
              "doorway. It is an experiment - everything downstream was tuned against falls - and "
              "with it off none of the four rows below can reach anything."),
    RDS_PARAM(AlgorithmConfig, "GameIntegration", "bAnimatedSlide", kBool, game.animatedSlide, 0, 1,
              1, "Game integration", "Slides while animated",
              "Whether the sliding grind may run on an actor who is on their feet. A dragging foot "
              "is what opens one, and a grind is a loop: it plays until the body leaves the slide "
              "state rather than for one frame, so a false slide is a sound that stays."),
    RDS_PAIRS(AlgorithmConfig, "GameIntegration", "bAnimatedRustle", kBool, game.animatedRustle, 0,
              1, 1, "Game integration", "Rustle while animated",
              "Whether clothes rustle on an actor who is on their feet. The one layer that "
              "arguably belongs in ordinary gameplay - a body walking is a body whose clothes are "
              "moving - and the reason the pose is still measured for an animated actor."),
    RDS_PARAM(AlgorithmConfig, "GameIntegration", "bAnimatedAirTime", kBool, game.animatedAirTime,
              0, 1, 1, "Game integration", "Air time while animated",
              "Whether a jump or a drop off a ledge counts as flight when it never knocks the "
              "actor down. Off, an animated body reads as having been on the ground the whole "
              "time and the rules that pay out for a long fall pay nothing."),
    RDS_PARAM(AlgorithmConfig, "GameIntegration", "fAnimatedIdleReleaseMs", kFloat,
              game.animatedIdleReleaseMs, 250, 30000, 50, "Game integration", "Idle release",
              "How long an animated actor may go quiet before the engine lets go of them. A "
              "knockdown ends when the actor gets up; walking has no such edge, so without this "
              "an actor is held from their first footstep until they leave the cull radius, with "
              "a burst budget and a hero count that ran out minutes ago."),

    // -- Stage 0: Ingest ------------------------------------------------------
    RDS_PARAM(AlgorithmConfig, "Ingest", "fMinImpactSpeed", kFloat, ingest.minImpactSpeed, 0, 400,
              1, "Ingest", "Minimum impact speed",
              "The quietest contact allowed to make any sound at all. Raise it and the small taps "
              "that fill out a tumble disappear; lower it and the settle at the end of a fall "
              "starts chattering."),
    RDS_PAIRS(AlgorithmConfig, "Ingest", "fSlideFloorFrac", kFloat, ingest.slideFloorFrac, 0, 1.0,
              0.01, "Ingest", "Slide floor relief",
              "How far the floor above comes down for a contact that is sliding rather than "
              "arriving.\n"
              "1.0 is off. The floor measures closing speed, and a body lying flat and skidding "
              "has almost none - so a clean slide is the thing it throws away most reliably, and "
              "the grind fades out as the slide gets purer. Lower this and those contacts are "
              "let in; too low and the settle at the end of a fall starts chattering again."),
    RDS_PARAM(AlgorithmConfig, "Ingest", "fSlideFloorAtTangent", kFloat,
              ingest.slideFloorAtTangent, 0, 2000, 10, "Ingest", "Slide floor full at",
              "Tangent speed at which the relief above is fully applied. It ramps in from the "
              "minimum impact speed, so a contact barely moving sideways keeps the full floor."),
    RDS_HRULE(AlgorithmConfig, "Ingest", "fBlowupDisagreeFrac", kFloat, ingest.blowupDisagreeFrac,
              0.01, 1.0, 0.01, "Ingest", "Blow-up disagreement",
              "How far the solver's closing speed and our own reconstruction of it may differ "
              "before the contact is treated as a physics explosion and thrown away. Lower is "
              "stricter: too low and real hard landings go silent."),
    RDS_PARAM(AlgorithmConfig, "Ingest", "fBlowupSpeedCeiling", kFloat, ingest.blowupSpeedCeiling,
              100, 5000, 10, "Ingest", "Blow-up speed ceiling",
              "Backstop for contacts with no reconstruction to check against. Anything faster is "
              "discarded as impossible, so setting it low makes the hardest hits in the game the "
              "only ones that never play."),
    RDS_PARAM(AlgorithmConfig, "Ingest", "fBlowupAngularCeiling", kFloat,
              ingest.blowupAngularCeiling, 10, 2000, 5, "Ingest", "Blow-up spin ceiling",
              "The same backstop for spin. A genuine armoured landing spins a limb to 60 rad/s, so "
              "anything near that discards real impacts."),
    RDS_HRULE(AlgorithmConfig, "Ingest", "fSelfContactThreshold", kFloat,
              ingest.selfContactThreshold, 0, 2000, 10, "Ingest", "Self-contact threshold",
              "Half of all contacts are one limb touching another limb of the same body. Below "
              "this they only thicken the cloth bed; above it they play as real impacts. Drop it "
              "and a ragdoll rattles against itself constantly."),
    RDS_PARAM(AlgorithmConfig, "Ingest", "bDropMirroredSelfContacts", kBool,
              ingest.dropMirroredSelfContacts, 0, 1, 1, "Ingest", "Drop mirrored self-contacts",
              "Every limb-on-own-limb contact is reported twice, once from each limb. Off, they "
              "all play twice and the whole mix doubles at the moment of landing."),
    RDS_HRULE(AlgorithmConfig, "Ingest", "fFrameGapMs", kFloat, ingest.frameGapMs, 0.1, 20, 0.1,
              "Ingest", "Frame gap",
              "How large a gap between contact reports means a new frame started. Only matters on "
              "replay; the measured split is one microsecond inside a frame against 20 ms "
              "between."),
    RDS_PARAM(AlgorithmConfig, "Ingest", "iBodySampleEveryNTicks", kInt,
              ingest.bodySampleEveryNTicks, 0, 16, 1, "Ingest", "Body samples",
              "How often the game measures where every ragdoll limb is - 1 is every frame, 0 is "
              "never. This is the only measurement of where the body actually is; with it off, "
              "air time falls back to guessing flight from the gaps between contacts, which "
              "calls a body that has simply stopped touching anything airborne. Raise it only if "
              "a capture is getting unmanageably large."),
    RDS_PARAM(AlgorithmConfig, "Ingest", "bCollapseManifolds", kBool, ingest.collapseManifolds, 0,
              1, 1, "Ingest", "Collapse manifolds",
              "One collision surface reported at several points becomes one contact at the "
              "strongest of them. Off, a flat landing on a floor plays several times over."),

    // -- Stage 0: glancing landings -------------------------------------------
    RDS_PARAM(AlgorithmConfig, "GlancingImpact", "bEnabled", kBool, glancing.enabled, 0, 1, 1,
              "Glancing landings", "Enabled",
              "Judge a landing limb by how much of its motion went into the ground rather than "
              "past it. Off, a foot the body drops squarely onto and a foot that clips the floor "
              "on the way past sound the same at the same closing speed."),
    RDS_PAIRS(AlgorithmConfig, "GlancingImpact", "fFullTransferFrac", kFloat,
              glancing.fullTransferFrac, 0, 1, 0.01, "Glancing landings", "Square landing at",
              "At or above this share of the limb's speed going into the surface, the landing is "
              "square and nothing is taken off. Measured: a foot the body lands squarely on reads "
              "0.99, a knee taking part of a fall 0.74, a foot clipping the floor 0.58."),
    RDS_PARAM(AlgorithmConfig, "GlancingImpact", "fNoTransferFrac", kFloat, glancing.noTransferFrac,
              0, 1, 0.01, "Glancing landings", "Full glance at",
              "At or below this the reduction is at full. Between the two the ramp is linear. Set "
              "it close to the value above for a sharp switch, far below it for a gentle slope."),
    RDS_PARAM(AlgorithmConfig, "GlancingImpact", "fMaxIntensityScale", kFloat,
              glancing.maxIntensityScale, 0, 1, 0.05, "Glancing landings", "Full glance keeps",
              "What fraction of its intensity a fully glancing landing keeps. Intensity carries "
              "the event's class and its rank, so low enough and the landing drops through the "
              "composite threshold and plays as a limb tap. One leaves intensity alone and uses "
              "only the level cut below."),
    RDS_PAIRS(AlgorithmConfig, "GlancingImpact", "fMaxGainCutDb", kFloat, glancing.maxGainCutDb,
              -30, 0, 0.5, "Glancing landings", "Full glance gain cut",
              "How much level a fully glancing landing loses before arbitration - so it is both "
              "quieter and ranked lower, and gives up the hero slot to whatever lands next. It "
              "does not change the cue's class; only the intensity scale above can do that."),
    RDS_PARAM(AlgorithmConfig, "GlancingImpact", "fMaxTrimCutDb", kFloat, glancing.maxTrimCutDb,
              -30, 0, 0.5, "Glancing landings", "Full glance level cut",
              "How much level a fully glancing landing loses, applied after arbitration - so it "
              "gets quieter and nothing else about the fall moves. This is the knob for "
              "separating a clipped landing from the hit after it without disturbing the rest."),
    RDS_HPAIR(AlgorithmConfig, "GlancingImpact", "fSlideRatioStart", kFloat,
              glancing.slideRatioStart, 0, 20, 0.25, "Glancing landings", "Slide begins at",
              "Sideways speed over closing speed, above which the contact starts looking like a "
              "slide rather than a clipped landing and the reduction begins backing off. A "
              "clipped foot runs about 1.4; a body sliding along the floor runs 3 to 17."),
    RDS_PARAM(AlgorithmConfig, "GlancingImpact", "fSlideRatioFull", kFloat, glancing.slideRatioFull,
              0, 20, 0.25, "Glancing landings", "Slide certain at",
              "Above this ratio the contact is a slide and the rule leaves it alone entirely. "
              "Without it a body sliding to a halt has every contact turned into a tap, which is "
              "not what a slide sounds like."),
    RDS_HRULE(AlgorithmConfig, "GlancingImpact", "bIncludeThigh", kBool, glancing.includeThigh, 0,
              1, 1, "Glancing landings", "Judge thighs too",
              "Whether the thigh counts as a landing limb beside the foot and the calf. Off, only "
              "feet and knees are judged and a hip skidding in stays at full level."),
    RDS_PARAM(AlgorithmConfig, "GlancingImpact", "fMinBodySpeed", kFloat, glancing.minBodySpeed, 0,
              500, 10, "Glancing landings", "Minimum limb speed",
              "Below this the limb is barely moving and the angle it arrives at means nothing, so "
              "the rule leaves it alone."),
    RDS_HRULE(AlgorithmConfig, "GlancingImpact", "bScaleCrunchGate", kBool,
              glancing.scaleCrunchGate, 0, 1, 1, "Glancing landings", "Also raise the crunch gate",
              "Whether a glancing landing must arrive proportionally faster to break a bone. Off "
              "by default: a bone can break at any angle, and the crunch is already quieter "
              "because the intensity behind its level fell. Turn it on if clipped landings crunch "
              "too readily."),

    // -- Stage 2, axis one: what the body is doing ----------------------------
    //
    // Was [Phase], which carried this and the mix's own state in one value. Every
    // row below is Renamed() from there, so an ini written before the split still
    // loads and migrates itself on the first save.
    Renamed(RDS_PARAM(AlgorithmConfig, "Motion", "fAirborneMinTimeMs", kFloat,
                      motion.airborneMinTimeMs, 0, 2000, 10, "Motion", "Airborne time",
                      "How long a body has to be off the ground before it counts as airborne, so "
                      "one bounce does not read as a fall. Only used on a take with no pose data - "
                      "with pose, the free-fall threshold below answers this properly."),
            "Phase", "fAirborneMinTimeMs"),
    Renamed(RDS_PARAM(AlgorithmConfig, "Motion", "fFreeFallFrac", kFloat, motion.freeFallFrac, 0.05,
                      1.5, 0.05, "Motion", "Free-fall threshold",
                      "How hard the body has to be accelerating downward before nothing is "
                      "considered to be holding it up, as a share of gravity. This is what tells a "
                      "real fall from a body that has merely stopped touching anything - a shove "
                      "pushes the body UP, and the old rule counted that as flying. Lower it and "
                      "scuffs start reading as falls; raise it and only clean drops count. Needs a "
                      "take with pose data."),
            "Phase", "fFreeFallFrac"),
    Renamed(RDS_PARAM(AlgorithmConfig, "Motion", "fGravityUnitsPerSec2", kFloat,
                      motion.gravityUnitsPerSec2, 100, 2000, 10, "Motion", "Gravity",
                      "Downward acceleration of an unsupported body, in game units. 686 is "
                      "9.8 m/s^2 at 69.99 units to the metre, and a real fall in the capture set "
                      "measures 675. Only worth moving if a mod changes the world's gravity."),
            "Phase", "fGravityUnitsPerSec2"),
    Renamed(RDS_PARAM(AlgorithmConfig, "Motion", "fFreeFallMinMs", kFloat, motion.freeFallMinMs, 0,
                      500, 5, "Motion", "Free-fall settle",
                      "How long the fall has to look like a fall before it is believed. Stops a "
                      "single noisy solver frame opening a flight."),
            "Phase", "fFreeFallMinMs"),
    Renamed(RDS_PARAM(AlgorithmConfig, "Motion", "fFreeFallHoldMs", kFloat, motion.freeFallHoldMs,
                      0, 500, 5, "Motion", "Free-fall hold",
                      "And how long it survives a frame that disagrees, so a body clipping "
                      "something on the way down does not land and take off a dozen times. It is "
                      "also what keeps the flight readable on the frame the body lands, which is "
                      "the frame the hero rule's arrival test needs it."),
            "Phase", "fFreeFallHoldMs"),
    RDS_PARAM(AlgorithmConfig, "Motion", "fLandedHoldMs", kFloat, motion.landedHoldMs, 0, 500, 5,
              "Motion", "Landed hold",
              "How long nothing may have touched the body before a tumble is allowed to become a "
              "flight again. The free-fall flag above is a latch on the acceleration and a body "
              "bouncing along a floor keeps it topped up, so without this the state alternated "
              "Tumble/Airborne every tick and put a 6 dB square wave on both loops. 0 is the old "
              "edge."),
    RDS_PARAM(AlgorithmConfig, "Motion", "fBedTrimGlideMs", kFloat, motion.bedTrimGlideMs, 0, 500,
              5, "Motion budgets", "Bed trim glide",
              "How fast the motion trim below arrives on the beds - the two loops, the armour "
              "skins and the settle cue. A loop is one voice held open across a state change, so a "
              "3 or 6 dB step lands on it as a click. Impacts are unaffected: an event takes the "
              "trim of the state it happened in. 0 is the old step."),
    RDS_HRULE(AlgorithmConfig, "Motion", "bDrivenDetect", kBool, motion.drivenEnabled, 0, 1, 1,
              "Motion", "Detect driven flight",
              "Whether a body something is pushing - a leash hauling on a collar, a shout, a "
              "blast, a spell that throws the target - is told apart from one that is only "
              "falling. On, air time is measured from the last frame something was pushing, so "
              "the rules that pay out for a long fall stop paying for a long pull. Off, the "
              "residual is still measured and reported so a gate can be chosen, but nothing acts "
              "on it and a powered flight reads as a fall exactly as it used to."),
    RDS_PARAM(AlgorithmConfig, "Motion", "fDrivenResidual", kFloat, motion.drivenResidual, 0, 3000,
              25, "Motion", "Driven-force gate",
              "How far the body's measured acceleration may sit from gravity before the flight "
              "counts as driven rather than falling. A leash hauling on a collar, a shout, a "
              "blast and a spell that throws the target all read the same way here, which is the "
              "point - the engine never has to know which mod did it. Air time is measured from "
              "the last driven frame, so a body that is being pulled has not been falling. Has "
              "no effect unless bDrivenDetect is on."),
    RDS_PARAM(AlgorithmConfig, "Motion", "fDrivenHoldMs", kFloat, motion.drivenHoldMs, 0, 500, 5,
              "Motion", "Driven hold",
              "How long a flight stays driven after the last frame that said so, so one quiet "
              "frame in the middle of a sustained pull does not hand the flight back."),
    RDS_PARAM(AlgorithmConfig, "Motion", "fLandingWindowMs", kFloat, motion.landingWindowMs, 0, 500,
              5, "Motion", "Landing window",
              "How long after a flight ends a contact still counts as its landing. The air-time "
              "rules ask about a fall that is over, and the flight flag has usually cleared by the "
              "time the arrival is judged - so this is what lets a landing be paid for the fall it "
              "ended instead of reading zero. Too long and a body lying still is still being paid "
              "for the fall that put it there."),
    Renamed(RDS_PAIRS(AlgorithmConfig, "Motion", "fLaunchTrimDb", kFloat, motion.launch.gainTrimDb,
                      -60, 12, 0.5, "Motion budgets", "Launch trim",
                      "How loud the moment of being knocked over is, before anything has landed."),
            "Phase", "fLaunchTrimDb"),
    Renamed(RDS_PARAM(AlgorithmConfig, "Motion", "iLaunchMaxCues", kInt,
                      motion.launch.maxCuesPerBurst, 0, 16, 1, "Motion budgets", "Launch grains",
                      "How many sounds the launch may spend at once."),
            "Phase", "iLaunchMaxCues"),
    Renamed(RDS_PAIRS(AlgorithmConfig, "Motion", "fAirborneTrimDb", kFloat,
                      motion.airborne.gainTrimDb, -60, 12, 0.5, "Motion budgets", "Airborne trim",
                      "How loud anything is while the body is in the air. Should be well down: the "
                      "airborne section is anticipation, not event."),
            "Phase", "fAirborneTrimDb"),
    Renamed(RDS_PARAM(AlgorithmConfig, "Motion", "iAirborneMaxCues", kInt,
                      motion.airborne.maxCuesPerBurst, 0, 16, 1, "Motion budgets",
                      "Airborne grains", "How many sounds may play while airborne."),
            "Phase", "iAirborneMaxCues"),
    Renamed(RDS_PAIRS(AlgorithmConfig, "Motion", "fTumbleTrimDb", kFloat, motion.tumble.gainTrimDb,
                      -60, 12, 0.5, "Motion budgets", "Tumble trim",
                      "How loud the rolling section after the first landing is."),
            "Phase", "fTumbleTrimDb"),
    Renamed(RDS_PARAM(AlgorithmConfig, "Motion", "iTumbleMaxCues", kInt,
                      motion.tumble.maxCuesPerBurst, 0, 16, 1, "Motion budgets", "Tumble grains",
                      "How many grains a tumble burst may contain."),
            "Phase", "iTumbleMaxCues"),
    // -- Stage 2, axis two: what the mix is doing -----------------------------
    RDS_PARAM(AlgorithmConfig, "Hero", "bEnabled", kBool, hero.enabled, 0, 1, 1, "Hero",
              "Hero moments",
              "Whether a fall is allowed a hero moment at all. Off, every budget comes from what "
              "the body is doing and a knockdown reads as a flat tumble throughout."),
    RDS_PARAM(AlgorithmConfig, "Hero", "fFloorFrac", kFloat, hero.floorFrac, 0.0, 1.5, 0.01, "Hero",
              "Hero floor",
              "How hard a contact has to be before it can be a hero moment at all, as a share of "
              "the loudest thing the mod hears. This is the gate that stops an opening scuff "
              "stealing the moment from the landing forty contacts later. Raise it and only real "
              "slams qualify; drop it to nothing and the first touch of every fall is the hero."),
    RDS_PARAM(AlgorithmConfig, "Hero", "fDominanceRatio", kFloat, hero.dominanceRatio, 1.0, 4.0,
              0.05, "Hero", "Dominance",
              "How much louder than the last few hundred milliseconds a contact has to be to count "
              "as the event rather than as part of one. 1.0 makes every contact over the floor a "
              "hero; high values mean only a contact that dwarfs everything around it qualifies."),
    RDS_HPAIR(AlgorithmConfig, "Hero", "fArrivalMinAirMs", kFloat, hero.arrivalMinAirMs, 0, 3000,
              10, "Hero", "Arrival flight",
              "The other way in: a contact that lands out of a genuinely measured flight this long "
              "is a landing whatever else is going on, however quiet the contacts around it were. "
              "Needs a take with pose data - on an old take this clause is simply off, because the "
              "old guess reads the first contact of a take as maximally airborne."),
    RDS_PARAM(AlgorithmConfig, "Hero", "fArrivalMinDropUnits", kFloat, hero.arrivalMinDropUnits, 0,
              4000, 10, "Hero", "Arrival drop",
              "And how far it fell, in units, if you want short hops excluded. 70 units is about a "
              "metre. Zero judges on flight time alone."),
    Renamed(RDS_HRULE(AlgorithmConfig, "Hero", "fWindowMs", kFloat, hero.windowMs, 20, 2000, 10,
                      "Hero", "Hero window",
                      "How long the hero moment stays open for its peers. A faceplant genuinely "
                      "has a knee, a chest and a head arriving inside a couple of hundred "
                      "milliseconds; shorten this and only the first of them is loud."),
            "Phase", "fPrimaryImpactWindowMs"),
    RDS_PARAM(AlgorithmConfig, "Hero", "fReanchorRatio", kFloat, hero.reanchorRatio, 1.0, 4.0, 0.05,
              "Hero", "Re-anchor",
              "How much bigger than the moment's own peak a later contact has to be to take the "
              "moment over - restarting the window, refilling the budget and moving the point "
              "every layer collapses to. This is what makes a landing one event that tracks its "
              "loudest grain, instead of one event followed by two it had no budget left for."),
    Renamed(RDS_HPAIR(AlgorithmConfig, "Hero", "fTrimDb", kFloat, hero.budget.gainTrimDb, -60, 12,
                      0.5, "Hero", "Hero trim",
                      "The hero moment's own trim. This is the reference everything else is "
                      "quieter than, so moving it moves the whole knockdown."),
            "Phase", "fPrimaryImpactTrimDb"),
    Renamed(RDS_PARAM(AlgorithmConfig, "Hero", "iMaxCues", kInt, hero.budget.maxCuesPerBurst, 0, 16,
                      1, "Hero", "Hero grains",
                      "How many grains the hero burst may contain. The references measure three to "
                      "five."),
            "Phase", "iPrimaryImpactMaxCues"),
    RDS_HRULE(AlgorithmConfig, "Hero", "bResetsBurst", kBool, hero.resetsBurst, 0, 1, 1, "Hero",
              "Reset the burst",
              "Whether opening a hero moment closes whatever burst was already running and starts "
              "its own with the grain count back at zero. On, because the thing a long fall most "
              "often loses to is not a gate but a burst that some scuff opened and filled while "
              "the body was still in the air."),
    RDS_PAIRS(AlgorithmConfig, "Hero", "fBurstGapFrac", kFloat, hero.burstGapFrac, 0, 1, 0.05,
              "Hero", "Waive the gap",
              "How much of the enforced silence between bursts a hero moment is excused, so the "
              "landing is not dropped for arriving too soon after the burst it just closed. 1.0 "
              "waives it entirely, 0 waives nothing."),
    RDS_PARAM(AlgorithmConfig, "Hero", "fBurstWindowMs", kFloat, hero.burstWindowMs, 0, 2000, 10,
              "Hero", "Hero burst window",
              "How long a burst stays open while a moment is running. 0 uses the ordinary burst "
              "window, which is what happened before this existed.\n\n"
              "The two want opposite things and that is the whole reason this is a second number. "
              "An ordinary burst wants a wide window - contacts arrive when the solver reports "
              "them, and a window shorter than a fall's own spread shuts before the loud one "
              "lands, which is how two taps at 0.03 and 0.07 intensity spend a burst and lock out "
              "the 465 u/s hit seventy milliseconds behind it. A hero burst wants a tight one: "
              "the grains of a landing are already together because the limbs arrive together, so "
              "the window's job there is not to wait for them but to shut before whatever comes "
              "next joins in and smears the crash into a roll.\n\n"
              "Three or four grains inside 20-40 ms and then several hundred milliseconds of "
              "nothing is the reference rhythm - the same picture `Waive the rate cap` is drawn "
              "from."),
    RDS_PARAM(AlgorithmConfig, "Hero", "fRateCapFrac", kFloat, hero.rateCapFrac, 0, 1.0, 0.05,
              "Hero", "Waive the rate cap",
              "How much of the minimum spacing between onsets is waived inside a hero moment, so "
              "its peers cluster instead of being spaced like ordinary contacts. This is the shape "
              "of a landing: evenly spaced onsets read as a metronome, three or four inside 20-40 "
              "ms then a long gap reads as a crash. Too near 1.0 and simultaneous onsets stop "
              "clustering and start summing - the peak doubles and the mix is no denser."),
    RDS_PARAM(AlgorithmConfig, "Hero", "iMaxPerEvent", kInt, hero.maxPerEvent, 0, 16, 1, "Hero",
              "Heroes per fall",
              "How many hero moments one knockdown may have. 0 is unlimited, which is the point: a "
              "body bouncing down a staircase has more than one real landing in it. Set it to 1 "
              "for the old behaviour of one hero per fall."),

    // -- Stage 4: Arbitration -------------------------------------------------
    RDS_PARAM(AlgorithmConfig, "Arbitration", "fRateCapMs", kFloat, arb.rateCapMs, 5, 500, 1,
              "Arbitration", "Rate cap",
              "The shortest gap allowed between any two impact onsets, whatever limb they came "
              "from. Below about 46 ms extra onsets stop adding detail and start adding mud; well "
              "above it a knockdown starts to sound sparse and deliberate."),
    RDS_PARAM(AlgorithmConfig, "Arbitration", "fRateCapOverrideDb", kFloat, arb.rateCapOverrideDb,
              0, 60, 0.5, "Arbitration", "Rate cap override",
              "How much louder than the impact holding the rate cap a new one must be to get "
              "through anyway. Lower lets a busy crash keep more of its hits; raise it towards 60 "
              "to switch the override off and let the cap always win."),
    RDS_HRULE(AlgorithmConfig, "Arbitration", "fChainMergeWindowMs", kFloat, arb.chainMergeWindowMs,
              0, 500, 5, "Arbitration", "Chain merge window",
              "How long one limb chain stays claimed by its strongest contact. This is what turns "
              "a hand, a wrist and an elbow landing together into one arm flop."),
    RDS_HRULE(AlgorithmConfig, "Arbitration", "fMaskDropBelowDb", kFloat, arb.maskDropBelowDb, 1,
              60, 0.5, "Arbitration", "Masking depth",
              "How far under the loudest recent sound a proposal has to be before it is dropped "
              "outright rather than played quietly. Small values let everything through and the "
              "landing turns to porridge; large values leave only the heroes."),
    RDS_PARAM(AlgorithmConfig, "Arbitration", "fMaskDecayDbPerSec", kFloat, arb.maskDecayDbPerSec,
              1, 400, 5, "Arbitration", "Masking recovery",
              "How fast the ear is assumed to recover after a loud hit. Slower means a big landing "
              "keeps the following second quiet."),
    RDS_HRULE(AlgorithmConfig, "Arbitration", "iBurstMaxGrains", kInt, arb.burstMaxGrains, 1, 16, 1,
              "Arbitration", "Grains per burst",
              "How many sounds one audible moment is built from. The references measure three to "
              "five; more reads as a machine gun rather than as an impact with texture."),
    RDS_PARAM(AlgorithmConfig, "Arbitration", "fBurstWindowMs", kFloat, arb.burstWindowMs, 20, 2000,
              10, "Arbitration", "Burst window",
              "How long one burst may last before it has to end. This plus the grain count is the "
              "shape of an audible moment."),
    RDS_PARAM(AlgorithmConfig, "Arbitration", "fBurstMinGapMs", kFloat, arb.burstMinGapMs, 0, 3000,
              10, "Arbitration", "Silence between bursts",
              "The silence enforced between audible moments. This is the single biggest lever on "
              "whether a three-second tumble reads as four events or as thirty."),
    RDS_HRULE(AlgorithmConfig, "Arbitration", "bSpatialCollapseOnHero", kBool,
              arb.spatialCollapseOnHero, 0, 1, 1, "Arbitration", "Collapse hero to one point",
              "During a hero moment, place every layer at one point. Several points read as "
              "several events; one point reads as one event with detail."),
    RDS_PARAM(AlgorithmConfig, "Arbitration", "fSpatialCollapseWindowMs", kFloat,
              arb.spatialCollapseWindowMs, 0, 1000, 10, "Arbitration", "Collapse window",
              "How long the collapse holds before the sound spreads back out across the limbs for "
              "the tumble."),
    RDS_HRULE(AlgorithmConfig, "Arbitration", "fFrameScaleK", kFloat, arb.frameScaleK, 0, 10, 0.25,
              "Arbitration", "Frame scaling",
              "How many frames every window is worth as well as its millisecond floor, so the "
              "system behaves the same at 24 fps and at 144. Zero pins every window to its floor "
              "and makes the mod frame-rate dependent."),
    RDS_HRULE(AlgorithmConfig, "Arbitration", "fTorsoWeightDb", kFloat, arb.torsoWeightDb, 0, 24,
              0.5, "Arbitration", "Torso priority",
              "Rank only, never loudness: how much of a head start a spine, COM or neck contact "
              "gets when the arbitrator decides which of a frame's contacts survives. Nobody "
              "hears this - they hear which contact was kept. At 3 dB a torso landing beside a "
              "slightly harder hand wins, and a hand 6 dB louder still wins. Zero is off."),
    RDS_PARAM(AlgorithmConfig, "Arbitration", "fHeadWeightDb", kFloat, arb.headWeightDb, 0, 24, 0.5,
              "Arbitration", "Head priority",
              "The same head start for a skull contact. Rank only. The hero test already has a "
              "partial one of these in HeadImpact:bHeroFloorRelief, so raise one or the other "
              "rather than both blind."),
    RDS_PARAM(AlgorithmConfig, "Arbitration", "fLimbWeightDb", kFloat, arb.limbWeightDb, 0, 24, 0.5,
              "Arbitration", "Limb priority",
              "The same head start for arms, legs and anything off a skeleton we could not read. "
              "Only the differences between the three matter, so leaving this at zero and "
              "raising the other two says the same thing as raising this and more."),

    // -- Intensity ------------------------------------------------------------
    RDS_PAIRS(AlgorithmConfig, "Intensity", "fSpeedRefLow", kFloat, intensity.speedRefLow, 0, 500,
              1, "Intensity", "Quiet anchor",
              "The contact speed that maps to the bottom of the loudness range. Anything at or "
              "under it is as quiet as the mod goes."),
    RDS_PARAM(AlgorithmConfig, "Intensity", "fSpeedRefHigh", kFloat, intensity.speedRefHigh, 100,
              4000, 10, "Intensity", "Loud anchor",
              "The contact speed that maps to full level. A guess, not a measurement: the take "
              "that would have established it was discarded. Lower it and ordinary knockdowns get "
              "loud; raise it and nothing ever reaches the top of the range."),
    RDS_PARAM(AlgorithmConfig, "Intensity", "fDynamicRangeDb", kFloat, intensity.dynamicRangeDb, 5,
              80, 1, "Intensity", "Dynamic range",
              "The whole span from the quietest contact to the loudest, in decibels. The "
              "references use about 35; a naive loudness curve gives 60 and sounds wrong at both "
              "ends - shouty at the top, inaudible at the bottom."),
    RDS_PAIRS(AlgorithmConfig, "Intensity", "fCurveExponent", kFloat, intensity.curveExponent, 0.1,
              4.0, 0.05, "Intensity", "Curve shape",
              "Where the loudness sits between the two anchors. Under 1 fills the middle out and "
              "makes ordinary contacts more present; over 1 keeps everything quiet until it is "
              "genuinely hard."),
    RDS_PARAM(AlgorithmConfig, "Intensity", "fSoftClipKnee", kFloat, intensity.softClipKnee, 0.1,
              1.0, 0.05, "Intensity", "Soft-clip knee",
              "Where the curve stops being straight and starts compressing, so another mod's "
              "absurd impulse gets loud rather than rejected. A silent obliterate is the worst "
              "possible outcome."),
    RDS_HPAIR(AlgorithmConfig, "Intensity", "fMassWeight", kFloat, intensity.massWeight, 0, 2, 0.05,
              "Intensity", "Mass weight",
              "How much a limb's own size makes it louder. At zero a fingertip and a torso at the "
              "same speed sound identical; high values make torso landings dominate everything."),
    RDS_PARAM(AlgorithmConfig, "Intensity", "fRadiusWeight", kFloat, intensity.radiusWeight, 0, 2,
              0.05, "Intensity", "Radius weight",
              "The same, from the limb's physical bounding radius rather than its mass. This is "
              "the half that still works on a skeleton we do not recognise."),
    RDS_HRULE(AlgorithmConfig, "Intensity", "fObliterateFrac", kFloat, intensity.obliterateFrac,
              0.2, 6, 0.02, "Intensity", "Obliterate point",
              "Where a contact stops being a fall and the gore tier opens, as a multiple of the "
              "loud anchor. Above 1 is above anything the mod treats as loud, which is where it "
              "belongs; under 1 and ordinary hard landings start coming apart."),

    // -- Intensity: the post-assignment half ----------------------------------
    RDS_PARAM(AlgorithmConfig, "PostIntensity", "fExtraRangeDb", kFloat,
              intensity.post.extraRangeDb, -30, 30, 0.5, "Intensity (after slots)",
              "Extra dynamic range",
              "Widens or narrows the gap between the quietest contact and the loudest, after every "
              "layer has been chosen. Unlike the dynamic range above it moves loudness only - the "
              "same contact is still built from the same layers at the same pitch."),
    RDS_PAIRS(AlgorithmConfig, "PostIntensity", "fCurveExponent", kFloat,
              intensity.post.curveExponent, 0.1, 4.0, 0.05, "Intensity (after slots)",
              "Curve shape",
              "Where the level sits between the two anchors, on top of the curve above. Under 1 "
              "brings ordinary contacts forward without giving them a heavier stack; over 1 holds "
              "them back without making them thinner. 1 changes nothing."),
    RDS_PARAM(AlgorithmConfig, "PostIntensity", "fSoftClipKnee", kFloat,
              intensity.post.softClipKnee, 0.1, 1.0, 0.05, "Intensity (after slots)",
              "Soft-clip knee",
              "Compresses the top of the level range so an absurd impulse gets loud rather than "
              "enormous, while still being built as the obliterate it is. 1 is off."),

    // -- Slot gains: one trim per file in the bank ----------------------------
    //
    // Applied at Stage 5 beside the layer mutes, and summed on top of the mix
    // section's per-role trims rather than replacing them: a role trim balances
    // kinds of layer against each other, and these fix the one file that came
    // back a decibel hot. Nothing here can change which cues were chosen.
    RDS_PARAM(AlgorithmConfig, "SlotGain", "fImpTransient", kFloat, slotGains.impTransient, -24,
              12, 0.5, "Slot gains", "imp_transient",
              "The bright contact click, on its own. The one to reach for when the attack reads as "
              "a tick rather than as a hit."),
    RDS_PARAM(AlgorithmConfig, "SlotGain", "fImpBody", kFloat, slotGains.impBody, -24, 12, 0.5,
              "Slot gains", "imp_body",
              "The low-mid flesh and mass. Carries most of what a body sounds like, so a decibel "
              "here is audible on every impact in the mod. The torso's, and the head's."),
    RDS_PARAM(AlgorithmConfig, "SlotGain", "fImpBodyLimb", kFloat, slotGains.impBodyLimb, -24, 12,
              0.5, "Slot gains", "imp_body_limb",
              "The same layer out on an arm or a leg. Until somebody records imp_body_limb_01.wav "
              "this trims the imp_body file the limb composite falls back to - which is the useful "
              "half of it early: it sits every limb impact back without touching the torso's."),
    RDS_PARAM(AlgorithmConfig, "SlotGain", "fImpSub", kFloat, slotGains.impSub, -24, 12, 0.5,
              "Slot gains", "imp_sub",
              "The late pitched boom - the loudest layer and the whole of the gnarl. Down a couple "
              "of decibels if the mod is booming through a soundbar."),
    RDS_PARAM(AlgorithmConfig, "SlotGain", "fLimbTap", kFloat, slotGains.limbTap, -24, 12, 0.5,
              "Slot gains", "limb_tap",
              "The burst filler. Quiet and dry by design: lift it and a tumble turns busy, drop it "
              "and the gaps between the real impacts open up."),
    RDS_PARAM(AlgorithmConfig, "SlotGain", "fCrunchGran", kFloat, slotGains.crunchGran, -24,
              12, 0.5, "Slot gains", "crunch_gran",
              "The granular bone break. Loud enough to be a fact about what happened, not so loud "
              "that it becomes the impact."),
    RDS_PARAM(AlgorithmConfig, "SlotGain", "fSpineCrunch", kFloat, slotGains.spineCrunch, -24,
              12, 0.5, "Slot gains", "spine_crunch",
              "The column going. Falls back to crunch_gran until somebody records one, and this "
              "trim is what balances the two against each other when they do."),
    RDS_PARAM(AlgorithmConfig, "SlotGain", "fLimbCrunch", kFloat, slotGains.limbCrunch, -24,
              12, 0.5, "Slot gains", "limb_crunch",
              "One bone out on a limb. The most-heard of the three by a distance, because limb "
              "contacts outnumber the other two several times over - so it is the one to pull "
              "down first if the mod starts sounding like a bag of sticks."),
    RDS_PARAM(AlgorithmConfig, "SlotGain", "fGoreWet", kFloat, slotGains.goreWet, -24, 12, 0.5,
              "Slot gains", "gore_wet",
              "The squelch, the top damage tier. It plays rarely enough that it is easy to leave "
              "far too hot without ever noticing."),
    RDS_PARAM(AlgorithmConfig, "SlotGain", "fScrapeGrain", kFloat, slotGains.scrapeGrain, -24, 12,
              0.5, "Slot gains", "scrape_grain",
              "The scuff a grind opens with. One per grind, so it is easy to leave hot without "
              "noticing - the take that shows it is a fall that grinds, launches and lands "
              "several times over."),
    RDS_PARAM(AlgorithmConfig, "SlotGain", "fScrapeLoop", kFloat, slotGains.scrapeLoop, -24,
              12, 0.5, "Slot gains", "scrape_loop",
              "The body grind, and its surface variants, which share this trim because they are "
              "the same layer on a different floor. Sits about 20 dB under the impacts; if it "
              "reads as a hiss rather than as a rumble the file is wrong, not the gain."),
    RDS_PARAM(AlgorithmConfig, "SlotGain", "fScrapeLimb", kFloat, slotGains.scrapeLimb, -24, 12,
              0.5, "Slot gains", "scrape_limb",
              "The light grind of one limb dragging, and its surface variants. Well under the "
              "body grind - a single foot should be only just audible at conversational "
              "distance."),
    RDS_PARAM(AlgorithmConfig, "SlotGain", "fScrapeLoopRumble", kFloat,
              slotGains.scrapeLoopRumble, -24, 12, 0.5, "Slot gains", "scrape_loop_rumble",
              "The bed under both grinds. Its own trim rather than the grind's, because the "
              "balance between mass and grit is the whole of the layer - and because it is a "
              "synthesised file arriving at a level nothing else in the bank shares."),
    RDS_PARAM(AlgorithmConfig, "SlotGain", "fAirWhoosh", kFloat, slotGains.airWhoosh, -24, 12, 0.5,
              "Slot gains", "air_whoosh",
              "The airborne rise. Anticipation, not event - lift it and every knockdown announces "
              "itself before it has landed."),
    RDS_PARAM(AlgorithmConfig, "SlotGain", "fHeadImpact", kFloat, slotGains.headImpact, -24,
              12, 0.5, "Slot gains", "head_impact",
              "The skull accent. Rare and unmissable is the brief, and it rides on a full "
              "composite already, so it needs less than it looks like it does."),
    RDS_PARAM(AlgorithmConfig, "SlotGain", "fSettleRest", kFloat, slotGains.settleRest, -24,
              12, 0.5, "Slot gains", "settle_rest",
              "The closing flop. Quiet enough to read as a full stop rather than as one more "
              "impact."),

    // ── Limbs: how much mass is behind a contact ─────────────────────────────
    RDS_PARAM(AlgorithmConfig, "Limbs", "bMassDeterminesLoudness", kBool,
              limbs.massDeterminesLoudness, 0, 1, 1, "Limbs",
              "Mass determines loudness instead of limb",
              "Off, how loud an impact is depends on which limb touched - and a foot is quiet even "
              "when the whole body lands on it. On, it depends on how much of the body is actually "
              "arriving, judged by how much the limb is travelling rather than swinging."),
    RDS_PARAM(AlgorithmConfig, "Limbs", "fBodyMass", kFloat, limbs.bodyMass, 0.1, 30, 0.25, "Limbs",
              "Whole-body mass",
              "How loud a landing that brings the whole body with it is, on the same scale as the "
              "limb table (a foot is 1, a thigh 7, the trunk 7.5). Every fully coupled contact uses "
              "this whatever limb it came through, which is what makes a feet-first and a hip-first "
              "landing at the same speed sound alike."),
    RDS_PARAM(AlgorithmConfig, "Limbs", "fRotationRefRatio", kFloat, limbs.rotationRefRatio, 0.05,
              8, 0.05, "Limbs", "Flail threshold",
              "How much spin counts as a limb swinging on its own rather than being carried. Lower "
              "and more contacts are treated as flails and go quiet; higher and even a whipping arm "
              "is priced as the body landing."),
    RDS_PARAM(AlgorithmConfig, "Limbs", "fCouplingWeight", kFloat, limbs.couplingWeight, 0, 1, 0.05,
              "Limbs", "Coupling strength",
              "How far towards the whole-body mass a coupled contact is allowed to go. At 0 the "
              "toggle above does nothing; at 1 a body landing is priced entirely as a body."),
    RDS_HRULE(AlgorithmConfig, "Limbs", "fMinBodySpeed", kFloat, limbs.minBodySpeed, 0, 2000, 10,
              "Limbs", "Minimum carrying speed",
              "A limb moving slower than this delivers nothing however straight its path is. Stops "
              "a body settling slowly onto the floor being priced as a landing."),

    // -- Stage 3: ImpactComposite --------------------------------------------
    RDS_PARAM(AlgorithmConfig, "ImpactComposite", "bEnabled", kBool, strategies.impact.enabled, 0,
              1, 1, "Impact composite", "Enabled",
              "The core layer stack. Off, the mod plays only accents and loops and every impact "
              "loses its body."),
    RDS_PARAM(AlgorithmConfig, "ImpactComposite", "fTapBelowIntensity", kFloat,
              strategies.impact.tapBelowIntensity, 0, 1, 0.01, "Impact composite",
              "Tap below intensity",
              "Under this intensity a contact plays as a single tap instead of a four-layer "
              "composite. It is the only place a level decision changes a cue's class, so it is "
              "the line a reduction has to cross before a landing stops being a hero moment. "
              "Raise it and more of a tumble becomes texture."),
    RDS_HPAIR(AlgorithmConfig, "ImpactComposite", "fTransientOffsetMs", kFloat,
              strategies.impact.transientOffsetMs, -50, 200, 1, "Impact composite",
              "Transient offset",
              "When the bright contact click lands. This is the reference everything else is "
              "measured from and should stay at zero."),
    RDS_PARAM(AlgorithmConfig, "ImpactComposite", "fBodyOffsetMs", kFloat,
              strategies.impact.bodyOffsetMs, -50, 400, 1, "Impact composite", "Body offset",
              "When the low-mid flesh and mass arrives. Measured at 8-34 ms in the references."),
    RDS_PAIRS(AlgorithmConfig, "ImpactComposite", "fSubOffsetMs", kFloat,
              strategies.impact.subOffsetMs, -50, 400, 1, "Impact composite", "Sub offset",
              "When the pitched boom arrives. The single most important number in the mod: at "
              "65 ms an impact reads as mass, at 0 it reads as a click with a thud stuck to it, "
              "and past 120 ms it stops belonging to the impact at all."),
    RDS_PARAM(AlgorithmConfig, "ImpactComposite", "fOffsetScatterMs", kFloat,
              strategies.impact.offsetScatterMs, 0, 50, 0.5, "Impact composite", "Offset scatter",
              "A few milliseconds of variation on each layer so no two composites have exactly the "
              "same envelope. Large values smear the shape instead of varying it."),
    RDS_HPAIR(AlgorithmConfig, "ImpactComposite", "fTransientGainAtMinDb", kFloat,
              strategies.impact.transientGainAtMinDb, -60, 12, 0.5, "Impact composite",
              "Transient at quiet",
              "How much of the stack is the bright click on the lightest contact. Light contacts "
              "are almost all transient."),
    RDS_PARAM(AlgorithmConfig, "ImpactComposite", "fTransientGainAtMaxDb", kFloat,
              strategies.impact.transientGainAtMaxDb, -60, 12, 0.5, "Impact composite",
              "Transient at loud", "And on the heaviest, where it only rides on top of the sub."),
    RDS_PAIRS(AlgorithmConfig, "ImpactComposite", "fBodyGainAtMinDb", kFloat,
              strategies.impact.bodyGainAtMinDb, -60, 12, 0.5, "Impact composite", "Body at quiet",
              "How much flesh a light contact has."),
    RDS_PARAM(AlgorithmConfig, "ImpactComposite", "fBodyGainAtMaxDb", kFloat,
              strategies.impact.bodyGainAtMaxDb, -60, 12, 0.5, "Impact composite", "Body at loud",
              "And a heavy one."),
    RDS_PAIRS(AlgorithmConfig, "ImpactComposite", "fSubGainAtMinDb", kFloat,
              strategies.impact.subGainAtMinDb, -80, 12, 0.5, "Impact composite", "Sub at quiet",
              "How much boom a light tap gets. Should be almost none, or every tap sounds like a "
              "cannon."),
    RDS_PARAM(AlgorithmConfig, "ImpactComposite", "fSubGainAtMaxDb", kFloat,
              strategies.impact.subGainAtMaxDb, -80, 12, 0.5, "Impact composite", "Sub at loud",
              "And a hard landing, where it is the loudest thing in the stack. This is where the "
              "gnarl comes from."),
    RDS_HPAIR(AlgorithmConfig, "ImpactComposite", "fPitchScatterSemis", kFloat,
              strategies.impact.pitchScatterSemis, 0, 6, 0.1, "Impact composite", "Pitch scatter",
              "Random pitch spread per voice. This is what stops thirteen files sounding like "
              "thirteen files. Past about three semitones it starts sounding like a pitch trick."),
    RDS_PARAM(AlgorithmConfig, "ImpactComposite", "fPitchIntensityBiasSemis", kFloat,
              strategies.impact.pitchIntensityBiasSemis, -12, 12, 0.25, "Impact composite",
              "Pitch bias with intensity",
              "How far down a heavy impact is pitched relative to a light one. Negative reads as "
              "heavier and bigger for free."),
    RDS_PARAM(AlgorithmConfig, "ImpactComposite", "fPitchMaxSemis", kFloat,
              strategies.impact.pitchMaxSemis, 0, 12, 0.25, "Impact composite", "Pitch limit",
              "The hard limit on the two above put together, so nothing ever sounds obviously "
              "transposed."),

    // -- Surfaces: what the floor does to a sound -----------------------------
    //
    // Gathered out of four sections that each owned a piece of it. Every moved
    // row is wrapped in Renamed(), so an ini written before the move still
    // loads and nobody loses a tuning pass to a refactor.
    RDS_PARAM(AlgorithmConfig, "Surfaces", "bEnabled", kBool, surfaces.enabled, 0, 1, 1,
              "Surfaces", "Enabled",
              "Off, nothing gets a surface skin and the mod plays the same body on marble as on "
              "moss. The fastest way to hear how much of the mix is the floor at all - the three "
              "mutes at the bottom answer the same question one surface at a time."),
    RDS_PARAM(AlgorithmConfig, "Surfaces", "fOffsetMs", kFloat, surfaces.offsetMs, -50, 300, 1,
              "Surfaces", "Offset on impacts",
              "When the wood, stone or soft colour arrives on a four-layer impact. Close to the "
              "transient, or it stops reading as the same event."),
    RDS_PAIRS(AlgorithmConfig, "Surfaces", "fGainAtMinDb", kFloat, surfaces.gainAtMinDb, -60, 12,
              0.5, "Surfaces", "Impact colour at quiet",
              "How audible the surface colour is on a light contact."),
    RDS_PARAM(AlgorithmConfig, "Surfaces", "fGainAtMaxDb", kFloat, surfaces.gainAtMaxDb, -60, 12,
              0.5, "Surfaces", "Impact colour at loud",
              "And on a heavy one. If the surface reads as a separate sound rather than as "
              "colour, it is too loud."),
    RDS_HPAIR(AlgorithmConfig, "Surfaces", "bOnTaps", kBool, surfaces.onTaps, 0, 1, 1, "Surfaces",
              "Colour taps too",
              "Give the burst filler a surface skin as well. Nine of every ten contacts are taps, "
              "so with this off a body sliding down a wooden staircase spends nine tenths of "
              "itself sounding like a fall down nothing in particular. Scuffs are where a floor "
              "is identified; the hero hits only confirm it."),
    RDS_PARAM(AlgorithmConfig, "Surfaces", "fTapOffsetMs", kFloat, surfaces.tapOffsetMs, -50, 300,
              1, "Surfaces", "Offset on taps",
              "Tighter than the impact offset on purpose. A tap is 40-100 ms of grain, so a skin "
              "arriving late would outlive what it is colouring."),
    RDS_PAIRS(AlgorithmConfig, "Surfaces", "fTapGainAtMinDb", kFloat, surfaces.tapGainAtMinDb, -60,
              12, 0.5, "Surfaces", "Tap colour at quiet",
              "How much floor a light scuff names. Low: this plays on most of the contacts in a "
              "take, so a decibel here is a decibel everywhere."),
    RDS_PARAM(AlgorithmConfig, "Surfaces", "fTapGainAtMaxDb", kFloat, surfaces.tapGainAtMaxDb, -60,
              12, 0.5, "Surfaces", "Tap colour at loud",
              "And on the hardest thing still classed as a tap - a fast graze along the floor, "
              "which is exactly where a surface should be at its most obvious."),
    RDS_PARAM(AlgorithmConfig, "Surfaces", "fTapHeadroomDb", kFloat, surfaces.tapHeadroomDb, -24,
              0, 0.5, "Surfaces", "Tap headroom",
              "How far under its own tap the colour is held, whatever the ramp above says. This "
              "is colour-not-dominate written down: 0 lets the skin tie with the grain it is "
              "colouring and there is nothing above 0. The tap is still ranked on the tap alone, "
              "so this changes what is heard rather than what is chosen."),
    Renamed(RDS_HRULE(AlgorithmConfig, "Surfaces", "fTrimDb", kFloat, surfaces.trimDb, -40, 20,
                      0.5, "Surfaces", "Surface trim",
                      "Global trim on every surface colour together. Up makes the "
                      "floor material obvious; down makes every surface sound the same, which is "
                      "what vanilla does. Each surface's own trim sums on top of this."),
            "Mix", "fSurfaceTrimDb"),

    // -- Armour: what the body was wearing ------------------------------------
    //
    // The surface section's twin, and deliberately the same shape: a colour
    // layer with an offset, a ramp, a headroom clamp, a tap half, a role trim
    // and a mute per class. Everything here is inert until `armor_*` files exist
    // - all four slots ship with no variants expected - so an install without
    // the assets sounds exactly like an install without this section.
    RDS_PARAM(AlgorithmConfig, "Armor", "bEnabled", kBool, armor.enabled, 0, 1, 1, "Armour",
              "Enabled",
              "Off, no cue gets an armour skin and the class is not even read. The fastest way to "
              "hear how much of the mix is what the body was wearing - the four mutes at the "
              "bottom answer the same question one class at a time."),

    RDS_HPAIR(AlgorithmConfig, "Armor", "bPerLimb", kBool, armor.perLimb, 0, 1, 1, "Armour",
              "Class per limb",
              "On, each contact is coloured by what that limb is wearing: heavy boots on an "
              "otherwise naked body give feet the plate rattle and everything else the skin slap. "
              "Off, every contact reads the cuirass instead and the whole body clanks together. "
              "The fast A/B for whether per-limb is worth having at all."),
    RDS_PARAM(AlgorithmConfig, "Armor", "bInheritFromBody", kBool, armor.inheritFromBody, 0, 1, 1, "Armour",
              "Empty slot takes the cuirass",
              "A site with nothing in its own armour slot reads as whatever the chest piece is, "
              "rather than as bare. Right for the common case - a cuirass with no separate "
              "gauntlets does not mean bare forearms - and wrong for heavy boots on a naked body, "
              "where you want the rest to read naked. Off, an empty slot means bare."),
    RDS_PARAM(AlgorithmConfig, "Armor", "bBareIsNaked", kBool, armor.bareIsNaked, 0, 1, 1, "Armour",
              "Nameless armour is bare skin",
              "A worn item with no name and no weight is treated as bare rather than as clothing. "
              "TNG's skin is a real armour record covering five slots, so without this a naked "
              "modded body reads as clothed on five sites and armor_bare essentially never fires. "
              "The recording loader has always applied this rule; this is the live path catching "
              "up."),
    RDS_PARAM(AlgorithmConfig, "Armor", "iActorClassSource", kInt, armor.actorClassSource, 0, 2, 1, "Armour",
              "Class for cues with no contact",
              "The airborne rise has no contact and therefore no limb to read a class off. "
              "0 uses the cuirass - what you hear moving on a body in flight - 1 uses the last "
              "contact's limb, 2 reports them as unclassified, which is the clothing class: it is "
              "defined as clothing and anything we cannot decide, so it is the value that asserts "
              "nothing."),

    RDS_HPAIR(AlgorithmConfig, "Armor", "bNoHeadOnLight", kBool, armor.noHeadOnLight, 0, 1, 1, "Armour",
              "No head accent in light armour",
              "Suppress the head accent when the head is in light armour. It takes away the "
              "accent only - the dull skull thud with the ring on it, which reads as bare skull "
              "and is wrong under a helm. The composite, both skins and the crunch all still "
              "fire, so a helmeted head landing hard is still loud; it just stops sounding like a "
              "melon."),
    RDS_PARAM(AlgorithmConfig, "Armor", "bNoHeadOnHeavy", kBool, armor.noHeadOnHeavy, 0, 1, 1, "Armour",
              "No head accent in heavy armour",
              "The same for a full helm. Independent of the light switch, because a padded coif "
              "and a steel helm are not the same argument."),

    RDS_HPAIR(AlgorithmConfig, "Armor", "fOffsetMs", kFloat, armor.offsetMs, -50, 300, 1, "Armour",
              "Offset on impacts",
              "When the rattle arrives on a four-layer impact. Between the transient and the body "
              "on purpose: metal moves because something stopped, so it is a consequence of the "
              "contact rather than the contact itself. At 0 it fuses with the transient into one "
              "brighter click, which is the failure mode to listen for."),
    RDS_PAIRS(AlgorithmConfig, "Armor", "fGainAtMinDb", kFloat, armor.gainAtMinDb, -60, 12, 0.5, "Armour",
              "Impact colour at quiet",
              "How audible the armour is on a light contact."),
    RDS_PARAM(AlgorithmConfig, "Armor", "fGainAtMaxDb", kFloat, armor.gainAtMaxDb, -60, 12, 0.5, "Armour",
              "Impact colour at loud",
              "And on a heavy one. If the armour reads as a separate sound rather than as colour, "
              "it is too loud."),
    RDS_PARAM(AlgorithmConfig, "Armor", "fHeadroomDb", kFloat, armor.headroomDb, -24, 0, 0.5, "Armour",
              "Impact headroom",
              "How far under the whole stack the rattle is held, whatever the ramp says. It "
              "matters more here than on the surface skin: armour keeps moving after the body has "
              "stopped, so this is the layer most likely to be long and loud relative to what it "
              "is colouring. The skin never enters the rank the arbitrator sorts by, so this "
              "changes what is heard rather than what is chosen."),

    RDS_HPAIR(AlgorithmConfig, "Armor", "bOnTaps", kBool, armor.onTaps, 0, 1, 1, "Armour",
              "Colour taps too",
              "Give the burst filler an armour skin as well. Nine of every ten contacts are taps, "
              "so this is most of where armour gets heard at all."),
    RDS_PARAM(AlgorithmConfig, "Armor", "fTapOffsetMs", kFloat, armor.tapOffsetMs, -50, 300, 1, "Armour",
              "Offset on taps",
              "Tighter than the impact offset, for the same reason the surface skin's is: a tap "
              "is 40-100 ms of grain and a skin arriving late outlives what it is colouring."),
    RDS_PAIRS(AlgorithmConfig, "Armor", "fTapGainAtMinDb", kFloat, armor.tapGainAtMinDb, -60, 12, 0.5, "Armour",
              "Tap colour at quiet",
              "How much armour a light scuff names. This plays on most of the contacts in a take, "
              "so a decibel here is a decibel everywhere."),
    RDS_PARAM(AlgorithmConfig, "Armor", "fTapGainAtMaxDb", kFloat, armor.tapGainAtMaxDb, -60, 12, 0.5, "Armour",
              "Tap colour at loud",
              "And on the hardest thing still classed as a tap."),
    RDS_PARAM(AlgorithmConfig, "Armor", "fTapHeadroomDb", kFloat, armor.tapHeadroomDb, -24, 0, 0.5, "Armour",
              "Tap headroom",
              "How far under its own tap the armour is held. 0 lets it tie with the grain it is "
              "colouring and there is nothing above 0."),

    RDS_HPAIR(AlgorithmConfig, "Armor", "bOnSlide", kBool, armor.onSlide, 0, 1, 1, "Armour",
              "Armour rides the slide",
              "Lay the armour skin over the scrape loop, at the class the contact that opened the "
              "slide was wearing. Plate dragged over flagstone is one of the few places this "
              "feature pays for itself with a single file."),
    RDS_PARAM(AlgorithmConfig, "Armor", "fSlideGainDb", kFloat, armor.slideGainDb, -60, 12, 0.5, "Armour",
              "Slide colour",
              "Flat rather than ramped, because a slide has no single intensity - it has a "
              "duration. The loop's own level already tracks how hard the body is grinding."),

    RDS_HPAIR(AlgorithmConfig, "Armor", "fBarePitchSemis", kFloat, armor.barePitchSemis, -6, 6, 0.1, "Armour",
              "Pitch bias, bare",
              "Pitch the whole composite for a naked limb. This costs no files at all - pitch is "
              "free and continuous in this engine - and it is the cheapest dynamic in the "
              "feature. Ships at 0 so an install with no armour assets is exactly today's mod; "
              "the voicing worth trying first is bare +0.5 against heavy -1.0, which reads as a "
              "lighter body and a heavier one with nothing recorded."),
    RDS_PARAM(AlgorithmConfig, "Armor", "fClothPitchSemis", kFloat, armor.clothPitchSemis, -6, 6, 0.1, "Armour",
              "Pitch bias, clothed",
              "The same for the default case. Usually left at 0, since it is the middle everything "
              "else is heard against."),
    RDS_PARAM(AlgorithmConfig, "Armor", "fLightPitchSemis", kFloat, armor.lightPitchSemis, -6, 6, 0.1, "Armour",
              "Pitch bias, light armour",
              "The same for leather and hide."),
    RDS_PARAM(AlgorithmConfig, "Armor", "fHeavyPitchSemis", kFloat, armor.heavyPitchSemis, -6, 6, 0.1, "Armour",
              "Pitch bias, heavy armour",
              "The same for plate. Negative reads as heavier and bigger, exactly as the "
              "composite's own intensity bias does."),

    RDS_HPAIR(AlgorithmConfig, "Armor", "fBareCompositeTrimDb", kFloat, armor.bareCompositeTrimDb, -24, 12, 0.5,
              "Armour", "Whole-stack trim, bare",
              "Trim the entire impact for a naked limb, not just the armour skin - the layer "
              "balance is untouched, the whole thing moves. Heavy armour genuinely is louder, and "
              "these four say so without changing which contact won."),
    RDS_PARAM(AlgorithmConfig, "Armor", "fClothCompositeTrimDb", kFloat, armor.clothCompositeTrimDb, -24, 12,
              0.5, "Armour", "Whole-stack trim, clothed", "The same for clothing."),
    RDS_PARAM(AlgorithmConfig, "Armor", "fLightCompositeTrimDb", kFloat, armor.lightCompositeTrimDb, -24, 12,
              0.5, "Armour", "Whole-stack trim, light armour", "The same for light armour."),
    RDS_PARAM(AlgorithmConfig, "Armor", "fHeavyCompositeTrimDb", kFloat, armor.heavyCompositeTrimDb, -24, 12,
              0.5, "Armour", "Whole-stack trim, heavy armour", "The same for plate."),

    RDS_HRULE(AlgorithmConfig, "Armor", "fTrimDb", kFloat, armor.trimDb, -40, 20, 0.5, "Armour",
              "Armour trim",
              "Global trim on all four armour skins together, the twin of the surface trim."),
    RDS_PAIRS(AlgorithmConfig, "Armor", "fBareTrimDb", kFloat, armor.bareTrimDb, -24, 12, 0.5, "Armour",
              "armor_bare",
              "The flat skin slap, alone. Four separately recorded sounds arrive at four different "
              "levels, which is why the role trim above is not enough."),
    RDS_PARAM(AlgorithmConfig, "Armor", "fClothTrimDb", kFloat, armor.clothTrimDb, -24, 12, 0.5, "Armour",
              "armor_cloth",
              "The soft cloth thump, alone. This is the class anything unresolved falls back to, "
              "so it plays far more often than the other three and is worth setting last."),
    RDS_PARAM(AlgorithmConfig, "Armor", "fLightTrimDb", kFloat, armor.lightTrimDb, -24, 12, 0.5, "Armour",
              "armor_light", "The leather creak, alone."),
    RDS_PARAM(AlgorithmConfig, "Armor", "fHeavyTrimDb", kFloat, armor.heavyTrimDb, -24, 12, 0.5, "Armour",
              "armor_heavy", "The plate rattle, alone."),

    RDS_HPAIR(AlgorithmConfig, "Armor", "bBare", kBool, armor.bare, 0, 1, 1, "Armour", "Play armor_bare",
              "The skin slap for a bare limb. Muted at render, so every arbitration decision "
              "stays identical and only the sound goes."),
    RDS_PARAM(AlgorithmConfig, "Armor", "bCloth", kBool, armor.cloth, 0, 1, 1, "Armour", "Play armor_cloth",
              "The cloth thump for the default case."),
    RDS_PARAM(AlgorithmConfig, "Armor", "bLight", kBool, armor.light, 0, 1, 1, "Armour", "Play armor_light",
              "The creak and jingle for light armour."),
    RDS_PARAM(AlgorithmConfig, "Armor", "bHeavy", kBool, armor.heavy, 0, 1, 1, "Armour", "Play armor_heavy",
              "The rattle for plate. The one most worth recording first: it is the class a "
              "generic thud is most obviously wrong for."),

    // -- Stage 3: HeadImpact --------------------------------------------------
    RDS_PARAM(AlgorithmConfig, "HeadImpact", "bEnabled", kBool, strategies.head.enabled, 0, 1, 1,
              "Head impact", "Enabled",
              "Head contacts get their own accent on top of the composite. Off, a head landing is "
              "just another limb."),
    RDS_PARAM(AlgorithmConfig, "HeadImpact", "fGateFrac", kFloat, strategies.head.gateFrac, 0, 3,
              0.01, "Head impact", "Gate point",
              "How hard a head has to hit before the accent fires, as a multiple of the loud "
              "anchor - so it stays where it is in the range when the anchor moves. Low values put "
              "a skull crack on every roll of the neck."),
    RDS_PAIRS(AlgorithmConfig, "HeadImpact", "fGainDb", kFloat, strategies.head.gainDb, -40, 12,
              0.5, "Head impact", "Accent level",
              "Before arbitration: how loud the head accent is against the stack, and therefore "
              "also how important. Pulling a hot head_impact wav down here costs the accent its "
              "place in the sort as well as its level - use the trim below for pure voicing."),
    RDS_PARAM(AlgorithmConfig, "HeadImpact", "fTrimDb", kFloat, strategies.head.trimDb, -40, 12,
              0.5, "Head impact", "Accent trim",
              "After arbitration: the same balance with no effect on the sort, the rate cap or "
              "the budgets. This is where wav voicing belongs; the slider above is where the "
              "*event* belongs."),
    RDS_PARAM(AlgorithmConfig, "HeadImpact", "fHeadDownBonus", kFloat,
              strategies.head.headDownBonus, 0, 2, 0.05, "Head impact", "Head-down bonus",
              "How much more willing the gate is when the head is already the low point of the "
              "body - a faceplant rather than a knock."),

    // -- Stage 3: HeadImpact, the company rule --------------------------------
    RDS_HRULE(AlgorithmConfig, "HeadImpact", "bCompanyEnabled", kBool,
              strategies.head.companyEnabled, 0, 1, 1, "Head impact", "Company rule",
              "Quieten a head that arrives inside a pile of other limbs. A head at the end of a "
              "spine whip is the fastest thing in the frame and the least of the collision; this "
              "is the head-specific half of the effective-mass problem."),
    RDS_PARAM(AlgorithmConfig, "HeadImpact", "iCompanyMaxPeers", kInt,
              strategies.head.companyMaxPeers, 0, 16, 1, "Head impact", "Company limit",
              "More other limbs than this hitting the world in the same frame and the head is "
              "treated as part of a sprawl. Zero damps any head that does not land alone."),
    RDS_PARAM(AlgorithmConfig, "HeadImpact", "fCompanyLeadFrac", kFloat,
              strategies.head.companyLeadFrac, 0, 5, 0.05, "Head impact", "Company lead",
              "How much faster than the fastest limb beside it the head must be to still count as "
              "the strike. Below this it is damped however few limbs are touching."),
    RDS_PARAM(AlgorithmConfig, "HeadImpact", "fCompanyGateFrac", kFloat,
              strategies.head.companyGateFrac, 0, 3, 0.05, "Head impact", "Company gate penalty",
              "How much the gate rises for a head inside a pile, as a fraction of itself."),
    RDS_PAIRS(AlgorithmConfig, "HeadImpact", "fCompanyDampDb", kFloat,
              strategies.head.companyDampDb, -40, 0, 0.5, "Head impact", "Company damping",
              "Before arbitration: how far the accent drops for a head inside a pile, and how far "
              "it drops in the sort with it - so a sprawled head stops outranking the limbs the "
              "loudness actually belongs to."),
    RDS_PARAM(AlgorithmConfig, "HeadImpact", "fCompanyTrimDb", kFloat,
              strategies.head.companyTrimDb, -40, 0, 0.5, "Head impact", "Company trim",
              "After arbitration: the same drop as loudness alone. Damping a sprawl purely here "
              "keeps the accent's place in the sort while taking it down in the mix - for when it "
              "is the right cue to spend the budget on and merely the wrong size."),

    // -- Stage 3: HeadImpact, the hero floor relief ---------------------------
    RDS_HRULE(AlgorithmConfig, "HeadImpact", "bHeroFloorRelief", kBool,
              strategies.head.heroFloorRelief, 0, 1, 1, "Head impact",
              "Head can start a moment",
              "Let a hard head strike open a hero moment the ordinary floor would have refused. "
              "The moment axis is otherwise limb-blind, so a faceplant that is plainly what a "
              "fall was about can come out as just another contact. Off, a head has to clear the "
              "same bar as an elbow."),
    RDS_PAIRS(AlgorithmConfig, "HeadImpact", "fHeroFloorReliefAtFrac", kFloat,
              strategies.head.heroFloorReliefAtFrac, 0, 3, 0.01, "Head impact",
              "Relief gate point",
              "How hard the head has to hit to earn the relief, as a multiple of the loud anchor "
              "- the same scale as the gate point above, so both stay where they are in the "
              "range when the anchor moves. Set it under the accent's gate and heads start "
              "anchoring moments they are not loud enough to be the accent of."),
    RDS_PARAM(AlgorithmConfig, "HeadImpact", "fHeroFloorReliefFrac", kFloat,
              strategies.head.heroFloorReliefFrac, 0, 1, 0.01, "Head impact", "Relief amount",
              "How much comes off the hero floor when it does, on that same scale, so the two "
              "subtract: a 0.30 floor with 0.10 here is a head floor of 0.20. It also lowers what "
              "the dominance test measures a quiet moment against, which is what actually lets "
              "the head anchor rather than merely pass the first gate - so this moves further "
              "than its size suggests. Zero is the same as switching it off."),

    // -- Stage 3: HeadImpact, claiming the onset -------------------------------
    //
    // The six budget-refund keys that used to sit here are gone. They bought a
    // hard head strike out of the arbitrator's budgets, which is what a hero
    // moment is - see HeroConfig. An ini that still sets them is read, ignored
    // and tidied away on the next save.
    Renamed(RDS_HRULE(AlgorithmConfig, "HeadImpact", "bClaimsOnsetOnHero", kBool,
                      strategies.head.claimsOnsetOnHero, 0, 1, 1, "Head impact", "Claims the onset",
                      "Whether the head accent becomes its own sound when the head is what made "
                      "the moment, instead of riding along with the body impact underneath it. "
                      "Off, a faceplant whose composite loses the rate cap goes completely "
                      "silent - the accent dies with whatever the arbitrator happened to drop."),
            "AirTime", "bHeadClaimsOnset"),

    // The head's ten damage rows used to sit here. They are under [Damage] now,
    // with the same ten for the spine and the same ten for a limb, and an old ini
    // is renamed in - see the Damage section for the one key that could not be.

    // -- Stage 3: AirTime, the head half --------------------------------------
    //
    // The head's own lead rule, moved out of HeadImpact when the body half
    // wanted the same measurement. Every key here carries the name it had
    // under [HeadImpact] as its legacy name, so a file written before the
    // move loads with its tuning intact and migrates itself the next time it
    // is saved.
    Renamed(RDS_PARAM(AlgorithmConfig, "AirTime", "bHeadEnabled", kBool,
                      strategies.airTime.headEnabled, 0, 1, 1, "Air time", "Head rule",
                      "Tell a dive from a sprawl by how long the body had been clear of the ground "
                      "when the head arrived, rather than by how fast the head was going. Off, a "
                      "head that lands first and a head that lands last sound the same at the same "
                      "speed."),
            "HeadImpact", "bLeadEnabled"),
    Renamed(RDS_PARAM(AlgorithmConfig, "AirTime", "fHeadClearMs", kFloat,
                      strategies.airTime.headClearMs, 0, 2000, 10, "Air time", "Head: air time",
                      "How long the rest of the body must have been off the ground for the head to "
                      "count as leading. Short values call the end of an ordinary sprawl a dive; "
                      "long ones only fire on a real fall from height."),
            "HeadImpact", "fLeadClearMs"),
    Renamed(RDS_HPAIR(AlgorithmConfig, "AirTime", "bHeadExcludeHands", kBool,
                      strategies.airTime.headExcludeHands, 0, 1, 1, "Air time",
                      "Head: ignore hands",
                      "A hand touching down with the head is the arm you threw out in front of the "
                      "dive, not proof the body was already down. Off, one hand landing in the "
                      "same frame as the skull collapses the air time to nothing and the rule "
                      "never fires. Hands still count as company, and they always count for the "
                      "body half and for the budget reset."),
            "HeadImpact", "bLeadExcludeHands"),
    Renamed(RDS_PARAM(AlgorithmConfig, "AirTime", "fHeadHandGraceMs", kFloat,
                      strategies.airTime.headHandGraceMs, 0, 2000, 10, "Air time",
                      "Head: hand grace",
                      "How recently a hand must have touched down to be forgiven as part of the "
                      "dive. Larger forgives more. Too large and a body that broke its fall on one "
                      "arm and clipped its head a second later reads as a faceplant."),
            "HeadImpact", "fLeadHandGraceMs"),
    Renamed(RDS_PARAM(AlgorithmConfig, "AirTime", "fHeadGateBonus", kFloat,
                      strategies.airTime.headGateBonus, 0, 1, 0.05, "Air time", "Head: gate bonus",
                      "How much lower the head gate sits for a head that led the body in, as a "
                      "fraction of that gate. This is what lets a slow, deliberate faceplant "
                      "through a gate set high enough to keep every tumbling skull out."),
            "HeadImpact", "fLeadGateBonus"),
    Renamed(RDS_HPAIR(AlgorithmConfig, "AirTime", "fHeadGainDb", kFloat,
                      strategies.airTime.headGainDb, -24, 24, 0.5, "Air time", "Head: level",
                      "Added to the accent for a head that fully led the body in, and scaled down "
                      "to nothing as the air time shortens. Before arbitration, so it buys the "
                      "accent rank as well as loudness."),
            "HeadImpact", "fLeadGainDb"),
    Renamed(RDS_PARAM(AlgorithmConfig, "AirTime", "fHeadTrimDb", kFloat,
                      strategies.airTime.headTrimDb, -24, 24, 0.5, "Air time", "Head: trim",
                      "The same boost after arbitration, where it is loudness alone and nothing "
                      "caps it. Reach for this when the accent is simply too quiet and the sort is "
                      "already coming out right."),
            "HeadImpact", "fLeadTrimDb"),
    Renamed(RDS_PARAM(AlgorithmConfig, "AirTime", "fHeadMaxLevelDb", kFloat,
                      strategies.airTime.headMaxLevelDb, -36, 12, 0.5, "Air time",
                      "Head: max level",
                      "The ceiling the boost may lift the accent to - where it lands, not how much "
                      "is added. It exists because a level before arbitration is also a rank: "
                      "without it a large boost on an already-loud contact does not just make the "
                      "skull loud, it makes it outrank the whole frame. Zero is the engine's own "
                      "natural ceiling and costs an ordinary head accent nothing. Cuts are never "
                      "capped."),
            "HeadImpact", "fLeadMaxLevelDb"),
    // The four head-halo keys are gone. They lifted the contacts *around* a led
    // head so the dive read as a body going down; the hero window is a peer
    // group and a spatial collapse already, and all four defaulted to zero.
    Renamed(RDS_HPAIR(AlgorithmConfig, "AirTime", "fHeadCrunchGateFrac", kFloat,
                      strategies.airTime.headCrunchGateFrac, 0, 3, 0.01, "Air time",
                      "Head crunch gate",
                      "The crunch gate a led head is held to instead of the global one, as a "
                      "multiple of the loud anchor. This is what puts a crunch on a slow dive "
                      "without putting one on every fast sprawl. Zero leaves the crunch gate "
                      "alone."),
            "HeadImpact", "fLeadCrunchGateFrac"),
    Renamed(RDS_PARAM(AlgorithmConfig, "AirTime", "fHeadCrunchProbability", kFloat,
                      strategies.airTime.headCrunchProbability, 0, 1, 0.05, "Air time",
                      "Head crunch chance",
                      "How often a led head over that gate actually crunches. One means every "
                      "time; lower keeps a dive from sounding scripted. The global crunch ramp is "
                      "not used here - it runs up to the certain speed and would leave a dive at "
                      "the bottom of it."),
            "HeadImpact", "fLeadCrunchProbability"),
    // `bHeadClaimsOnset` moved to [HeadImpact] as `bClaimsOnsetOnHero`: the
    // question is the same, but it is fired by the moment axis now rather than
    // by the head's own private air-time ramp.

    // -- Stage 3: AirTime, the body half --------------------------------------
    RDS_HRULE(AlgorithmConfig, "AirTime", "bBodyEnabled", kBool, strategies.airTime.bodyEnabled, 0,
              1, 1, "Air time", "Body rule",
              "The same idea for every limb that is not the head: a contact that arrives after "
              "real air time is a landing, and one that arrives mid-tumble is not. Closing speed "
              "cannot tell those apart - a tumble reaches the speeds it fell at - so without this "
              "the end of a fall is mixed exactly like the middle of one."),
    RDS_PARAM(AlgorithmConfig, "AirTime", "fBodyClearMs", kFloat, strategies.airTime.bodyClearMs, 0,
              2000, 10, "Air time", "Body: air time",
              "How long the body must have been off the ground for a limb to get the full lift, "
              "with a linear ramp up to it. Hands count as the ground here, so an arm that broke "
              "the fall ends the air time for everything after it."),
    RDS_PARAM(AlgorithmConfig, "AirTime", "fBodyIntensity", kFloat,
              strategies.airTime.bodyLift.intensity, 0, 1, 0.01, "Air time", "Body: intensity",
              "How much bigger a landing is made at full air time. Intensity is the loud knob - it "
              "moves the layer balance, the pitch bias and whether the contact is a composite at "
              "all rather than a tap - so this is the one that turns a landing that was filler "
              "into an event. The fall's own size is untouched, so a lifted landing cannot walk "
              "the actor into a different phase."),
    RDS_PAIRS(AlgorithmConfig, "AirTime", "fBodyGainDb", kFloat, strategies.airTime.bodyLift.gainDb, -24,
              24, 0.5, "Air time", "Body: level",
              "Added before arbitration, where a level is also a rank - so this is what stops the "
              "landing being crowded out by the scuffs around it."),
    RDS_PARAM(AlgorithmConfig, "AirTime", "fBodyTrimDb", kFloat, strategies.airTime.bodyLift.trimDb, -24,
              24, 0.5, "Air time", "Body: trim",
              "...and after it, where it is loudness alone and cannot take the frame over."),
    RDS_PARAM(AlgorithmConfig, "AirTime", "fBodyMaxLevelDb", kFloat,
              strategies.airTime.bodyLift.maxLevelDb, -36, 12, 0.5, "Air time", "Body: max level",
              "The ceiling the lift may reach, exactly as the head's: where the contact lands, not "
              "how much is added. Zero is the engine's own natural ceiling. Cuts are never "
              "capped."),

    // The seven budget-reset keys are gone. Air time is still the evidence -
    // it is `Hero:fArrivalMinAirMs` - but it buys a hero moment now rather than
    // a private waiver, so the effect lives on [Hero] with the rest of what a
    // moment is worth.

    // -- Stage 3: Damage ------------------------------------------------------
    //
    // Three parts, two tiers each, and the same twenty-two rows every time. It
    // replaces [CrunchGore] and the head's own damage block, which were two
    // differently-shaped rules for one sound sharing a single budget - so
    // whichever of them reached a contact first decided which layer you heard.
    //
    // Both shapes survive as tunings. A tier whose two chances are 1 is the
    // head's old deterministic rule exactly; one whose chance at threshold is
    // 0.15 is the old body ramp exactly. Nothing was lost by having one rule.
    //
    // What moved: the head's damage keys were under [HeadImpact] and are renamed
    // in from there, so an existing ini still loads. fHeadCrunchCapFrac is the
    // one that cannot be: the head's crunch ramp used to end at fGoreAtFrac
    // implicitly, and a rename maps one old key to one new key. It is a separate
    // number now and it takes its default.
    //
    // The body's own old keys have no honest target - a probability gate with
    // hysteresis is not a two-ended ramp - so [CrunchGore] is simply gone and its
    // lines in an old ini are ignored with a debug note, like any unknown key.
    Renamed(RDS_PARAM(AlgorithmConfig, "Damage", "bEnabled", kBool,
                      strategies.damage.enabled, 0, 1, 1, "Damage", "Damage enabled",
                      "The master switch for crunch and gore on every part of the body. This is most of "
                      "what makes the mod gnarly. Each part and each tier has its own switch under it."),
            "HeadImpact", "bDamageEnabled"),
    RDS_HRULE(AlgorithmConfig, "Damage", "iObliterateBudgetBonus", kInt,
              strategies.damage.obliterateBudgetBonus, 0, 16, 1, "Damage", "Obliterate: budget bonus",
              "Extra slots a tier's budget is granted when the contact is past "
              "Intensity:fObliterateFrac. That point used to be a second gate under the gore, "
              "which made the most extreme contacts the mod can see the hardest ones to hear; it "
              "loosens the limits now instead. 0 holds an obliterate to the ordinary budget."),
    RDS_PARAM(AlgorithmConfig, "Damage", "fObliterateSpacingScale", kFloat,
              strategies.damage.obliterateSpacingScale, 0, 1, 0.01, "Damage", "Obliterate: spacing scale",
              "What a tier's spacing is multiplied by for a contact past the obliterate point. 1 "
              "leaves the spacing alone, 0 removes it entirely. It is a relaxation and not a "
              "waiver on purpose: a raised budget is still a budget, so a ridiculous impulse "
              "from another mod cannot machine-gun the layer."),
    RDS_HRULE(AlgorithmConfig, "Damage", "bViolenceEnabled", kBool,
              strategies.damage.violence.enabled, 0, 1, 1, "Damage", "Violence terms",
              "Let how violent the fall has been move all six tiers, on top of how hard the "
              "contact itself was. A medium knock on a body that has been cartwheeling down a "
              "staircase breaks something more readily than the same knock on a body that was "
              "lying still. Off by default and off changes nothing; the amounts below are "
              "already set to a usable voicing."),
    RDS_PAIRS(AlgorithmConfig, "Damage", "fViolenceThrashWeight", kFloat,
              strategies.damage.violence.thrashWeight, 0, 4, 0.05, "Damage", "Thrash weight",
              "How much of the violence reading comes from the limbs being slammed about relative "
              "to the body. Its own measurement, sharing nothing with the garment's: this one "
              "weights each limb by how much *body* is on it, where the rustle weights by how "
              "much cloth. A naked body breaks exactly as well as a clothed one."),
    RDS_PARAM(AlgorithmConfig, "Damage", "fViolenceTumbleWeight", kFloat,
              strategies.damage.violence.tumbleWeight, 0, 4, 0.05, "Damage", "Tumble weight",
              "How much of it comes from the limbs being wrung about their own axes. A steady "
              "cartwheel has a large tumble and a small thrash; a body slamming from one pose to "
              "another has the reverse. Reads zero on takes recorded before the pose sidecar "
              "carried rotation."),
    RDS_HPAIR(AlgorithmConfig, "Damage", "fViolenceThrashFloor", kFloat,
              strategies.damage.violence.thrashFloor, 0, 8000, 10, "Damage", "Thrash floor",
              "Mass-weighted relative limb acceleration, u/s2, under which a fall counts as calm "
              "however long it goes on."),
    RDS_PARAM(AlgorithmConfig, "Damage", "fViolenceThrashFull", kFloat,
              strategies.damage.violence.thrashFull, 10, 20000, 50, "Damage", "Thrash full",
              "Where the thrash half reads maximum. Leave headroom above what the recordings "
              "reach: the worst fall in the corpus is not the worst fall there is."),
    RDS_HPAIR(AlgorithmConfig, "Damage", "fViolenceTumbleFloor", kFloat,
              strategies.damage.violence.tumbleFloor, 0, 2000, 5, "Damage", "Tumble floor",
              "Limb surface speed from rotation, u/s, under which spinning contributes nothing."),
    RDS_PARAM(AlgorithmConfig, "Damage", "fViolenceTumbleFull", kFloat,
              strategies.damage.violence.tumbleFull, 10, 4000, 10, "Damage", "Tumble full",
              "Where the rotation half reads maximum."),
    RDS_PARAM(AlgorithmConfig, "Damage", "fViolenceThrashCeiling", kFloat,
              strategies.damage.violence.thrashCeiling, 100, 40000, 50, "Damage",
              "Thrash ceiling",
              "Per-limb clamp before the sum, against a solver blow-up and against one limb "
              "striking stone standing in for the whole body."),
    RDS_PARAM(AlgorithmConfig, "Damage", "fViolenceHoldMs", kFloat,
              strategies.damage.violence.holdMs, 50, 4000, 10, "Damage", "Violence memory",
              "How long a violent stretch is remembered. Long enough to bridge the gaps between "
              "the bounces of a staircase, short enough that a fall which has settled is judged "
              "as settled. It is measured only between collisions - a contact cannot raise it, "
              "or every impact would be evidence of its own violence."),
    RDS_PARAM(AlgorithmConfig, "Damage", "fViolenceLimbShare", kFloat,
              strategies.damage.violence.limbShare, 0, 1, 0.05, "Damage", "Limb share",
              "How much of the answer is this limb rather than the whole body. 0 asks only how "
              "bad the fall is; 1 asks only how hard this particular arm is being whipped about. "
              "They come apart - a body sliding to a stop with one leg still cartwheeling is "
              "quiet on the first and loud on the second."),
    RDS_HPAIR(AlgorithmConfig, "Damage", "fViolenceGateDropFrac", kFloat,
              strategies.damage.violence.gateDropFrac, 0, 1, 0.05, "Damage", "Gate drop",
              "How far a violent fall lowers each tier's threshold, as a fraction of that tier's "
              "own span. The aggressive half of occurrence: it admits weaker contacts, so it can "
              "put a crunch on a knock that would never have earned one. What comes through the "
              "lowered bar still arrives at the quiet end of the level ramp."),
    RDS_PARAM(AlgorithmConfig, "Damage", "fViolenceChanceBonus", kFloat,
              strategies.damage.violence.chanceBonus, 0, 1, 0.05, "Damage", "Chance bonus",
              "How much more often an already-eligible contact fires. DEAD at the shipped defaults: "
              "all six tiers fire with certainty once past their gate, so there is no room above "
              "to add. Pull a tier's 'chance at gate' below 1 and this comes alive."),
    RDS_HPAIR(AlgorithmConfig, "Damage", "iViolenceBudgetBonus", kInt,
              strategies.damage.violence.budgetBonus, 0, 16, 1, "Damage", "Budget bonus",
              "Extra breaks a violent fall is allowed, on top of each tier's own budget. **The "
              "lever that actually moves how often you hear one**: measured on the corpus these "
              "tiers are budget-limited rather than threshold-limited, so lowering the gate admits "
              "contacts into a ledger that is usually already spent. Measured over the corpus: "
              "the gate drop alone is +3% more breaks, this alone is +5%, and the two together "
              "at 3 are +22%. Still a budget, so even the worst tumble cannot machine-gun it."),
    RDS_PARAM(AlgorithmConfig, "Damage", "fViolenceSpacingScale", kFloat,
              strategies.damage.violence.spacingScale, 0, 1, 0.05, "Damage", "Spacing scale",
              "How far the gap between two breaks shrinks when the fall is violent. DEAD at the "
              "shipped defaults: every tier ships with no spacing at all, and scaling zero is "
              "zero. Set a tier's spacing and this starts tightening it."),
    RDS_PARAM(AlgorithmConfig, "Damage", "fViolenceLevelBonusDb", kFloat,
              strategies.damage.violence.levelBonusDb, 0, 12, 0.5, "Damage", "Level bonus",
              "How much louder a break is when the fall is violent. A tier is discrete - a bone "
              "either broke or it did not - so this only pushes a break that was already "
              "happening further forward, and never softens one into existence."),
    RDS_HRULE(AlgorithmConfig, "Damage", "bHeadEnabled", kBool,
              strategies.damage.head.enabled, 0, 1, 1, "Head damage", "Enabled",
              "Crunch and gore for a head contact. Off, the part still sounds - it just never "
              "breaks. This is the honest way to silence one part's damage, because it stops the "
              "cue being proposed instead of muting it after the arbitrator has already made "
              "room for it."),
    RDS_PARAM(AlgorithmConfig, "Damage", "fHeadBodyForceShare", kFloat,
              strategies.damage.head.bodyForceShare, 0, 1, 0.01, "Head damage", "Body force share",
              "What the thresholds here are judged on. 0 is the contact's own closing speed and "
              "nothing else. 1 is the body's recent-energy envelope - the same decaying peak the "
              "hero rule measures dominance against, in the same u/s the gates are written in. "
              "Anything between is a blend. A skull is judged on its own arrival - that is the "
              "whole checkable claim of the head tier - so this is 0 by default here and nowhere "
              "else. The envelope has already taken the maximum with every contact of the frame, "
              "so turning this up can only ever loosen a gate, never tighten one."),
    RDS_HRULE(AlgorithmConfig, "Damage", "bHeadCrunchEnabled", kBool,
              strategies.damage.head.crunch.enabled, 0, 1, 1, "Head damage", "Crunch enabled",
              "The granular bone-break layer, for the skull. The two tiers are independent: gore "
              "is not nested inside the crunch, so a contact bad enough to be wet still sounds "
              "wet on a frame where the crunch budget is gone."),
    Renamed(
        RDS_HPAIR(AlgorithmConfig, "Damage", "fHeadCrunchAtFrac", kFloat,
                  strategies.damage.head.crunch.atFrac, 0, 3, 0.01, "Head damage", "Crunch threshold",
                  "Where the crunch opens for the skull, as a multiple of the loud anchor - so the "
                  "whole tier structure moves with the range instead of being re-derived every time "
                  "the anchor does."),
            "HeadImpact", "fCrunchAtFrac"),
    RDS_PARAM(AlgorithmConfig, "Damage", "fHeadCrunchCapFrac", kFloat,
              strategies.damage.head.crunch.capFrac, 0, 3, 0.01, "Head damage", "Crunch cap",
              "Where both ramps top out, on the same scale. Above it a harder contact has "
              "nothing more to say through this tier. A cap at or under the threshold is not an "
              "error - it is how you ask for a step instead of a ramp - and collapses to full "
              "from the threshold up rather than dividing by a span of one unit."),
    RDS_PAIRS(AlgorithmConfig, "Damage", "fHeadCrunchProbAtGate", kFloat,
              strategies.damage.head.crunch.probAtGate, 0, 1, 0.01, "Head damage", "Chance at threshold",
              "How likely the crunch is at its threshold. 1, the default, means the threshold "
              "means what it says. Pull it down and the bottom of the range becomes a maybe that "
              "firms up as the contact gets worse - which is the shape to reach for when a tier "
              "fires too reliably to feel alive."),
    RDS_PARAM(AlgorithmConfig, "Damage", "fHeadCrunchProbAtCap", kFloat,
              strategies.damage.head.crunch.probAtCap, 0, 1, 0.01, "Head damage", "Chance at cap",
              "How likely it is at the cap and above. Left at 1 with the chance at threshold "
              "also at 1, the tier draws no random number at all - which keeps switching this "
              "part's damage on from re-rolling every variant and scatter after it."),
    Renamed(
        RDS_HPAIR(AlgorithmConfig, "Damage", "fHeadCrunchQuietDb", kFloat,
                  strategies.damage.head.crunch.quietDb, -40, 12, 0.5, "Head damage", "Crunch at threshold",
                  "How loud the crunch is just over its threshold, against the contact's own onset "
                  "level. Deliberately well down: the point of a ramp is that crossing it is not an "
                  "event in itself."),
            "HeadImpact", "fCrunchQuietDb"),
    Renamed(
        RDS_PARAM(AlgorithmConfig, "Damage", "fHeadCrunchLoudDb", kFloat,
                  strategies.damage.head.crunch.loudDb, -40, 12, 0.5, "Head damage", "Crunch at cap",
                  "How loud it is at the cap. Everything between is a straight ramp on the same span "
                  "the chance ramps over."),
            "HeadImpact", "fCrunchLoudDb"),
    Renamed(
        RDS_PARAM(AlgorithmConfig, "Damage", "fHeadCrunchDelayMs", kFloat,
                  strategies.damage.head.crunch.delayMs, 0, 200, 1, "Head damage", "Crunch delay",
                  "How long after the impact this lands, measured from the composite's own body "
                  "offset so moving the stack moves it too. Not zero: a break is what the bone did, "
                  "and it reads as consequence rather than as texture when it arrives a beat late."),
            "HeadImpact", "fCrunchDelayMs"),
    RDS_HPAIR(AlgorithmConfig, "Damage", "iHeadCrunchBudget", kInt,
              strategies.damage.head.crunch.budget, 0, 16, 1, "Head damage", "Crunch per knockdown",
              "How many of these one knockdown may produce, reset when the body is launched "
              "again. One budget per part per tier and never a shared one: a shared counter lets "
              "the order contacts happen to arrive in decide which layer you hear, and a crunch "
              "that was proposed and then dropped spends the budget anyway."),
    RDS_PARAM(AlgorithmConfig, "Damage", "fHeadCrunchSpacingMs", kFloat,
              strategies.damage.head.crunch.spacingMs, 0, 2000, 10, "Head damage", "Crunch spacing",
              "The shortest gap between two of these. 0, the default, is off. The finer control "
              "of the two and the one to reach for first: any value at all stops one sprawl "
              "arriving as a burst of breaking sticks without capping what a long tumble may add "
              "up to."),
    RDS_HRULE(AlgorithmConfig, "Damage", "bHeadGoreEnabled", kBool,
              strategies.damage.head.gore.enabled, 0, 1, 1, "Head damage", "Gore enabled",
              "The wet layer above the crunch, for the skull. The two tiers are independent: "
              "gore is not nested inside the crunch, so a contact bad enough to be wet still "
              "sounds wet on a frame where the crunch budget is gone."),
    Renamed(
        RDS_HPAIR(AlgorithmConfig, "Damage", "fHeadGoreAtFrac", kFloat,
                  strategies.damage.head.gore.atFrac, 0, 3, 0.01, "Head damage", "Gore threshold",
                  "Where the gore opens for the skull, as a multiple of the loud anchor - so the "
                  "whole tier structure moves with the range instead of being re-derived every time "
                  "the anchor does."),
            "HeadImpact", "fGoreAtFrac"),
    Renamed(
        RDS_PARAM(AlgorithmConfig, "Damage", "fHeadGoreCapFrac", kFloat,
                  strategies.damage.head.gore.capFrac, 0, 3, 0.01, "Head damage", "Gore cap",
                  "Where both ramps top out, on the same scale. Above it a harder contact has "
                  "nothing more to say through this tier. A cap at or under the threshold is not an "
                  "error - it is how you ask for a step instead of a ramp - and collapses to full "
                  "from the threshold up rather than dividing by a span of one unit."),
            "HeadImpact", "fGoreFullFrac"),
    RDS_PAIRS(AlgorithmConfig, "Damage", "fHeadGoreProbAtGate", kFloat,
              strategies.damage.head.gore.probAtGate, 0, 1, 0.01, "Head damage", "Chance at threshold",
              "How likely the gore is at its threshold. 1, the default, means the threshold "
              "means what it says. Pull it down and the bottom of the range becomes a maybe that "
              "firms up as the contact gets worse - which is the shape to reach for when a tier "
              "fires too reliably to feel alive."),
    RDS_PARAM(AlgorithmConfig, "Damage", "fHeadGoreProbAtCap", kFloat,
              strategies.damage.head.gore.probAtCap, 0, 1, 0.01, "Head damage", "Chance at cap",
              "How likely it is at the cap and above. Left at 1 with the chance at threshold "
              "also at 1, the tier draws no random number at all - which keeps switching this "
              "part's damage on from re-rolling every variant and scatter after it."),
    Renamed(
        RDS_HPAIR(AlgorithmConfig, "Damage", "fHeadGoreQuietDb", kFloat,
                  strategies.damage.head.gore.quietDb, -40, 12, 0.5, "Head damage", "Gore at threshold",
                  "How loud the gore is just over its threshold, against the contact's own onset "
                  "level. Deliberately well down: the point of a ramp is that crossing it is not an "
                  "event in itself."),
            "HeadImpact", "fGoreQuietDb"),
    Renamed(
        RDS_PARAM(AlgorithmConfig, "Damage", "fHeadGoreLoudDb", kFloat,
                  strategies.damage.head.gore.loudDb, -40, 12, 0.5, "Head damage", "Gore at cap",
                  "How loud it is at the cap. Everything between is a straight ramp on the same span "
                  "the chance ramps over."),
            "HeadImpact", "fGoreLoudDb"),
    Renamed(
        RDS_PARAM(AlgorithmConfig, "Damage", "fHeadGoreDelayMs", kFloat,
                  strategies.damage.head.gore.delayMs, 0, 200, 1, "Head damage", "Gore delay",
                  "How long after the impact this lands, measured from the composite's own body "
                  "offset so moving the stack moves it too. Not zero: a break is what the bone did, "
                  "and it reads as consequence rather than as texture when it arrives a beat late."),
            "HeadImpact", "fGoreDelayMs"),
    RDS_HPAIR(AlgorithmConfig, "Damage", "iHeadGoreBudget", kInt,
              strategies.damage.head.gore.budget, 0, 16, 1, "Head damage", "Gore per knockdown",
              "How many of these one knockdown may produce, reset when the body is launched "
              "again. One budget per part per tier and never a shared one: a shared counter lets "
              "the order contacts happen to arrive in decide which layer you hear, and a crunch "
              "that was proposed and then dropped spends the budget anyway."),
    RDS_PARAM(AlgorithmConfig, "Damage", "fHeadGoreSpacingMs", kFloat,
              strategies.damage.head.gore.spacingMs, 0, 2000, 10, "Head damage", "Gore spacing",
              "The shortest gap between two of these. 0, the default, is off. The finer control "
              "of the two and the one to reach for first: any value at all stops one sprawl "
              "arriving as a burst of breaking sticks without capping what a long tumble may add "
              "up to."),
    RDS_HRULE(AlgorithmConfig, "Damage", "bSpineEnabled", kBool,
              strategies.damage.spine.enabled, 0, 1, 1, "Spine damage", "Enabled",
              "Crunch and gore for a neck or torso contact. Off, the part still sounds - it just "
              "never breaks. This is the honest way to silence one part's damage, because it "
              "stops the cue being proposed instead of muting it after the arbitrator has "
              "already made room for it."),
    RDS_PARAM(AlgorithmConfig, "Damage", "fSpineBodyForceShare", kFloat,
              strategies.damage.spine.bodyForceShare, 0, 1, 0.01, "Spine damage", "Body force share",
              "What the thresholds here are judged on. 0 is the contact's own closing speed and "
              "nothing else. 1 is the body's recent-energy envelope - the same decaying peak the "
              "hero rule measures dominance against, in the same u/s the gates are written in. "
              "Anything between is a blend. A spine contact is most of the body by definition, "
              "so what happened to the body is close to what happened to it. The envelope has "
              "already taken the maximum with every contact of the frame, so turning this up can "
              "only ever loosen a gate, never tighten one."),
    RDS_HRULE(AlgorithmConfig, "Damage", "bSpineCrunchEnabled", kBool,
              strategies.damage.spine.crunch.enabled, 0, 1, 1, "Spine damage", "Crunch enabled",
              "The granular bone-break layer, for the spine. The two tiers are independent: gore "
              "is not nested inside the crunch, so a contact bad enough to be wet still sounds "
              "wet on a frame where the crunch budget is gone."),
    RDS_HPAIR(AlgorithmConfig, "Damage", "fSpineCrunchAtFrac", kFloat,
              strategies.damage.spine.crunch.atFrac, 0, 3, 0.01, "Spine damage", "Crunch threshold",
              "Where the crunch opens for the spine, as a multiple of the loud anchor - so the "
              "whole tier structure moves with the range instead of being re-derived every time "
              "the anchor does."),
    RDS_PARAM(AlgorithmConfig, "Damage", "fSpineCrunchCapFrac", kFloat,
              strategies.damage.spine.crunch.capFrac, 0, 3, 0.01, "Spine damage", "Crunch cap",
              "Where both ramps top out, on the same scale. Above it a harder contact has "
              "nothing more to say through this tier. A cap at or under the threshold is not an "
              "error - it is how you ask for a step instead of a ramp - and collapses to full "
              "from the threshold up rather than dividing by a span of one unit."),
    RDS_PAIRS(AlgorithmConfig, "Damage", "fSpineCrunchProbAtGate", kFloat,
              strategies.damage.spine.crunch.probAtGate, 0, 1, 0.01, "Spine damage", "Chance at threshold",
              "How likely the crunch is at its threshold. 1, the default, means the threshold "
              "means what it says. Pull it down and the bottom of the range becomes a maybe that "
              "firms up as the contact gets worse - which is the shape to reach for when a tier "
              "fires too reliably to feel alive."),
    RDS_PARAM(AlgorithmConfig, "Damage", "fSpineCrunchProbAtCap", kFloat,
              strategies.damage.spine.crunch.probAtCap, 0, 1, 0.01, "Spine damage", "Chance at cap",
              "How likely it is at the cap and above. Left at 1 with the chance at threshold "
              "also at 1, the tier draws no random number at all - which keeps switching this "
              "part's damage on from re-rolling every variant and scatter after it."),
    RDS_HPAIR(AlgorithmConfig, "Damage", "fSpineCrunchQuietDb", kFloat,
              strategies.damage.spine.crunch.quietDb, -40, 12, 0.5, "Spine damage", "Crunch at threshold",
              "How loud the crunch is just over its threshold, against the contact's own onset "
              "level. Deliberately well down: the point of a ramp is that crossing it is not an "
              "event in itself."),
    RDS_PARAM(AlgorithmConfig, "Damage", "fSpineCrunchLoudDb", kFloat,
              strategies.damage.spine.crunch.loudDb, -40, 12, 0.5, "Spine damage", "Crunch at cap",
              "How loud it is at the cap. Everything between is a straight ramp on the same span "
              "the chance ramps over."),
    RDS_PARAM(AlgorithmConfig, "Damage", "fSpineCrunchDelayMs", kFloat,
              strategies.damage.spine.crunch.delayMs, 0, 200, 1, "Spine damage", "Crunch delay",
              "How long after the impact this lands, measured from the composite's own body "
              "offset so moving the stack moves it too. Not zero: a break is what the bone did, "
              "and it reads as consequence rather than as texture when it arrives a beat late."),
    RDS_HPAIR(AlgorithmConfig, "Damage", "iSpineCrunchBudget", kInt,
              strategies.damage.spine.crunch.budget, 0, 16, 1, "Spine damage", "Crunch per knockdown",
              "How many of these one knockdown may produce, reset when the body is launched "
              "again. One budget per part per tier and never a shared one: a shared counter lets "
              "the order contacts happen to arrive in decide which layer you hear, and a crunch "
              "that was proposed and then dropped spends the budget anyway."),
    RDS_PARAM(AlgorithmConfig, "Damage", "fSpineCrunchSpacingMs", kFloat,
              strategies.damage.spine.crunch.spacingMs, 0, 2000, 10, "Spine damage", "Crunch spacing",
              "The shortest gap between two of these. 0, the default, is off. The finer control "
              "of the two and the one to reach for first: any value at all stops one sprawl "
              "arriving as a burst of breaking sticks without capping what a long tumble may add "
              "up to."),
    RDS_HRULE(AlgorithmConfig, "Damage", "bSpineGoreEnabled", kBool,
              strategies.damage.spine.gore.enabled, 0, 1, 1, "Spine damage", "Gore enabled",
              "The wet layer above the crunch, for the spine. The two tiers are independent: "
              "gore is not nested inside the crunch, so a contact bad enough to be wet still "
              "sounds wet on a frame where the crunch budget is gone."),
    RDS_HPAIR(AlgorithmConfig, "Damage", "fSpineGoreAtFrac", kFloat,
              strategies.damage.spine.gore.atFrac, 0, 3, 0.01, "Spine damage", "Gore threshold",
              "Where the gore opens for the spine, as a multiple of the loud anchor - so the "
              "whole tier structure moves with the range instead of being re-derived every time "
              "the anchor does."),
    RDS_PARAM(AlgorithmConfig, "Damage", "fSpineGoreCapFrac", kFloat,
              strategies.damage.spine.gore.capFrac, 0, 3, 0.01, "Spine damage", "Gore cap",
              "Where both ramps top out, on the same scale. Above it a harder contact has "
              "nothing more to say through this tier. A cap at or under the threshold is not an "
              "error - it is how you ask for a step instead of a ramp - and collapses to full "
              "from the threshold up rather than dividing by a span of one unit."),
    RDS_PAIRS(AlgorithmConfig, "Damage", "fSpineGoreProbAtGate", kFloat,
              strategies.damage.spine.gore.probAtGate, 0, 1, 0.01, "Spine damage", "Chance at threshold",
              "How likely the gore is at its threshold. 1, the default, means the threshold "
              "means what it says. Pull it down and the bottom of the range becomes a maybe that "
              "firms up as the contact gets worse - which is the shape to reach for when a tier "
              "fires too reliably to feel alive."),
    RDS_PARAM(AlgorithmConfig, "Damage", "fSpineGoreProbAtCap", kFloat,
              strategies.damage.spine.gore.probAtCap, 0, 1, 0.01, "Spine damage", "Chance at cap",
              "How likely it is at the cap and above. Left at 1 with the chance at threshold "
              "also at 1, the tier draws no random number at all - which keeps switching this "
              "part's damage on from re-rolling every variant and scatter after it."),
    RDS_HPAIR(AlgorithmConfig, "Damage", "fSpineGoreQuietDb", kFloat,
              strategies.damage.spine.gore.quietDb, -40, 12, 0.5, "Spine damage", "Gore at threshold",
              "How loud the gore is just over its threshold, against the contact's own onset "
              "level. Deliberately well down: the point of a ramp is that crossing it is not an "
              "event in itself."),
    RDS_PARAM(AlgorithmConfig, "Damage", "fSpineGoreLoudDb", kFloat,
              strategies.damage.spine.gore.loudDb, -40, 12, 0.5, "Spine damage", "Gore at cap",
              "How loud it is at the cap. Everything between is a straight ramp on the same span "
              "the chance ramps over."),
    RDS_PARAM(AlgorithmConfig, "Damage", "fSpineGoreDelayMs", kFloat,
              strategies.damage.spine.gore.delayMs, 0, 200, 1, "Spine damage", "Gore delay",
              "How long after the impact this lands, measured from the composite's own body "
              "offset so moving the stack moves it too. Not zero: a break is what the bone did, "
              "and it reads as consequence rather than as texture when it arrives a beat late."),
    RDS_HPAIR(AlgorithmConfig, "Damage", "iSpineGoreBudget", kInt,
              strategies.damage.spine.gore.budget, 0, 16, 1, "Spine damage", "Gore per knockdown",
              "How many of these one knockdown may produce, reset when the body is launched "
              "again. One budget per part per tier and never a shared one: a shared counter lets "
              "the order contacts happen to arrive in decide which layer you hear, and a crunch "
              "that was proposed and then dropped spends the budget anyway."),
    RDS_PARAM(AlgorithmConfig, "Damage", "fSpineGoreSpacingMs", kFloat,
              strategies.damage.spine.gore.spacingMs, 0, 2000, 10, "Spine damage", "Gore spacing",
              "The shortest gap between two of these. 0, the default, is off. The finer control "
              "of the two and the one to reach for first: any value at all stops one sprawl "
              "arriving as a burst of breaking sticks without capping what a long tumble may add "
              "up to."),
    RDS_HRULE(AlgorithmConfig, "Damage", "bLimbEnabled", kBool,
              strategies.damage.limb.enabled, 0, 1, 1, "Limb damage", "Enabled",
              "Crunch and gore for an arm, leg, hand or foot contact - and anything on a "
              "skeleton we could not name. Off, the part still sounds - it just never breaks. "
              "This is the honest way to silence one part's damage, because it stops the cue "
              "being proposed instead of muting it after the arbitrator has already made room "
              "for it."),
    RDS_PARAM(AlgorithmConfig, "Damage", "fLimbBodyForceShare", kFloat,
              strategies.damage.limb.bodyForceShare, 0, 1, 0.01, "Limb damage", "Body force share",
              "What the thresholds here are judged on. 0 is the contact's own closing speed and "
              "nothing else. 1 is the body's recent-energy envelope - the same decaying peak the "
              "hero rule measures dominance against, in the same u/s the gates are written in. "
              "Anything between is a blend. A limb is the part whose own closing speed says "
              "least about whether it broke: a leg can snap in a slam it barely touched. The "
              "envelope has already taken the maximum with every contact of the frame, so "
              "turning this up can only ever loosen a gate, never tighten one."),
    RDS_HRULE(AlgorithmConfig, "Damage", "bLimbCrunchEnabled", kBool,
              strategies.damage.limb.crunch.enabled, 0, 1, 1, "Limb damage", "Crunch enabled",
              "The granular bone-break layer, for a limb. The two tiers are independent: gore is "
              "not nested inside the crunch, so a contact bad enough to be wet still sounds wet "
              "on a frame where the crunch budget is gone."),
    RDS_HPAIR(AlgorithmConfig, "Damage", "fLimbCrunchAtFrac", kFloat,
              strategies.damage.limb.crunch.atFrac, 0, 3, 0.01, "Limb damage", "Crunch threshold",
              "Where the crunch opens for a limb, as a multiple of the loud anchor - so the "
              "whole tier structure moves with the range instead of being re-derived every time "
              "the anchor does."),
    RDS_PARAM(AlgorithmConfig, "Damage", "fLimbCrunchCapFrac", kFloat,
              strategies.damage.limb.crunch.capFrac, 0, 3, 0.01, "Limb damage", "Crunch cap",
              "Where both ramps top out, on the same scale. Above it a harder contact has "
              "nothing more to say through this tier. A cap at or under the threshold is not an "
              "error - it is how you ask for a step instead of a ramp - and collapses to full "
              "from the threshold up rather than dividing by a span of one unit."),
    RDS_PAIRS(AlgorithmConfig, "Damage", "fLimbCrunchProbAtGate", kFloat,
              strategies.damage.limb.crunch.probAtGate, 0, 1, 0.01, "Limb damage", "Chance at threshold",
              "How likely the crunch is at its threshold. 1, the default, means the threshold "
              "means what it says. Pull it down and the bottom of the range becomes a maybe that "
              "firms up as the contact gets worse - which is the shape to reach for when a tier "
              "fires too reliably to feel alive."),
    RDS_PARAM(AlgorithmConfig, "Damage", "fLimbCrunchProbAtCap", kFloat,
              strategies.damage.limb.crunch.probAtCap, 0, 1, 0.01, "Limb damage", "Chance at cap",
              "How likely it is at the cap and above. Left at 1 with the chance at threshold "
              "also at 1, the tier draws no random number at all - which keeps switching this "
              "part's damage on from re-rolling every variant and scatter after it."),
    RDS_HPAIR(AlgorithmConfig, "Damage", "fLimbCrunchQuietDb", kFloat,
              strategies.damage.limb.crunch.quietDb, -40, 12, 0.5, "Limb damage", "Crunch at threshold",
              "How loud the crunch is just over its threshold, against the contact's own onset "
              "level. Deliberately well down: the point of a ramp is that crossing it is not an "
              "event in itself."),
    RDS_PARAM(AlgorithmConfig, "Damage", "fLimbCrunchLoudDb", kFloat,
              strategies.damage.limb.crunch.loudDb, -40, 12, 0.5, "Limb damage", "Crunch at cap",
              "How loud it is at the cap. Everything between is a straight ramp on the same span "
              "the chance ramps over."),
    RDS_PARAM(AlgorithmConfig, "Damage", "fLimbCrunchDelayMs", kFloat,
              strategies.damage.limb.crunch.delayMs, 0, 200, 1, "Limb damage", "Crunch delay",
              "How long after the impact this lands, measured from the composite's own body "
              "offset so moving the stack moves it too. Not zero: a break is what the bone did, "
              "and it reads as consequence rather than as texture when it arrives a beat late."),
    RDS_HPAIR(AlgorithmConfig, "Damage", "iLimbCrunchBudget", kInt,
              strategies.damage.limb.crunch.budget, 0, 16, 1, "Limb damage", "Crunch per knockdown",
              "How many of these one knockdown may produce, reset when the body is launched "
              "again. One budget per part per tier and never a shared one: a shared counter lets "
              "the order contacts happen to arrive in decide which layer you hear, and a crunch "
              "that was proposed and then dropped spends the budget anyway."),
    RDS_PARAM(AlgorithmConfig, "Damage", "fLimbCrunchSpacingMs", kFloat,
              strategies.damage.limb.crunch.spacingMs, 0, 2000, 10, "Limb damage", "Crunch spacing",
              "The shortest gap between two of these. 0, the default, is off. The finer control "
              "of the two and the one to reach for first: any value at all stops one sprawl "
              "arriving as a burst of breaking sticks without capping what a long tumble may add "
              "up to."),
    RDS_HRULE(AlgorithmConfig, "Damage", "bLimbGoreEnabled", kBool,
              strategies.damage.limb.gore.enabled, 0, 1, 1, "Limb damage", "Gore enabled",
              "The wet layer above the crunch, for a limb. The two tiers are independent: gore "
              "is not nested inside the crunch, so a contact bad enough to be wet still sounds "
              "wet on a frame where the crunch budget is gone."),
    RDS_HPAIR(AlgorithmConfig, "Damage", "fLimbGoreAtFrac", kFloat,
              strategies.damage.limb.gore.atFrac, 0, 3, 0.01, "Limb damage", "Gore threshold",
              "Where the gore opens for a limb, as a multiple of the loud anchor - so the whole "
              "tier structure moves with the range instead of being re-derived every time the "
              "anchor does."),
    RDS_PARAM(AlgorithmConfig, "Damage", "fLimbGoreCapFrac", kFloat,
              strategies.damage.limb.gore.capFrac, 0, 3, 0.01, "Limb damage", "Gore cap",
              "Where both ramps top out, on the same scale. Above it a harder contact has "
              "nothing more to say through this tier. A cap at or under the threshold is not an "
              "error - it is how you ask for a step instead of a ramp - and collapses to full "
              "from the threshold up rather than dividing by a span of one unit."),
    RDS_PAIRS(AlgorithmConfig, "Damage", "fLimbGoreProbAtGate", kFloat,
              strategies.damage.limb.gore.probAtGate, 0, 1, 0.01, "Limb damage", "Chance at threshold",
              "How likely the gore is at its threshold. 1, the default, means the threshold "
              "means what it says. Pull it down and the bottom of the range becomes a maybe that "
              "firms up as the contact gets worse - which is the shape to reach for when a tier "
              "fires too reliably to feel alive."),
    RDS_PARAM(AlgorithmConfig, "Damage", "fLimbGoreProbAtCap", kFloat,
              strategies.damage.limb.gore.probAtCap, 0, 1, 0.01, "Limb damage", "Chance at cap",
              "How likely it is at the cap and above. Left at 1 with the chance at threshold "
              "also at 1, the tier draws no random number at all - which keeps switching this "
              "part's damage on from re-rolling every variant and scatter after it."),
    RDS_HPAIR(AlgorithmConfig, "Damage", "fLimbGoreQuietDb", kFloat,
              strategies.damage.limb.gore.quietDb, -40, 12, 0.5, "Limb damage", "Gore at threshold",
              "How loud the gore is just over its threshold, against the contact's own onset "
              "level. Deliberately well down: the point of a ramp is that crossing it is not an "
              "event in itself."),
    RDS_PARAM(AlgorithmConfig, "Damage", "fLimbGoreLoudDb", kFloat,
              strategies.damage.limb.gore.loudDb, -40, 12, 0.5, "Limb damage", "Gore at cap",
              "How loud it is at the cap. Everything between is a straight ramp on the same span "
              "the chance ramps over."),
    RDS_PARAM(AlgorithmConfig, "Damage", "fLimbGoreDelayMs", kFloat,
              strategies.damage.limb.gore.delayMs, 0, 200, 1, "Limb damage", "Gore delay",
              "How long after the impact this lands, measured from the composite's own body "
              "offset so moving the stack moves it too. Not zero: a break is what the bone did, "
              "and it reads as consequence rather than as texture when it arrives a beat late."),
    RDS_HPAIR(AlgorithmConfig, "Damage", "iLimbGoreBudget", kInt,
              strategies.damage.limb.gore.budget, 0, 16, 1, "Limb damage", "Gore per knockdown",
              "How many of these one knockdown may produce, reset when the body is launched "
              "again. One budget per part per tier and never a shared one: a shared counter lets "
              "the order contacts happen to arrive in decide which layer you hear, and a crunch "
              "that was proposed and then dropped spends the budget anyway."),
    RDS_PARAM(AlgorithmConfig, "Damage", "fLimbGoreSpacingMs", kFloat,
              strategies.damage.limb.gore.spacingMs, 0, 2000, 10, "Limb damage", "Gore spacing",
              "The shortest gap between two of these. 0, the default, is off. The finer control "
              "of the two and the one to reach for first: any value at all stops one sprawl "
              "arriving as a burst of breaking sticks without capping what a long tumble may add "
              "up to."),

    // -- The slide, end to end ------------------------------------------------
    //
    // One section and one drawer, because a slide is one thing. It used to be
    // two: when a slide starts and stops lived under [Motion] with the rest of
    // the motion axis, and what one sounds like lived under [ScrapeLoop], so
    // answering "why is the grind doing that" meant two panels and knowing which
    // half owned which number.
    //
    // The split inside it is still the design's: the state is physics, the
    // voicing is design. What moved is only where you turn the knobs.
    RDS_PARAM(AlgorithmConfig, "Slide", "bEnabled", kBool, strategies.scrape.enabled, 0, 1, 1,
              "Slide and scrape", "Scrape enabled",
              "The grinding loops for a body dragging along a surface, and the catches riding on "
              "them. Off, a slide still happens - the state, the impact budget and the slide-end "
              "lift are all still there - it simply makes no sound of its own."),

    // -- when a slide is happening --------------------------------------------
    Renamed(Renamed(RDS_HRULE(AlgorithmConfig, "Slide", "fSlideMinTangentSpeed", kFloat,
                              motion.slideMinTangentSpeed, 0, 2000, 10, "Slide and scrape",
                              "Slide speed",
                              "How fast a body has to be travelling along the ground before the "
                              "slide state opens and the grinding loops are allowed."),
                    "Phase", "fSlideMinTangentSpeed"),
            "Motion", "fSlideMinTangentSpeed"),
    Renamed(Renamed(RDS_PARAM(AlgorithmConfig, "Slide", "fSlideMinDurationMs", kFloat,
                              motion.slideMinDurationMs, 0, 3000, 10, "Slide and scrape",
                              "Slide duration",
                              "And for how long, so a single glancing blow does not read as a "
                              "slide."),
                    "Phase", "fSlideMinDurationMs"),
            "Motion", "fSlideMinDurationMs"),
    Renamed(Renamed(RDS_PAIRS(AlgorithmConfig, "Slide", "fSlideMinDistance", kFloat,
                              motion.slideMinDistance, 0, 2000, 5, "Slide and scrape",
                              "Slide distance",
                              "Or how far the body has to have travelled, if it gets there before "
                              "the duration above. A fast skid crosses two metres in a few "
                              "frames; without this only slow grinding ever counted as a slide."),
                    "ScrapeLoop", "fMinDistance"),
            "Motion", "fSlideMinDistance"),
    Renamed(RDS_PARAM(AlgorithmConfig, "Slide", "fSlideGraceMs", kFloat, motion.slideGraceMs, 0,
                      2000, 10, "Slide and scrape", "Slide grace",
                      "How long the grazing may stop before the slide is over. This does not "
                      "decide *how* it ended - the body does that, and the two ways out are "
                      "coming to rest and going airborne - only that it has. Short, and a couple "
                      "of quiet frames in a long grind break it into pieces; long, and the grind "
                      "carries on past the moment the body was stopped."),
            "Motion", "fSlideGraceMs"),
    RDS_PAIRS(AlgorithmConfig, "Slide", "fSlideHoldSpeed", kFloat, motion.slideHoldSpeed, 0, 2000,
              5, "Slide and scrape", "Slide hold speed",
              "The exit speed, and the counterpart to the slide speed above - that one opens a "
              "slide and is asked of the contacts, this one keeps it and is asked of the body.\n\n"
              "Over this, the slide outlives its own grazes: the collision stream going quiet "
              "while the corpse is still travelling is the solver taking a breath, not the slide "
              "ending, and the grind should not stop with the body still moving. Under it, the "
              "slide grace above has the last word exactly as before.\n\n"
              "Set it under the slide speed and the pair is hysteresis - it takes real rubbing to "
              "start a grind and rather less to keep one. Zero is off. Needs a take with pose: "
              "without it the only speed available is the decaying contact hold, and holding a "
              "slide on that ends it on its own decay curve rather than on the body."),
    RDS_PARAM(AlgorithmConfig, "Slide", "fSlideHoldMaxMs", kFloat, motion.slideHoldMaxMs, 0, 8000,
              50, "Slide and scrape", "Slide hold cap",
              "The longest a slide may be held on speed alone with nothing touching, counted from "
              "the last graze. The body slowing down is what ends the hold in every honest case; "
              "this is the bound for a pose stream that says a body is drifting when nothing is "
              "in contact with it. It never shortens an ordinary slide - one with grazes in it "
              "never reaches this timer."),
    Renamed(RDS_HPAIR(AlgorithmConfig, "Slide", "fGrazeRatio", kFloat, ingest.grazeRatio, 0.2, 10,
                      0.1, "Slide and scrape", "Graze ratio",
                      "How much faster a contact must be sliding along a surface than into it "
                      "before it counts as rubbing rather than hitting. This is the first "
                      "decision a slide is made of: it is what feeds the scrape path, and about "
                      "half of all worthwhile contacts in a tumble currently land on this side of "
                      "it, which the mod's own notes flag as untested."),
            "Ingest", "fGrazeRatio"),
    Renamed(RDS_PARAM(AlgorithmConfig, "Slide", "fGrazeMaxImpactSpeed", kFloat,
                      ingest.grazeMaxImpactSpeed, 0, 4000, 10, "Slide and scrape",
                      "Graze speed ceiling",
                      "Above this closing speed a contact is always a thud, however much it is "
                      "also sliding. Raise it and hard skids go quiet as the scrape path swallows "
                      "them; lower it and genuine slides start thudding."),
            "Ingest", "fGrazeMaxImpactSpeed"),
    RDS_HRULE(AlgorithmConfig, "Slide", "bSlidesDontClaim", kBool,
              strategies.scrape.slidesDontClaim, 0, 1, 1, "Slide and scrape",
              "Slides dont claim",
              "Whether a running slide hands its harder contacts back to the impact path "
              "instead of spending them as scrape.\n"
              "Off, a grind owns every rub under it, which is what keeps a skid from thudding "
              "on every frame. On, the slider below draws the line and anything over it is "
              "built as an impact as well as being ground.\n"
              "A rub with no slide running under it always reaches the impact path and never "
              "had a switch worth keeping: claiming those was deleting about half of every "
              "knockdown's contacts and playing nothing in their place."),
    RDS_PARAM(AlgorithmConfig, "Slide", "fClaimBelowIntensity", kFloat,
              strategies.scrape.claimBelowIntensity, 0, 1.0, 0.01, "Slide and scrape",
              "Claim below intensity",
              "Where the line above sits: under this intensity the slide keeps the contact, at "
              "or over it the impact path gets it too.\n"
              "Does nothing unless 'Slides dont claim' is on. Raise it and the grind keeps more "
              "- at 1.0 it keeps everything and the switch above stops meaning anything. Lower "
              "it and the heavy contacts of a grind - a spine or a thigh coming down at 0.3-0.5 "
              "intensity while the body slides - stop being spent as scrape grains and get "
              "built as impacts. Too low and a genuine skid starts thudding on every frame, "
              "which is what the scrape path exists to prevent."),


    // -- the impacts that ride on one -----------------------------------------
    Renamed(Renamed(RDS_PAIRS(AlgorithmConfig, "Slide", "fSlideTrimDb", kFloat,
                              motion.slide.gainTrimDb, -60, 12, 0.5, "Slide and scrape",
                              "Slide impact trim",
                              "How loud the impacts during a slide are. The grinding loops have "
                              "their own levels."),
                    "Phase", "fSlideTrimDb"),
            "Motion", "fSlideTrimDb"),
    Renamed(Renamed(RDS_PARAM(AlgorithmConfig, "Slide", "iSlideMaxCues", kInt,
                              motion.slide.maxCuesPerBurst, 0, 16, 1, "Slide and scrape",
                              "Slide grains",
                              "How many one-shots may punctuate a slide. This is the budget the "
                              "catches below are paid for out of."),
                    "Phase", "iSlideMaxCues"),
            "Motion", "iSlideMaxCues"),

    // -- the fades ------------------------------------------------------------
    Renamed(RDS_HPAIR(AlgorithmConfig, "Slide", "fStartFadeMs", kFloat,
                      strategies.scrape.startFadeMs, 0, 1000, 5, "Slide and scrape", "Fade in",
                      "How gently a grind arrives. Too short and it clicks in at the start of a "
                      "slide; long enough and it hides the detector's own latency, because a "
                      "slide is not declared until 150 ms or 45 units into a grind that has "
                      "already started.\n\n"
                      "**Testbench only today.** The game renderer bakes a loop's gain into its "
                      "buffer and never calls FadeInPlay, so in-game a grind still switches on "
                      "at its full level. See `GameRenderer::StartLoopVoice`."),
            "ScrapeLoop", "fStartFadeMs"),
    Renamed(RDS_PARAM(AlgorithmConfig, "Slide", "fStopFadeMs", kFloat,
                      strategies.scrape.stopFadeMs, 0, 2000, 5, "Slide and scrape", "Fade out",
                      "How gently it leaves when the slide ends on the ground. Short, because a "
                      "body grinding to a halt does not fade - it slows down, and the level "
                      "already tracks how fast it is going, so a long fade here is a second "
                      "ending laid over the real one.\n\n"
                      "What makes that true is the speed ramp reaching the voice floor by the "
                      "time the grind stops: see `fSpeedRangeDb`. Break that correspondence and "
                      "this fade is back to hiding a step."),
            "ScrapeLoop", "fStopFadeMs"),
    Renamed(RDS_PARAM(AlgorithmConfig, "Slide", "fLaunchFadeMs", kFloat,
                      strategies.scrape.launchFadeMs, 0, 2000, 5, "Slide and scrape",
                      "Launch fade",
                      "And how gently it leaves when the slide ended because the body went "
                      "airborne, which is faster: the surface is simply gone."),
            "ScrapeLoop", "fLaunchFadeMs"),

    // -- the body grind -------------------------------------------------------
    RDS_HRULE(AlgorithmConfig, "Slide", "bBodyEnabled", kBool, strategies.scrape.bodyEnabled, 0, 1,
              1, "Slide and scrape", "Body grind",
              "The heavy, full-weight grind: falling at a run and skidding several metres, or "
              "being dragged flat on your back. It runs alongside the limb grinds rather than "
              "instead of them, and the two cross-fade on how much of the body is touching."),
    Renamed(RDS_PARAM(AlgorithmConfig, "Slide", "fGainDb", kFloat, strategies.scrape.gainDb, -60, 6,
                      0.5, "Slide and scrape", "Body level",
                      "How loud the body grind sits under the impacts at full speed and full "
                      "weight. The references put a slide 15-25 dB under them."),
            "ScrapeLoop", "fGainDb"),
    Renamed(RDS_PAIRS(AlgorithmConfig, "Slide", "fSpeedForMinGain", kFloat,
                      strategies.scrape.speedForMinGain, 0, 2000, 10, "Slide and scrape",
                      "Body quiet at",
                      "The body speed the grind is quietest at - and, at or under it, stops. "
                      "Level and pitch track the measured centre of mass, so a slide is exactly "
                      "as loud as the body is fast, and at the bottom of that ramp there is "
                      "nothing left to be quiet about.\n\n"
                      "This is what ends a grind that has run out of speed. It used to be the "
                      "mix's voice floor doing that job, which only worked while the gains "
                      "happened to put the bottom of the ramp a decibel under it - move the body "
                      "level or the floor and the grind ran on at its quietest for as long as the "
                      "slide lasted, at one unchanging level. At 0 there is no bottom to reach "
                      "until the body has completely stopped, which is that same behaviour asked "
                      "for on purpose."),
            "ScrapeLoop", "fSpeedForMinGain"),
    Renamed(RDS_PARAM(AlgorithmConfig, "Slide", "fSpeedForMaxGain", kFloat,
                      strategies.scrape.speedForMaxGain, 0, 4000, 10, "Slide and scrape",
                      "Body loud at",
                      "And the speed it reaches full level at, so the grind tracks the slide "
                      "rather than sitting at one level throughout."),
            "ScrapeLoop", "fSpeedForMaxGain"),
    Renamed(RDS_PAIRS(AlgorithmConfig, "Slide", "fSpeedRangeDb", kFloat,
                      strategies.scrape.speedRangeDb, -60, 0, 0.5, "Slide and scrape",
                      "Body speed range",
                      "How far under its level the grind sits at the quiet end - the depth of the "
                      "speed dependence. At 0 the slide is one level whatever it is doing; deep "
                      "enough that a crawl is inaudible is what keeps a body settling on its side "
                      "from sounding like it is being dragged."),
            "ScrapeLoop", "fSpeedRangeDb"),
    Renamed(RDS_PARAM(AlgorithmConfig, "Slide", "fPitchPerThousandUnits", kFloat,
                      strategies.scrape.pitchPerThousandUnits, 0, 2, 0.01, "Slide and scrape",
                      "Body pitch with speed",
                      "How much faster the grind plays as the slide speeds up, per thousand "
                      "units/s of measured body speed. A little sells the acceleration; a lot "
                      "sounds like a tape being spooled."),
            "ScrapeLoop", "fPitchPerThousandUnits"),
    RDS_PARAM(AlgorithmConfig, "Slide", "bBodyFollowsContact", kBool,
              strategies.scrape.bodyFollowsContact, 0, 1, 1, "Slide and scrape",
              "Body grind follows the contact",
              "Hang the body grind on the bone nearest where the body is actually touching, "
              "rather than on the actor's root - which is roughly the pelvis, and is not where "
              "the sound is even for a real full-body slide. Nearest-to-contact rather than "
              "lowest on purpose: a body can grind along a wall, down a staircase or across a "
              "ceiling, and a rule that assumes the floor is below gets all three wrong. Still "
              "one single point, so nothing smears."),

    // -- how much body is on the floor ----------------------------------------
    RDS_HRULE(AlgorithmConfig, "Slide", "bFractionEnabled", kBool,
              strategies.scrape.fractionEnabled, 0, 1, 1, "Slide and scrape",
              "Weigh the body grind by contact",
              "Add up the anatomical weight of the limbs that are actually rubbing and let that "
              "decide how much body grind there is.\n"
              "This is the fix for \"it fires on almost nothing and then plays at full body "
              "volume\". Nothing in the mod ever measured how much of the body was touching, so "
              "the level stood on speed alone and a corpse dragged by one ankle read exactly as "
              "loud as the same corpse lying flat. One foot is about 1.5 % of a body; both feet "
              "and both shins about 13 %; flat on your back and skidding is 60 % and up. Off, the "
              "level is speed alone again, which is the old behaviour."),
    RDS_PAIRS(AlgorithmConfig, "Slide", "fBodyFracStart", kFloat, strategies.scrape.bodyFracStart,
              0, 1, 0.01, "Slide and scrape", "Body grind from",
              "The share of the body that has to be touching before the body grind plays at all. "
              "Below this the limb grinds are the whole of the slide."),
    RDS_PARAM(AlgorithmConfig, "Slide", "fBodyFracFull", kFloat, strategies.scrape.bodyFracFull, 0,
              1, 0.01, "Slide and scrape", "Body grind full at",
              "And the share at which it is at full weight."),
    RDS_PAIRS(AlgorithmConfig, "Slide", "fBodyFracRangeDb", kFloat,
              strategies.scrape.bodyFracRangeDb, -60, 0, 0.5, "Slide and scrape",
              "Body weight range",
              "How far under its level the body grind sits at the bottom of that ramp. Deep, "
              "because just over the threshold it should be barely there - crossing it must not "
              "be an event in itself."),
    RDS_PARAM(AlgorithmConfig, "Slide", "fContactHoldMs", kFloat, strategies.scrape.contactHoldMs,
              0, 1000, 5, "Slide and scrape", "Contact hold",
              "How long a limb goes on counting as touching after its last rub. Contacts are "
              "dense when a fall is busy and absent when it is not, so without a hold the "
              "measurement flickers at solver rate and the grind comes out as a tremolo."),

    // -- the limb grinds ------------------------------------------------------
    RDS_HRULE(AlgorithmConfig, "Slide", "bLimbEnabled", kBool, strategies.scrape.limbEnabled, 0, 1,
              1, "Slide and scrape", "Limb grinds",
              "The light, dry grind of one limb dragging: a foot, a trailing hand, a forearm. One "
              "per limb - two arms, two legs and the head - so several limbs scraping at once are "
              "several sounds, each on its own bone. The torso has none, because a spine on the "
              "floor is the body sliding and that is the body grind's subject."),
    RDS_PAIRS(AlgorithmConfig, "Slide", "iMaxLimbLoops", kInt, strategies.scrape.maxLimbLoops, 0, 5,
              1, "Slide and scrape", "Limb grinds at once",
              "How many of the five may run together, given to whichever limbs are rubbing "
              "hardest. Purely a judgement about the mix - a voice costs nothing the engine "
              "cannot afford - and the default is under five because five limbs grinding at "
              "once is more sound than the picture supports."),
    RDS_PARAM(AlgorithmConfig, "Slide", "fLimbMinTangentSpeed", kFloat,
              strategies.scrape.limbMinTangentSpeed, 0, 2000, 10, "Slide and scrape",
              "Limb grind from",
              "How hard a limb has to be rubbing before it gets a grind of its own."),
    RDS_PARAM(AlgorithmConfig, "Slide", "fLimbHoldTangentSpeed", kFloat,
              strategies.scrape.limbHoldTangentSpeed, 0, 2000, 10, "Slide and scrape",
              "Limb grind holds to",
              "How hard a limb with a grind already running has to stay rubbing to keep it. Below "
              "'Limb grind from', so a loop is not lost to every dip another limb dips under."),
    RDS_PARAM(AlgorithmConfig, "Slide", "fLimbGainDb", kFloat, strategies.scrape.limbGainDb, -60, 6,
              0.5, "Slide and scrape", "Limb level",
              "How loud one limb dragging is. A single foot should be only just audible at "
              "conversational distance - closer to the cloth bed than to the body grind - and it "
              "is easy to overshoot here, where you are listening for it deliberately."),
    RDS_PAIRS(AlgorithmConfig, "Slide", "fLimbSpeedForMinGain", kFloat,
              strategies.scrape.limbSpeedForMinGain, 0, 2000, 10, "Slide and scrape",
              "Limb quiet at",
              "The limb grind's own speed response, measured on how fast that limb is rubbing "
              "rather than on the body - a dragging foot behind a body that has otherwise stopped "
              "is still a dragging foot."),
    RDS_PARAM(AlgorithmConfig, "Slide", "fLimbSpeedForMaxGain", kFloat,
              strategies.scrape.limbSpeedForMaxGain, 0, 4000, 10, "Slide and scrape",
              "Limb loud at", "And the rubbing speed it reaches full level at."),
    RDS_PAIRS(AlgorithmConfig, "Slide", "fLimbSpeedRangeDb", kFloat,
              strategies.scrape.limbSpeedRangeDb, -60, 0, 0.5, "Slide and scrape",
              "Limb speed range", "The depth of that dependence, as for the body grind."),
    RDS_PARAM(AlgorithmConfig, "Slide", "fLimbPitchPerThousandUnits", kFloat,
              strategies.scrape.limbPitchPerThousandUnits, 0, 2, 0.01, "Slide and scrape",
              "Limb pitch with speed",
              "How much faster a limb grind plays as the limb speeds up. A small contact patch "
              "reads its speed more in pitch than a whole body does, so this can sit above the "
              "body's."),
    RDS_PAIRS(AlgorithmConfig, "Slide", "bLimbFollowsLimb", kBool,
              strategies.scrape.limbFollowsLimb, 0, 1, 1, "Slide and scrape",
              "Limb grind follows the limb",
              "Play a limb grind from the limb that is making it. Off, it collapses onto the body "
              "the way every loop used to - which is what put your own scrape inside your head, "
              "since the player's ragdoll is at arm's length and a collapse to one point there is "
              "not spatial at all."),
    RDS_PARAM(AlgorithmConfig, "Slide", "fLimbHoldMs", kFloat, strategies.scrape.limbHoldMs, 0,
              2000, 10, "Slide and scrape", "Limb hold",
              "How long a different bone in the same limb has to be the one rubbing before the "
              "grind moves onto it. This is what stops a scrape smearing as the contact hops from "
              "forearm to hand and back frame by frame; short and it wanders, long and it lags "
              "behind what you can see."),

    // -- the head -------------------------------------------------------------
    RDS_HRULE(AlgorithmConfig, "Slide", "bHeadTint", kBool, strategies.scrape.headTint, 0, 1, 1,
              "Slide and scrape", "Tint the head grind",
              "A skull dragging is a small contact patch, so it plays the limb file rather than "
              "the body one - but it is a skull. Rather than a whole extra sound, the head's "
              "grind is the same file pitched down a little with its own trim, which is the same "
              "trick the head impact uses to earn its character. Off, the head grinds exactly "
              "like an arm."),
    RDS_PAIRS(AlgorithmConfig, "Slide", "fHeadPitchScale", kFloat,
              strategies.scrape.headPitchScale, 0.5, 1.5, 0.01, "Slide and scrape",
              "Head pitch", "How far down. 1 is no tint at all."),
    RDS_PARAM(AlgorithmConfig, "Slide", "fHeadGainDb", kFloat, strategies.scrape.headGainDb, -24,
              12, 0.5, "Slide and scrape", "Head trim",
              "And what the head grind is worth against the other limbs."),

    // -- one over the other ---------------------------------------------------
    RDS_HPAIR(AlgorithmConfig, "Slide", "bBodyDucksLimbs", kBool,
              strategies.scrape.bodyDucksLimbs, 0, 1, 1, "Slide and scrape",
              "Body grind ducks the limbs",
              "Pull the limb grinds down under the body grind as it comes in. Both play together "
              "by design, but a full-weight slide with four limb grinds still going on top of it "
              "is more sound than the picture supports. Scaled by the body grind's own weight, so "
              "it arrives with the body rather than switching on."),
    RDS_PARAM(AlgorithmConfig, "Slide", "fLimbDuckDb", kFloat, strategies.scrape.limbDuckDb, -60, 0,
              0.5, "Slide and scrape", "Limb duck",
              "How far down, at full body weight. Deep enough is suppression rather than damping: "
              "a limb grind ducked under the mix's own voice floor is stopped outright and gives "
              "its voice back, so this one slider covers both."),

    // -- the entry scuff ------------------------------------------------------
    //
    // Three sliders left this block rather than being defaulted off:
    // `fGrainCatchRatio`, `fGrainMinGapMs` and `fGrainProbability` were a
    // threshold, a rate limit and a dice roll for a *stream* of catches, and none
    // of the three means anything for one grain per grind. Dropped without a
    // `Renamed`, so an ini tuned for the old layer does not carry a value into a
    // feature it was never about.
    RDS_HRULE(AlgorithmConfig, "Slide", "bGrainEnabled", kBool, strategies.scrape.grainEnabled, 0,
              1, 1, "Slide and scrape", "Entry scuff",
              "Play a short scrape grain the moment a grind starts - the scuff of the limb "
              "arriving on the surface, under the head of the loop it introduces.\n"
              "A slide is not declared until 150 ms or 45 units into a grind that is already "
              "happening, so the loop always opens into a body that has been scraping for a "
              "moment, and it used to open with nothing marking the arrival. That is the "
              "'somebody turned a noise on' this fixes, and it is the same shape as the transient "
              "in front of an impact's sub.\n"
              "This is what the catch layer became. It used to fire all the way through a slide, "
              "on any rub harder than the slide's own average: sixty-five grit peaks a second is "
              "what a real slide has, but at cue rate that arrives as a rattle of separate little "
              "impacts over a grind rather than as grit in it. The density belongs to the file "
              "now; the entry is the one moment in a slide that is genuinely an event."),
    RDS_PAIRS(AlgorithmConfig, "Slide", "fGrainGainDb", kFloat, strategies.scrape.grainGainDb, -60,
              6, 0.5, "Slide and scrape", "Scuff level",
              "How loud the scuff is against the grind it introduces. Against the loop's level "
              "and not a contact's, so it scales with the grind: a limb barely dragging scuffs "
              "quietly and a body arriving at speed scuffs hard, with nothing to tune twice."),
    RDS_PARAM(AlgorithmConfig, "Slide", "fGrainPitchScatter", kFloat,
              strategies.scrape.grainPitchScatter, 0, 1, 0.01, "Slide and scrape",
              "Scuff pitch scatter",
              "How far either way a scuff's pitch is thrown. It still earns its place at one per "
              "grind: a body that grinds, launches and lands three times in a fall enters three "
              "times, and three entries at one pitch read as one sample repeating."),
    RDS_PARAM(AlgorithmConfig, "Slide", "bGrainOnBody", kBool, strategies.scrape.grainOnBody, 0, 1,
              1, "Slide and scrape", "Also scuff the body grind",
              "Give the body grind an entry scuff as well as the limb grinds.\n"
              "Off by default, and that is about the picture rather than caution. A limb arriving "
              "is a scuff - a foot catching, a hand slapping down and dragging - and has a real "
              "edge to it. A torso arriving flat is not a scuff, it is a fall, and the impact "
              "composite has already voiced it: a knockdown that ends in a skid has just put a "
              "full stack with a sub on it into the same 100 ms, and a scuff on top is a fifth "
              "layer on the loudest moment in the mod.\n"
              "Turn it on for the entry the composite does not cover: a body already down and "
              "still, then dragged. There is no collision there for the impact path to have "
              "voiced, so the grind opens out of silence - which is the case this layer is for."),

    // -- how still the level sits ---------------------------------------------
    RDS_HPAIR(AlgorithmConfig, "Slide", "fContactSpeedBlend", kFloat,
              strategies.scrape.contactSpeedBlend, 0, 1, 0.05, "Slide and scrape", "Level wobble",
              "How much of the contact's own speed is blended into the level's.\n"
              "The body's speed is smooth by nature, so a level that follows it alone reads as a "
              "constant however correct it is. Contact speed is genuinely spiky, because limbs "
              "load and unload as a body tumbles, and a little of it makes the grind breathe. A "
              "wobble around the body speed and never a replacement for it: the level used to be "
              "driven purely by contact speed, which is the speed of a limb rather than of a "
              "body, and that was wrong. 0 is the pure body speed."),
    RDS_PARAM(AlgorithmConfig, "Slide", "fLevelDeadbandDb", kFloat,
              strategies.scrape.levelDeadbandDb, 0, 6, 0.05, "Slide and scrape", "Level deadband",
              "How far the level has to move before a running grind is told about it. The shared "
              "figure is three-quarters of a decibel, which is most of what a breathing grind "
              "does - the loop was being held flat by the thing meant to keep the cue list "
              "readable."),

    // -- the rumble bed -------------------------------------------------------
    RDS_HRULE(AlgorithmConfig, "Slide", "bRumbleEnabled", kBool, strategies.scrape.rumbleEnabled, 0,
              1, 1, "Slide and scrape", "Rumble bed",
              "Hold a low bed under the grinds for as long as anything is grinding: the mass of "
              "the floor being loaded by a body crossing it, with none of the grit that rides on "
              "top.\n"
              "This is the missing half of the slide, and it is measured rather than guessed. "
              "Against GTA 4's slide events our grinds are 35-45 dB out on the bass-to-hiss "
              "balance and in the opposite direction - theirs bass-led with the sub band loudest "
              "and a hard rolloff over 8 kHz, ours broadband and flat to 20 kHz with the sub 40 dB "
              "down. The grain rates match, so density was never the problem. And no EQ rescues "
              "it: there is nothing under the shelf to boost.\n"
              "One voice for the whole actor rather than one per grind, because the floor is one "
              "object - and its own voice rather than baked into the six grind files, because the "
              "bed and the grit want opposite things from the runtime: the grit's pitch tracks "
              "speed and the bed's must not. Off, the slide is the grinds alone, which is the "
              "fastest A/B for whether this is what fixed it."),
    RDS_PARAM(AlgorithmConfig, "Slide", "fRumbleGainDb", kFloat, strategies.scrape.rumbleGainDb,
              -60, 6, 0.5, "Slide and scrape", "Bed level",
              "How loud the bed is at the top of its ramp. Over the body grind's own level rather "
              "than under it, which looks wrong and is the point: in the references the sub band "
              "is the loudest band of a slide and the grit rides on top of it. The same inversion "
              "the impact sub has against its transient, for the same reason - the layer carrying "
              "the mass is not the layer carrying the character.\n"
              "The bed has no depth or curve of its own to set beside this. It rides the limb "
              "grinds' ramp - 'Limb quiet at', 'Limb loud at' and 'Limb speed range' - measured "
              "on how hard the body is rubbing rather than on how fast it is travelling. Body "
              "speed is smooth by definition, and a bed levelled on it read as a switch rather "
              "than a swell. Tuning the limb grinds' depth now moves the bed's with it, which is "
              "the price of not having two knobs over one quantity."),
    RDS_PAIRS(AlgorithmConfig, "Slide", "fRumblePitch", kFloat, strategies.scrape.rumblePitch, 0.25,
              2, 0.01, "Slide and scrape", "Bed pitch",
              "A fixed pitch for the bed. Leave it at 1 unless the recording itself needs "
              "transposing to sit right - this is a tuning of the file, not a response to "
              "anything the body is doing."),
    RDS_PARAM(AlgorithmConfig, "Slide", "fRumblePitchPerThousandUnits", kFloat,
              strategies.scrape.rumblePitchPerThousandUnits, 0, 2, 0.01, "Slide and scrape",
              "Bed pitch with speed",
              "The one ramp in this section that is deliberately flat, exposed so the claim can "
              "be tested by ear instead of believed. Floor and body resonance do not move with "
              "how fast the body is going - the reference slides hold a static spectrum and swell "
              "in level - and pitching bass down at a crawl is flubby, blooms on a subwoofer and "
              "vanishes on a laptop speaker. 0 is the design's answer; the flubbiness arrives well "
              "before the movement reads as speed."),
    RDS_PAIRS(AlgorithmConfig, "Slide", "bRumbleOnLimbs", kBool, strategies.scrape.rumbleOnLimbs, 0,
              1, 1, "Slide and scrape", "Bed under limb grinds",
              "Whether a limb-only slide gets a bed at all. A single dragging foot is a small "
              "contact patch, and a small contact patch still loads the floor - the difference "
              "between a foot and a torso is how hard, not whether."),
    RDS_PARAM(AlgorithmConfig, "Slide", "fRumbleLimbGainDb", kFloat,
              strategies.scrape.rumbleLimbGainDb, -60, 0, 0.5, "Slide and scrape", "Bed limb trim",
              "How much less bed a limb-only slide gets. Interpolated on the same contact fraction "
              "the body grind's weight uses, so it arrives with the body rather than switching: "
              "all of this at 'Body grind from' and none of it at 'Body grind full at'. One file, "
              "one voice, one number between the two cases.\n"
              "With no body grind running at all - the switch off, or under 'Body grind from', or "
              "the body slower than 'Body quiet at' - the whole of this is applied however much of "
              "the body happens to be touching. Otherwise a body lying flat with the body grind "
              "switched off measures a full contact fraction, cancels this trim, and plays the bed "
              "at full body level under nothing but limb grinds."),

    // -- the floor ------------------------------------------------------------
    RDS_HRULE(AlgorithmConfig, "Slide", "bSurfaceVariants", kBool,
              strategies.scrape.surfaceVariants, 0, 1, 1, "Slide and scrape", "Surface variants",
              "Let the grinds pick a file for the floor they are on - stone, wood, or the default "
              "for everything else. Off, everything plays the default grind, which is what the "
              "mod did on flagstone, floorboards, dirt, snow and ice alike. A surface with no "
              "recording behind it falls back to the default anyway, so this costs nothing until "
              "the files exist."),

    // -- Stage 3: MotionFoley -------------------------------------------------
    RDS_PARAM(AlgorithmConfig, "MotionFoley", "bEnabled", kBool, strategies.foley.enabled, 0, 1, 1,
              "Motion foley", "Enabled",
              "The airborne anticipation whoosh. This section used to own a continuous cloth "
              "bed as well; it was muted in every saved config and went with its slot."),
    RDS_PARAM(AlgorithmConfig, "MotionFoley", "bAirborneRise", kBool, strategies.foley.airborneRise,
              0, 1, 1, "Motion foley", "Airborne rise",
              "The anticipation whoosh while a body is in the air. On by default at a low level: "
              "it tells the ear something is about to land."),
    RDS_PARAM(AlgorithmConfig, "MotionFoley", "fAirborneRiseGainDb", kFloat,
              strategies.foley.airborneRiseGainDb, -80, 6, 0.5, "Motion foley", "Rise level",
              "How loud the whoosh is. It should be felt rather than heard."),

    // -- Stage 3: AccumDamage -------------------------------------------------
    RDS_HRULE(AlgorithmConfig, "DamageAccum", "bEnabled", kBool, strategies.accum.enabled, 0, 1, 1,
              "Accumulated damage", "Enabled",
              "Damage from being worked on rather than from one bad landing. Every contact adds "
              "to a pool on its own limb, the pool heals over seconds, and crossing each rung of "
              "a ladder plays a break. It exists because the tiers cannot see repetition: in the "
              "wall-bashing take a head takes twenty-four hits whose hardest is 371 u/s against a "
              "432 gate, so every tier correctly refuses every one of them and the skull never "
              "comes apart. Off by default and off changes nothing."),
    RDS_PARAM(AlgorithmConfig, "DamageAccum", "bHeadOnly", kBool, strategies.accum.headOnly, 0, 1,
              1, "Accumulated damage", "Head only",
              "Only the head and neck accumulate, and only they break. Left open, every limb of a "
              "long tumble is being 'worked on' and the knockdown ends in a bag of breaking "
              "sticks; closed to the head this is what it was written for - a skull bashed against "
              "something until it gives way. The other limbs stop pooling entirely rather than "
              "pooling silently, so nothing is banked against the moment this is turned off."),
    RDS_PARAM(AlgorithmConfig, "DamageAccum", "bRequireHeld", kBool, strategies.accum.requireHeld,
              0, 1, 1, "Accumulated damage", "Only while held",
              "Only while the player has this body in their hands. The case the ladder exists for "
              "is a VR player holding a ragdoll and driving its head into a wall - a thing the "
              "player does, rather than a thing that happens to a body - and everything else a "
              "knockdown contains should still sound like a fall. Needs VR and HIGGS: on flat "
              "Skyrim, without HIGGS, and on recordings made before hold rows existed nothing is "
              "ever held, so turning this on there switches the ladder off."),
    RDS_PARAM(AlgorithmConfig, "DamageAccum", "fHealMs", kFloat, strategies.accum.healMs, 100,
              30000, 100, "Accumulated damage", "Heal time",
              "How long the pool takes to drain. An injury healing, not an envelope releasing: "
              "long enough to bridge the second or so between one blow and the next, short enough "
              "that two separate falls are two separate histories."),
    RDS_HPAIR(AlgorithmConfig, "DamageAccum", "fIgnoreBelowIntensity", kFloat,
              strategies.accum.ignoreBelowIntensity, 0, 1, 0.01, "Accumulated damage",
              "Ignore below",
              "Contacts under this intensity add nothing. Without it the settling scrabble at the "
              "end of every knockdown fills the pool on its own, and a body that has come to rest "
              "eventually breaks a bone for no reason anyone watching can see."),
    RDS_PARAM(AlgorithmConfig, "DamageAccum", "fPerHitScale", kFloat, strategies.accum.perHitScale,
              0, 10, 0.05, "Accumulated damage", "Per-hit scale",
              "What each qualifying contact adds, as a multiple of how far it clears the floor. "
              "The knob for making a limb wear out faster without moving the ladder."),
    RDS_PARAM(AlgorithmConfig, "DamageAccum", "fMaxPool", kFloat, strategies.accum.maxPool, 0.1,
              40, 0.1, "Accumulated damage", "Pool ceiling",
              "How much damage one limb can hold. A limb worked on indefinitely stops climbing "
              "rather than banking damage to spend later, which is what stops a long beating from "
              "firing the rest of the ladder the instant a rung is reached."),
    RDS_HPAIR(AlgorithmConfig, "DamageAccum", "fBreakIntensity", kFloat,
              strategies.accum.breakIntensity, 0, 1, 0.01, "Accumulated damage", "Break intensity",
              "How hard the blow that actually breaks the limb has to be. Reaching a rung only "
              "*arms* a limb: it then sits there taking damage and making no sound until a "
              "contact with real weight behind it lands. Without it the break falls on whichever "
              "contact happened to tip the arithmetic over, which in a beating is usually a "
              "nothing - the twenty-fourth gentle scuff, no different from the twenty-third. A "
              "bone going is the loudest thing here and it should land on a hit you can see."),
    RDS_PARAM(AlgorithmConfig, "DamageAccum", "fRearmFrac", kFloat, strategies.accum.rearmFrac, 0,
              1, 0.05, "Accumulated damage", "Re-arm margin",
              "How far a limb has to recover before a rung it already fired can fire again, as a "
              "fraction of that rung. Load-bearing rather than a refinement: at 1 a pool resting "
              "near a threshold steps down and back up on alternate contacts and cracks the same "
              "bone on a loop - 51 breaks on one take before this existed. 0 means a rung never "
              "re-arms within a knockdown."),
    RDS_HPAIR(AlgorithmConfig, "DamageAccum", "fMinGapMs", kFloat, strategies.accum.minGapMs, 0,
              5000, 10, "Accumulated damage", "Minimum gap",
              "The floor under how often one limb may produce a break. The rungs space themselves; "
              "this guards the case they cannot, which is a limb sitting exactly on a threshold "
              "while contacts keep arriving."),
    RDS_PAIRS(AlgorithmConfig, "DamageAccum", "iMaxPerActor", kInt, strategies.accum.maxPerActor,
              0, 64, 1, "Accumulated damage", "Breaks per body",
              "How many breaks the ladder may produce across the whole body in one knockdown. "
              "Not a formality: eighteen limbs each allowed their own budget is a hundred-odd, "
              "and the first pass duly produced 132 breaks on one long tumble. The ladder is the "
              "rare awful thing that happens when a body is worked on, and a fall that brushes "
              "every limb is not that."),
    RDS_PARAM(AlgorithmConfig, "DamageAccum", "iMaxPerLimb", kInt, strategies.accum.maxPerLimb, 0,
              32, 1, "Accumulated damage", "Breaks per limb",
              "How many breaks one limb may produce in a knockdown. Charged per limb, so a body "
              "worked over everywhere is allowed to sound like it."),
    RDS_HPAIR(AlgorithmConfig, "DamageAccum", "fStage1AtDamage", kFloat,
              strategies.accum.stage1.atDamage, 0, 20, 0.05, "Accumulated damage",
              "Rung 1: damage", "How worked-over a limb has to be before rung 1 fires - the first thing that gives. 0 removes the rung, which is how a three-step ladder is asked for."),
    RDS_PARAM(AlgorithmConfig, "DamageAccum", "bStage1Gore", kBool,
              strategies.accum.stage1.gore, 0, 1, 1, "Accumulated damage", "Rung 1: gore",
              "Off plays the part's crunch, on plays the gore. Choosing these is how the ladder gets its shape: the default is crunch, crunch, gore, louder gore."),
    RDS_PARAM(AlgorithmConfig, "DamageAccum", "fStage1LevelDb", kFloat,
              strategies.accum.stage1.levelDb, -40, 20, 0.5, "Accumulated damage",
              "Rung 1: level", "On top of the contact's own level. Rising up the ladder is what makes the last rung the one you flinch at."),
    RDS_HPAIR(AlgorithmConfig, "DamageAccum", "fStage2AtDamage", kFloat,
              strategies.accum.stage2.atDamage, 0, 20, 0.05, "Accumulated damage",
              "Rung 2: damage", "How worked-over a limb has to be before rung 2 fires - a second break as it is worked on. 0 removes the rung, which is how a three-step ladder is asked for."),
    RDS_PARAM(AlgorithmConfig, "DamageAccum", "bStage2Gore", kBool,
              strategies.accum.stage2.gore, 0, 1, 1, "Accumulated damage", "Rung 2: gore",
              "Off plays the part's crunch, on plays the gore. Choosing these is how the ladder gets its shape: the default is crunch, crunch, gore, louder gore."),
    RDS_PARAM(AlgorithmConfig, "DamageAccum", "fStage2LevelDb", kFloat,
              strategies.accum.stage2.levelDb, -40, 20, 0.5, "Accumulated damage",
              "Rung 2: level", "On top of the contact's own level. Rising up the ladder is what makes the last rung the one you flinch at."),
    RDS_HPAIR(AlgorithmConfig, "DamageAccum", "fStage3AtDamage", kFloat,
              strategies.accum.stage3.atDamage, 0, 20, 0.05, "Accumulated damage",
              "Rung 3: damage", "How worked-over a limb has to be before rung 3 fires - it stops being a break and starts being a mess. 0 removes the rung, which is how a three-step ladder is asked for."),
    RDS_PARAM(AlgorithmConfig, "DamageAccum", "bStage3Gore", kBool,
              strategies.accum.stage3.gore, 0, 1, 1, "Accumulated damage", "Rung 3: gore",
              "Off plays the part's crunch, on plays the gore. Choosing these is how the ladder gets its shape: the default is crunch, crunch, gore, louder gore."),
    RDS_PARAM(AlgorithmConfig, "DamageAccum", "fStage3LevelDb", kFloat,
              strategies.accum.stage3.levelDb, -40, 20, 0.5, "Accumulated damage",
              "Rung 3: level", "On top of the contact's own level. Rising up the ladder is what makes the last rung the one you flinch at."),
    RDS_HPAIR(AlgorithmConfig, "DamageAccum", "fStage4AtDamage", kFloat,
              strategies.accum.stage4.atDamage, 0, 20, 0.05, "Accumulated damage",
              "Rung 4: damage", "How worked-over a limb has to be before rung 4 fires - the loud one, and the top of the ladder. 0 removes the rung, which is how a three-step ladder is asked for."),
    RDS_PARAM(AlgorithmConfig, "DamageAccum", "bStage4Gore", kBool,
              strategies.accum.stage4.gore, 0, 1, 1, "Accumulated damage", "Rung 4: gore",
              "Off plays the part's crunch, on plays the gore. Choosing these is how the ladder gets its shape: the default is crunch, crunch, gore, louder gore."),
    RDS_PARAM(AlgorithmConfig, "DamageAccum", "fStage4LevelDb", kFloat,
              strategies.accum.stage4.levelDb, -40, 20, 0.5, "Accumulated damage",
              "Rung 4: level", "On top of the contact's own level. Rising up the ladder is what makes the last rung the one you flinch at."),

    // -- Stage 3: Rustle ------------------------------------------------------
    RDS_PARAM(AlgorithmConfig, "Rustle", "bEnabled", kBool, strategies.rustle.enabled, 0, 1, 1,
              "Rustle", "Enabled",
              "The garment: a continuous fabric and armour layer riding a knockdown, loudest "
              "while the limbs are thrashing and the body is turning over. Off by default, and "
              "off changes nothing at all - nothing is measured and no cue is emitted. Every "
              "slider under it is already set to a usable voicing, so this switch is the whole "
              "of turning it on."),

    RDS_HRULE(AlgorithmConfig, "Rustle", "fThrashWeight", kFloat, strategies.rustle.thrashWeight,
              0, 4, 0.05, "Rustle", "Thrash weight",
              "How much of the level comes from the limbs accelerating relative to the body. "
              "This is the signal - and the reason the layer is not driven off the body's own "
              "acceleration, which barely moves between the treads of a staircase while the "
              "limbs are being flung about."),
    RDS_HPAIR(AlgorithmConfig, "Rustle", "fThrashFloor", kFloat, strategies.rustle.thrashFloor, 0,
              4000, 10, "Rustle", "Thrash floor",
              "Relative limb acceleration, u/s2, below which there is no rustle at all. The "
              "number that keeps a body lying still silent, and the first one to set by ear."),
    RDS_PARAM(AlgorithmConfig, "Rustle", "fThrashFull", kFloat, strategies.rustle.thrashFull, 10,
              8000, 10, "Rustle", "Thrash full",
              "Where the thrash term reads maximum. Set it near a tumble's typical peak, not "
              "near an impact's: a limb landing measured 5110 u/s2, and a range stretched to "
              "hold one flattens the whole tumble to nothing."),
    RDS_PARAM(AlgorithmConfig, "Rustle", "fTumbleWeight", kFloat, strategies.rustle.tumbleWeight, 0,
              4, 0.05, "Rustle", "Tumble weight",
              "How much of the level comes from the limbs rotating. A limb spinning at a steady "
              "rate drags its sleeve continuously with no acceleration at all, which is what a "
              "fall down stairs is mostly made of - so this cannot be folded into the thrash."),
    RDS_HPAIR(AlgorithmConfig, "Rustle", "fTumbleFloor", kFloat, strategies.rustle.tumbleFloor, 0,
              2000, 5, "Rustle", "Tumble floor",
              "Limb surface speed from rotation, u/s, below which rotation contributes nothing."),
    RDS_PARAM(AlgorithmConfig, "Rustle", "fTumbleFull", kFloat, strategies.rustle.tumbleFull, 10,
              4000, 10, "Rustle", "Tumble full",
              "Where the rotation term reads maximum."),
    RDS_PARAM(AlgorithmConfig, "Rustle", "fThrashCeiling", kFloat, strategies.rustle.thrashCeiling,
              100, 20000, 50, "Rustle", "Thrash ceiling",
              "Per-limb clamp on acceleration before the sum. Guards against a solver blow-up "
              "making the layer scream, and does the ordinary job too: one limb hitting stone is "
              "held to something sane while the rest contribute their real motion, so an impact "
              "lifts the level instead of pinning it."),
    RDS_PARAM(AlgorithmConfig, "Rustle", "bRelativeToBody", kBool,
              strategies.rustle.relativeToBody, 0, 1, 1, "Rustle", "Relative to body",
              "Subtract the body's own acceleration from each limb's. On is right - fabric moves "
              "when a limb moves relative to the body it is on, and everything on a falling body "
              "accelerates together. Off is the naive version, kept so the difference is "
              "audible."),
    RDS_HPAIR(AlgorithmConfig, "Rustle", "fSpeedWeight", kFloat, strategies.rustle.speedWeight, 0,
              3, 0.05, "Rustle", "Speed weight",
              "How much a fast body multiplies everything above. A multiplier and never an "
              "addend: the same thrash at speed moves more cloth further, but a body drifting "
              "fast and limply must stay silent. 0 is pure thrash and tumble."),
    RDS_PARAM(AlgorithmConfig, "Rustle", "fSpeedForFull", kFloat, strategies.rustle.speedForFull,
              10, 3000, 10, "Rustle", "Speed for full",
              "Body speed, u/s, at which the speed multiplier is at its maximum."),
    RDS_HPAIR(AlgorithmConfig, "Rustle", "fAirWeight", kFloat, strategies.rustle.airWeight, 0, 2,
              0.05, "Rustle", "Air weight",
              "How much a free fall adds on its own. Off by default: a cloak does flap in a long "
              "drop, but the airborne whoosh already covers that state and two voices saying "
              "'this body is falling' is one too many. Turn it up to make the comparison."),
    RDS_PARAM(AlgorithmConfig, "Rustle", "fAirSpeedForFull", kFloat,
              strategies.rustle.airSpeedForFull, 10, 3000, 10, "Rustle", "Air speed for full",
              "Fall speed, u/s, at which the airborne term is at its maximum."),
    RDS_PARAM(AlgorithmConfig, "Rustle", "fSilenceDrive", kFloat, strategies.rustle.silenceDrive, 0,
              0.5, 0.005, "Rustle", "Silence below",
              "How quiet the layer has to get before its voice is stopped rather than held at "
              "silence. There is no separate hold time - the release is the hold, and a second "
              "timer would be the same delay expressed twice."),

    RDS_HPAIR(AlgorithmConfig, "Rustle", "fAttackMs", kFloat, strategies.rustle.attackMs, 1, 500,
              1, "Rustle", "Attack",
              "How fast the level rises. Short: cloth responds immediately, and a slow attack "
              "puts the rustle behind the impact that caused it."),
    RDS_PARAM(AlgorithmConfig, "Rustle", "fReleaseMs", kFloat, strategies.rustle.releaseMs, 1, 2000,
              5, "Rustle", "Release",
              "How slowly it falls, and the single most characterful number here. It fills the "
              "gaps between bounces so a tumble reads as continuous rather than as a rattle, and "
              "it leaves a decaying fabric tail behind every impact - clothing settling after a "
              "hit, with nothing inferred to produce it."),
    RDS_HPAIR(AlgorithmConfig, "Rustle", "fPitchAtFloor", kFloat, strategies.rustle.pitchAtFloor,
              0.5, 2, 0.01, "Rustle", "Pitch at floor",
              "The loop's pitch when barely moving. Keep the range narrow - a garment does not "
              "change pitch much, and a wide sweep is the fastest way to sound synthetic."),
    RDS_PARAM(AlgorithmConfig, "Rustle", "fPitchAtFull", kFloat, strategies.rustle.pitchAtFull, 0.5,
              2, 0.01, "Rustle", "Pitch at full",
              "The loop's pitch at full drive."),
    RDS_HPAIR(AlgorithmConfig, "Rustle", "fWanderDepthDb", kFloat, strategies.rustle.wanderDepthDb,
              0, 12, 0.1, "Rustle", "Wander depth",
              "A slow wobble on the level so a two-second file does not read as a two-second "
              "file. Deterministic, not random - the engine has to stay reproducible."),
    RDS_PARAM(AlgorithmConfig, "Rustle", "fWanderHz", kFloat, strategies.rustle.wanderHz, 0.01, 5,
              0.01, "Rustle", "Wander rate",
              "How slow the wobble is."),
    RDS_PARAM(AlgorithmConfig, "Rustle", "fLevelDeadbandDb", kFloat,
              strategies.rustle.levelDeadbandDb, 0, 6, 0.05, "Rustle", "Level deadband",
              "How far the level has to move before the running loop is told about it. Without "
              "one a loop emits an update every frame and buries the cue list."),
    RDS_HPAIR(AlgorithmConfig, "Rustle", "fStartFadeMs", kFloat, strategies.rustle.startFadeMs, 0,
              2000, 5, "Rustle", "Start fade",
              "Longer than the grind's. Fabric has no transient, and a rustle that snaps in is "
              "the most obvious thing in the mix."),
    RDS_PARAM(AlgorithmConfig, "Rustle", "fStopFadeMs", kFloat, strategies.rustle.stopFadeMs, 0,
              4000, 5, "Rustle", "Stop fade",
              "How long the layer takes to go once it is under the silence threshold."),

    RDS_HRULE(AlgorithmConfig, "Rustle", "fGainDb", kFloat, strategies.rustle.gainDb, -80, 6, 0.5,
              "Rustle", "Level",
              "The layer's level at full drive. It is a bed - felt more than heard, and well "
              "under the impacts it plays between."),
    RDS_PARAM(AlgorithmConfig, "Rustle", "fDriveRangeDb", kFloat, strategies.rustle.driveRangeDb,
              -60, 0, 0.5, "Rustle", "Drive range",
              "How far under the level the layer sits at the bottom of its ramp. Deep, so "
              "crossing the floor is not an event in itself."),
    RDS_HPAIR(AlgorithmConfig, "Rustle", "fBareTrimDb", kFloat, strategies.rustle.bareTrimDb, -40,
              20, 0.5, "Rustle", "Bare trim",
              "Naked skin. Cut hard by default: with one slot carrying every armour class there "
              "is no empty slot to make a bare body silent, so this is what does it."),
    RDS_PARAM(AlgorithmConfig, "Rustle", "fClothTrimDb", kFloat, strategies.rustle.clothTrimDb, -40,
              20, 0.5, "Rustle", "Cloth trim",
              "Clothing, and anything whose class could not be resolved."),
    RDS_HPAIR(AlgorithmConfig, "Rustle", "fLightTrimDb", kFloat, strategies.rustle.lightTrimDb, -40,
              20, 0.5, "Rustle", "Light trim", "Leather and hide."),
    RDS_PARAM(AlgorithmConfig, "Rustle", "fHeavyTrimDb", kFloat, strategies.rustle.heavyTrimDb, -40,
              20, 0.5, "Rustle", "Heavy trim",
              "Mail and plate. Genuinely louder than cloth, and this is where to say so."),
    RDS_PARAM(AlgorithmConfig, "Rustle", "fPlayerTrimDb", kFloat, strategies.rustle.playerTrimDb,
              -40, 20, 0.5, "Rustle", "Player trim",
              "Your own ragdoll's garment hangs on your own bones, at arm's length from the "
              "ears, so it wants to sit lower than anyone else's."),
    RDS_PARAM(AlgorithmConfig, "Rustle", "fSlideDuckDb", kFloat, strategies.rustle.slideDuckDb, -60,
              0, 0.5, "Rustle", "Slide duck",
              "How far a running body grind pulls the rustle down, scaled by the grind's own "
              "weight so it arrives with the slide rather than switching on. A slide already has "
              "four layers describing the same motion; fabric under all of it is mud."),

    // -- Mix ------------------------------------------------------------------
    RDS_PARAM(AlgorithmConfig, "Mix", "fMasterGainDb", kFloat, mix.masterGainDb, -40, 20, 0.5,
              "Mix", "Master gain",
              "The whole mod's level against the rest of the game. With vanilla's body impacts "
              "suppressed there is nothing to sit next to, so this calibrates against footsteps "
              "and combat - and there is no bus control in this engine, so it is the only lever."),
    RDS_HPAIR(AlgorithmConfig, "Mix", "fTransientTrimDb", kFloat, mix.transientTrimDb, -40, 20, 0.5,
              "Mix", "Transient trim",
              "Global trim on the bright contact layer. Up for clicky and present, down for "
              "distant and soft."),
    RDS_PARAM(AlgorithmConfig, "Mix", "fBodyTrimDb", kFloat, mix.bodyTrimDb, -40, 20, 0.5, "Mix",
              "Body trim", "Global trim on the flesh-and-mass layer."),
    RDS_PAIRS(AlgorithmConfig, "Mix", "fSubTrimDb", kFloat, mix.subTrimDb, -40, 20, 0.5, "Mix",
              "Sub trim",
              "Global trim on the pitched boom. The one control most worth touching if the mod "
              "feels either weightless or overbearing."),
    RDS_PARAM(AlgorithmConfig, "Mix", "fGrainTrimDb", kFloat, mix.grainTrimDb, -40, 20, 0.5, "Mix",
              "Grain trim",
              "Global trim on the small filler taps that give a burst its texture."),
    RDS_PAIRS(AlgorithmConfig, "Mix", "fLoopTrimDb", kFloat, mix.loopTrimDb, -40, 20, 0.5, "Mix",
              "Loop trim", "Global trim on the scrape and cloth loops."),
    RDS_PARAM(AlgorithmConfig, "Mix", "fVoiceFloorDb", kFloat, mix.voiceFloorDb, -100, 0, 1, "Mix",
              "Voice floor",
              "Below this a sound is not worth opening a voice for and is dropped before the "
              "renderer ever sees it. Nothing is capped against the count any more, so this is "
              "the one thing standing between the mix and a pile of inaudible voices - raise it "
              "and the mod spends its work on what can actually be heard."),

    // -- Balance ---------------------------------------------------------------
    //
    // The four Mix trims above again, split three ways by which part of the body
    // made the cue. Every one is 0, so this whole section is inert until it is
    // tuned - what a forearm should sound like against a spine is taste, and no
    // recording answers it.
    RDS_PARAM(AlgorithmConfig, "Balance", "bEnabled", kBool, balance.enabled, 0, 1, 1,
              "Balance", "Enabled",
              "Shape the impact composite differently for the head, the spine and the limbs. "
              "Every trim below is 0 by default, so this is for switching a balance you have "
              "already tuned off in one gesture and hearing what it was doing."),

    RDS_HPAIR(AlgorithmConfig, "Balance", "fHeadTransientTrimDb", kFloat,
              balance.head.transientTrimDb, -18, 6, 0.5, "Balance", "Head: transient",
              "After arbitration. Trim on the bright contact layer of a head impact. The skull "
              "accent is a separate decision and lives in HeadImpact."),
    RDS_PARAM(AlgorithmConfig, "Balance", "fHeadBodyTrimDb", kFloat, balance.head.bodyTrimDb,
              -18, 6, 0.5, "Balance", "Head: body",
              "After arbitration. A head plays the torso's mass layer, so this is the only way "
              "to give a faceplant less of it than a back-slam gets."),
    RDS_PAIRS(AlgorithmConfig, "Balance", "fHeadSubTrimDb", kFloat, balance.head.subTrimDb, -18, 6,
              0.5, "Balance", "Head: sub",
              "After arbitration. How much of the pitched boom a skull is worth."),
    RDS_PARAM(AlgorithmConfig, "Balance", "fHeadSurfaceTrimDb", kFloat, balance.head.surfaceTrimDb,
              -18, 6, 0.5, "Balance", "Head: surface",
              "After arbitration. How much of what it hit comes through on a head contact."),

    RDS_HPAIR(AlgorithmConfig, "Balance", "fSpineTransientTrimDb", kFloat,
              balance.spine.transientTrimDb, -18, 6, 0.5, "Balance", "Spine: transient",
              "After arbitration. The neck and the torso - the column, and the part with a whole "
              "body behind it."),
    RDS_PARAM(AlgorithmConfig, "Balance", "fSpineBodyTrimDb", kFloat, balance.spine.bodyTrimDb,
              -18, 6, 0.5, "Balance", "Spine: body",
              "After arbitration. Trim on the flesh-and-mass layer of a torso landing."),
    RDS_PAIRS(AlgorithmConfig, "Balance", "fSpineSubTrimDb", kFloat, balance.spine.subTrimDb, -18,
              6, 0.5, "Balance", "Spine: sub",
              "After arbitration. The boom under a body going down, which is the one the design "
              "says should be there."),
    RDS_PARAM(AlgorithmConfig, "Balance", "fSpineSurfaceTrimDb", kFloat,
              balance.spine.surfaceTrimDb, -18, 6, 0.5, "Balance", "Spine: surface",
              "After arbitration. How much floor comes through under a torso."),

    RDS_HPAIR(AlgorithmConfig, "Balance", "fLimbTransientTrimDb", kFloat,
              balance.limb.transientTrimDb, -18, 6, 0.5, "Balance", "Limb: transient",
              "After arbitration. Arms, legs, and anything off a skeleton we could not name."),
    RDS_PARAM(AlgorithmConfig, "Balance", "fLimbBodyTrimDb", kFloat, balance.limb.bodyTrimDb, -18,
              6, 0.5, "Balance", "Limb: body",
              "After arbitration. Sits the mass layer back out on a stick. SlotGain:fImpBodyLimb "
              "is the other half of this and answers a different question - that one is 'this wav "
              "is hot', this one is 'a forearm has less body in it'. They sum."),
    RDS_PAIRS(AlgorithmConfig, "Balance", "fLimbSubTrimDb", kFloat, balance.limb.subTrimDb, -18, 6,
              0.5, "Balance", "Limb: sub",
              "After arbitration. The one most likely to want moving: a hand slapping the floor "
              "has no body behind it, so the boom under it is borrowed weight."),
    RDS_PARAM(AlgorithmConfig, "Balance", "fLimbSurfaceTrimDb", kFloat, balance.limb.surfaceTrimDb,
              -18, 6, 0.5, "Balance", "Limb: surface",
              "After arbitration. Out on a hand the floor is most of what there is to hear, so "
              "this is the one of the twelve most likely to want to go up."),

    // -- Compress --------------------------------------------------------------
    RDS_PARAM(AlgorithmConfig, "Compress", "bEnabled", kBool, compress.enabled, 0, 1, 1,
              "Compress", "Enabled",
              "Hold the top of one class of cue down without touching the rest of it. Every "
              "threshold below is 0 by default, which is the top of the range and therefore no "
              "holding at all, so switching this on changes nothing until you lower one.\n\n"
              "For squeezing the whole range instead of its top, the controls already exist and "
              "are global: Intensity:fDynamicRangeDb and the two soft-clip knees."),
    RDS_PARAM(AlgorithmConfig, "Compress", "fRatio", kFloat, compress.ratio, 1, 20, 0.5,
              "Compress", "Ratio",
              "Decibels in per decibel out, above a threshold. 1 is off. 4 squeezes the loud ones "
              "together while leaving them distinguishable. 20 is near enough a hard cap to be "
              "one - and a hard cap is worth avoiding, because everything over the threshold "
              "arrives at exactly the threshold and a dozen different impacts become a dozen "
              "identical ones. One ratio for every class: the thresholds say which classes get "
              "held, this says how firmly."),
    RDS_HRULE(AlgorithmConfig, "Compress", "fTransientDb", kFloat, compress.transientDb, -60, 0,
              0.5, "Compress", "Impact: transient",
              "Where the impact's click starts being held, in dB under the loudest thing this mod "
              "can produce. 0 is that top, so it holds nothing.\n\n"
              "The four impact lines are a multiband: each layer is measured against its own "
              "level, so the body can be held while the transient is left alone. This is the one "
              "least worth pulling down - it sits about 8 dB under the body already, and holding "
              "it is how an impact stops reading as a strike."),
    RDS_PARAM(AlgorithmConfig, "Compress", "fBodyDb", kFloat, compress.bodyDb, -60, 0, 0.5,
              "Compress", "Impact: body",
              "Where the impact's mass starts being held, in dB under the mod's loudest.\n\n"
              "The line to reach for first. Measured over the corpus, imp_body is the layer that "
              "owns the composite's peak at every intensity - about 8 dB clear of the transient - "
              "so this is where the headroom is or it is nowhere. Pull this down and "
              "Mix:fMasterGainDb finally has somewhere to spend."),
    RDS_PARAM(AlgorithmConfig, "Compress", "fBassDb", kFloat, compress.bassDb, -60, 0, 0.5,
              "Compress", "Impact: bass",
              "Where the pitched sub at +65 ms starts being held, in dB under the mod's loudest. "
              "The longest layer and the lowest, so it holds the peak longest even where it does "
              "not set it, and the one whose excursion a soft clip mangles most audibly. Hold it "
              "here rather than trimming it with Mix:fSubTrimDb: a trim takes the weight off "
              "every hit, this takes it off only the ones that were too big."),
    RDS_PARAM(AlgorithmConfig, "Compress", "fBodyLimbDb", kFloat, compress.bodyLimbDb, -60, 0, 0.5,
              "Compress", "Impact: body (limb)",
              "Where the same mass out on an arm or a leg starts being held, in dB under the "
              "mod's loudest. Its own line rather than the body's because it is drier and about "
              "6 dB quieter, so a threshold that holds the torso correctly never reaches it.\n\n"
              "Read off the layer the engine asked for, so it works before anybody has recorded "
              "an imp_body_limb - exactly as SlotGain:fImpBodyLimb trims the imp_body file the "
              "limb composite falls back to."),
    RDS_PARAM(AlgorithmConfig, "Compress", "fImpactDb", kFloat, compress.impactDb, -60, 0, 0.5,
              "Compress", "Impact: skins",
              "Where a composite's surface skin and armour skin start being held, in dB under the "
              "mod's loudest. The catch-all for every layer of an impact that has no line of its "
              "own.\n\n"
              "Both skins ride the body layer about 12 dB under it, so neither is ever the layer "
              "that clips and this is rarely the slider you want - the four above are. It kept "
              "the old key's name because a config written before the split still reads back "
              "meaning the same thing for the layers it still governs."),
    RDS_PARAM(AlgorithmConfig, "Compress", "fTapDb", kFloat, compress.tapDb, -60, 0, 0.5,
              "Compress", "Tap",
              "Where the small filler taps start being held, in dB under the mod's loudest. The "
              "line most worth pulling down: nine of every ten contacts are taps, so a tap that "
              "can reach the top of the range is a tumble with no shape to it. Lower it and the "
              "hero hits stand out without anything else getting quieter."),
    RDS_PARAM(AlgorithmConfig, "Compress", "fHeadDb", kFloat, compress.headDb, -60, 0, 0.5,
              "Compress", "Head",
              "Where the head accent starts being held, in dB under the mod's loudest. Holds the "
              "accent once the lead boost is in without taking it off the sort - unlike "
              "HeadImpact:fLeadMaxLevelDb, which does both, and unlike that one this squeezes "
              "rather than clamps."),
    RDS_PARAM(AlgorithmConfig, "Compress", "fCrunchDb", kFloat, compress.crunchDb, -60, 0, 0.5,
              "Compress", "Crunch",
              "Where the bone crunch starts being held, in dB under the mod's loudest."),
    RDS_PARAM(AlgorithmConfig, "Compress", "fGoreDb", kFloat, compress.goreDb, -60, 0, 0.5,
              "Compress", "Gore",
              "Where the wet gore layer starts being held, in dB under the mod's loudest."),
    RDS_PARAM(AlgorithmConfig, "Compress", "fScrapeDb", kFloat, compress.scrapeDb, -60, 0, 0.5,
              "Compress", "Scrape",
              "Where the sliding scrape loop starts being held, in dB under the mod's loudest. A "
              "loop is re-levelled while it runs, so this is the one class where the holding "
              "happens continuously rather than at one instant."),
    RDS_PARAM(AlgorithmConfig, "Compress", "fAirborneDb", kFloat, compress.airborneDb, -60, 0, 0.5,
              "Compress", "Airborne rise",
              "Where the airborne whoosh starts being held, in dB under the mod's loudest."),

    // -- Player ---------------------------------------------------------------
    RDS_PARAM(AlgorithmConfig, "Player", "bEnabled", kBool, player.enabled, 0, 1, 1, "Player",
              "Enabled", "Whether your own ragdoll makes any sound at all."),
    RDS_HRULE(AlgorithmConfig, "Player", "bAttachToBones", kBool, player.attachToBones, 0, 1, 1,
              "Player", "Attach to bones",
              "At arm's length collapsing every layer to one point stops helping and starts "
              "sounding like the audio is inside your head, so your own limbs carry their own "
              "sounds."),
    RDS_PARAM(AlgorithmConfig, "Player", "fSubTrimDb", kFloat, player.subTrimDb, -40, 12, 0.5,
              "Player", "Sub trim",
              "A 30 Hz boom at zero distance through headphones is overwhelming, and in VR low "
              "frequency is felt as much as heard. This pulls it back for your own falls only."),
    RDS_PARAM(AlgorithmConfig, "Player", "bSkipAirborneWhoosh", kBool, player.skipAirborneWhoosh, 0,
              1, 1, "Player", "Skip whoosh",
              "You are the one moving and the view already tells you, so the anticipation whoosh "
              "is off for your own falls."),
    RDS_HRULE(AlgorithmConfig, "Player", "fMasterGainDb", kFloat, player.masterGainDb, -40, 20, 0.5,
              "Player", "Master gain", "Your own ragdoll's level, separate from everyone else's."),

    // -- Distance -------------------------------------------------------------
    RDS_PAIRS(AlgorithmConfig, "Distance", "fFullRadius", kFloat, distance.fullRadius, 0, 5000, 50,
              "Distance", "Full detail radius",
              "Inside this everything plays: composites, grains, loops and bed. About ten metres."),
    RDS_PARAM(AlgorithmConfig, "Distance", "fSimplifiedRadius", kFloat, distance.simplifiedRadius,
              0, 20000, 100, "Distance", "Simplified radius",
              "Between the two radii only the hero composites play - nobody resolves the detail at "
              "that range. Beyond it the actor is dropped entirely, which is what keeps a "
              "battlefield of ragdolls from ever becoming a performance question."),

    // -- Slot resolution ------------------------------------------------------
    RDS_PARAM(AlgorithmConfig, "Slots", "bShuffleBag", kBool, slots.shuffleBag, 0, 1, 1, "Slots",
              "Shuffle bag",
              "Pick variants out of a shuffled bag rather than at random. Random repeats itself "
              "immediately, and an immediate repeat is exactly what people notice."),
    RDS_PARAM(AlgorithmConfig, "Slots", "bStableVariants", kBool, slots.stableVariants, 0, 1, 1,
              "Slots", "Stable variants",
              "Give every cue its own dice, rolled from the contact it came from, instead of "
              "dealing from one running deck. Off, adding or removing a single cue early in a "
              "take re-rolls the sample and the pitch of everything after it and two exports "
              "differ everywhere rather than where the change bit. The seed below still re-rolls "
              "the whole take, so the variety is unchanged."),
    RDS_HPAIR(AlgorithmConfig, "Slots", "bConditionalVariants", kBool, slots.conditionalVariants,
              0, 1, 1, "Slots", "Conditional variants",
              "Honour the per-file conditions in RagdollSounds_SFX.ini. A file tagged for a "
              "surface, an armour class or both is only a candidate where it matches, and beats "
              "the untagged files where it does. Off, every file on a slot is a candidate "
              "everywhere - which is what the mod did before conditions existed."),
    RDS_PARAM(AlgorithmConfig, "Slots", "bSurfaceConditions", kBool, slots.surfaceConditions, 0, 1,
              1, "Slots", "Honour the surface half",
              "Turn off to ignore the surface half of every condition while the armour half keeps "
              "working. This and the next answer the question this feature gets asked most: is "
              "the stone-specific recording actually earning its file, or would the generic one "
              "have done?"),
    RDS_PARAM(AlgorithmConfig, "Slots", "bArmorConditions", kBool, slots.armorConditions, 0, 1, 1,
              "Slots", "Honour the armour half",
              "The same for the armour half. With both off the ladder collapses entirely and the "
              "master switch above is the shorter way to say it."),
    RDS_PARAM(AlgorithmConfig, "Slots", "iRngSeed", kInt, slots.rngSeed, 0, 2147483647, 1, "Slots",
              "Random seed",
              "0 seeds from the clock, which is what the game wants. Any other value makes every "
              "run identical, which is what an A/B between two configs needs so it compares the "
              "configs and not two dice rolls."),

    // -- Layer mutes ----------------------------------------------------------
    //
    // Applied at render, so muting one leaves every arbitration decision
    // identical and only silences what comes out. That is what makes an A/B
    // honest: the strategy `bEnabled` flags gate at proposal time instead, and
    // turning one of those off lets other cues move into the freed budget.
    RDS_PARAM(AlgorithmConfig, "Layers", "bImpTransient", kBool, layers.impTransient, 0, 1, 1, "Layers",
              "imp_transient",
              "The contact itself - bright, fast, and the quietest layer of the stack. Mute it and an impact loses its edge but keeps its weight."),
    RDS_PARAM(AlgorithmConfig, "Layers", "bImpBody", kBool, layers.impBody, 0, 1, 1, "Layers",
              "imp_body",
              "Low-mid flesh and mass, arriving ~20 ms in. The main body of the sound at ordinary intensities, where the sub has not taken over yet."),
    RDS_PARAM(AlgorithmConfig, "Layers", "bImpSub", kBool, layers.impSub, 0, 1, 1, "Layers",
              "imp_sub",
              "The late pitched boom, ~65 ms in. The design's central claim is that this one layer is what makes a hit read as mass rather than contact - mute it and the gnarl should leave with it. If it does not, the balance is wrong, not the timing."),
    RDS_PARAM(AlgorithmConfig, "Layers", "bLimbTap", kBool, layers.limbTap, 0, 1, 1, "Layers",
              "limb_tap",
              "The burst filler: quiet, dry, heavily pitch-scattered taps that give a burst its three to five grains. Mute it to hear the bursts reduced to their hero hits alone."),
    RDS_PARAM(AlgorithmConfig, "Layers", "bCrunchGran", kBool, layers.crunchGran, 0, 1, 1, "Layers",
              "crunch_gran",
              "The granular low-mid crackle that is what a bone break actually is - ten times the transient density of a plain thud in that band, not a snap sample. Covers spine_crunch and limb_crunch too: they are the same layer on a different bone. To silence one part and not the others use Damage:b<Part>Enabled, which stops the cue being proposed rather than muting it after the arbitrator made room for it."),
    RDS_PARAM(AlgorithmConfig, "Layers", "bGoreWet", kBool, layers.goreWet, 0, 1, 1, "Layers",
              "gore_wet",
              "The wet squelch: the top damage tier, and the one layer all three parts share. The fastest way to check the gore thresholds are not opening on ordinary knockdowns."),
    RDS_PARAM(AlgorithmConfig, "Layers", "bScrapeGrain", kBool, layers.scrapeGrain, 0, 1, 1,
              "Layers", "scrape_grain",
              "The scuff a grind opens with. Mute it and the grinds still start, just out of "
              "nothing - which is the A/B for whether a slide needs an arrival marked at all."),
    RDS_PARAM(AlgorithmConfig, "Layers", "bScrapeLoop", kBool, layers.scrapeLoop, 0, 1, 1, "Layers",
              "scrape_loop",
              "The full-weight sliding rumble, surface variants included. The A/B for the body "
              "half of a slide: mute it and the state, the budget, the slide-end impact and the "
              "limb grinds all still happen."),
    RDS_PARAM(AlgorithmConfig, "Layers", "bScrapeLimb", kBool, layers.scrapeLimb, 0, 1, 1, "Layers",
              "scrape_limb",
              "The light grind of one limb dragging, surface variants included. Mute it and a "
              "dragging foot goes silent while a full slide is untouched, which is the cleanest "
              "way to hear what the two-loop split bought."),
    RDS_PARAM(AlgorithmConfig, "Layers", "bScrapeLoopRumble", kBool, layers.scrapeLoopRumble, 0, 1,
              1, "Layers", "scrape_loop_rumble",
              "The bed of mass under both grinds. Mute it and the grinds run exactly as they did "
              "before the layer existed, which is the A/B the whole thing was built for: our "
              "grinds measure 35-45 dB brighter than a real slide, and this is the half that was "
              "missing rather than the half that was wrong."),
    RDS_PARAM(AlgorithmConfig, "Layers", "bAirWhoosh", kBool, layers.airWhoosh, 0, 1, 1, "Layers",
              "air_whoosh",
              "The airborne anticipation rise. Mute it to judge whether the fall still reads as a fall without being told one is coming."),
    RDS_PARAM(AlgorithmConfig, "Layers", "bHeadImpact", kBool, layers.headImpact, 0, 1, 1, "Layers",
              "head_impact",
              "The dull skull thud with its granular edge. Mute it to check the head gate is firing on head contacts and not on everything that happens to be moving fast."),
    RDS_PARAM(AlgorithmConfig, "Layers", "bSettleRest", kBool, layers.settleRest, 0, 1, 1, "Layers",
              "settle_rest",
              "The soft final flop that closes the event. Mute it and falls should audibly trail off unfinished, which is the whole reason it exists."),
};

#undef RDS_PARAM
#undef RDS_PAIRS
#undef RDS_HRULE
#undef RDS_HPAIR
#undef RDS_PARAM_STR

[[nodiscard]] const void* MemberPtr(const void* root, const ParamDesc& p) {
    return static_cast<const std::byte*>(root) + p.offset;
}

[[nodiscard]] void* MemberPtr(void* root, const ParamDesc& p) {
    return static_cast<std::byte*>(root) + p.offset;
}

[[nodiscard]] bool EqualsIgnoreCase(std::string_view a, std::string_view lowerB) {
    if (a.size() != lowerB.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        const char c = (a[i] >= 'A' && a[i] <= 'Z') ? static_cast<char>(a[i] + 32) : a[i];
        if (c != lowerB[i]) {
            return false;
        }
    }
    return true;
}

// ── the surfaces list ────────────────────────────────────────────────────────
//
// Thirteen classes x eight fields, generated rather than typed out. Every other
// row in this file is a literal because every other row is a decision; these
// hundred and four are the same eight decisions repeated, and a hand-written
// table of them would be a hundred and four chances to paste `kIce` into
// `kGlass`'s offset.
//
// They are ordinary ParamDesc rows in every other respect, which is the point:
// the ini reader, the writer, the clamp, the delta log, the slider panel, the
// remote patcher and the export all walk `AlgorithmParams()` and none of them
// needs to know these were not typed. What makes them *a list* rather than a
// section is only that `SurfaceFileParams` filters them by `opened`.

/// Backing store for the section and group names, which ParamDesc holds as
/// views. A function-local static built once and never touched again, so the
/// views outlive every caller.
struct SurfaceRowStrings {
    std::string section;  ///< "Surface.ice"
    std::string group;    ///< "Surface: ice"
    std::string tooltips[8];
};

[[nodiscard]] const std::vector<ParamDesc>& SurfaceRows() {
    static const std::vector<ParamDesc> rows = [] {
        static std::vector<SurfaceRowStrings> strings(SurfaceConfig::kClasses);
        std::vector<ParamDesc> out;
        out.reserve(SurfaceConfig::kClasses * 8);

        const SurfaceSkinConfig defaults{};
        const std::size_t base = offsetof(AlgorithmConfig, surfaces.skins);

        for (std::size_t i = 0; i < SurfaceConfig::kClasses; ++i) {
            const auto surface = static_cast<SurfaceClass>(i);
            const std::string name{ToString(surface)};
            const SurfaceClass parent = SurfaceParent(surface);
            const std::string from =
                parent == SurfaceClass::kCount
                    ? std::string{"the [Surfaces] section"}
                    : std::format("[Surface.{}]", ToString(parent));

            SurfaceRowStrings& s = strings[i];
            s.section = std::format("Surface.{}", name);
            s.group = std::format("Surface: {}", name);

            // Every tooltip says what the field does *and* where the value comes
            // from when the block is closed, because "why did this move when I
            // touched stone" is the first question the inheritance raises.
            const std::string tail = std::format(" Inherited from {} until this surface has a "
                                                 "block of its own.", from);
            s.tooltips[0] = std::format(
                "Play the {} colour at all. Muted at render, so every arbitration decision stays "
                "identical and only the sound goes.{}", name, tail);
            s.tooltips[1] = std::format(
                "The {} colour alone, summed on top of Surfaces:fTrimDb. Separately recorded "
                "sounds arrive at separate levels, and pulling the role trim to fix one takes "
                "every other floor with it.{}", name, tail);
            s.tooltips[2] = std::format(
                "When the {} colour arrives, relative to the contact frame. Close to the "
                "transient or it stops reading as the same event - but a hollow knock has a "
                "resonant delay that a brittle crack does not.{}", name, tail);
            s.tooltips[3] = std::format(
                "How audible {} is on a light contact.{}", name, tail);
            s.tooltips[4] = std::format(
                "And on a heavy one. The gap between the two is how much this material *changes* "
                "with force: wide for glass, nearly flat for carpet.{}", name, tail);
            s.tooltips[5] = std::format(
                "Give the burst filler a {} colour as well. Nine of every ten contacts are taps, "
                "so this is where a floor gets identified rather than confirmed - but a splash on "
                "every one of them is absurd, which is why this is per-surface.{}", name, tail);
            s.tooltips[6] = std::format(
                "How audible {} is on a light scuff. The tap's offset and its headroom clamp stay "
                "in [Surfaces] - they describe the grain being coloured.{}", name, tail);
            s.tooltips[7] = std::format(
                "And on a hard one.{}", tail);

            const std::size_t skin = base + i * sizeof(SurfaceSkinConfig);
            const auto row = [&](std::string_view key, ParamType type, std::size_t member,
                                 double def, double lo, double hi, double step,
                                 std::string_view label, std::size_t tip, bool pair,
                                 bool rule) {
                ParamDesc p{};
                p.section = s.section;
                p.key = key;
                p.type = type;
                p.offset = skin + member;
                p.defaultValue = def;
                p.minValue = lo;
                p.maxValue = hi;
                p.step = step;
                p.group = s.group;
                p.label = label;
                p.tooltip = s.tooltips[tip];
                p.pairWithNext = pair;
                p.ruleBefore = rule;
                out.push_back(p);
            };

            row("bEnabled", ParamType::kBool, offsetof(SurfaceSkinConfig, enabled),
                defaults.enabled ? 1.0 : 0.0, 0, 1, 1, "Play this surface", 0, true, false);
            row("fTrimDb", ParamType::kFloat, offsetof(SurfaceSkinConfig, trimDb),
                defaults.trimDb, -24, 12, 0.5, "Trim", 1, false, false);
            row("fOffsetMs", ParamType::kFloat, offsetof(SurfaceSkinConfig, offsetMs),
                defaults.offsetMs, -50, 300, 1, "Offset on impacts", 2, false, true);
            row("fGainAtMinDb", ParamType::kFloat, offsetof(SurfaceSkinConfig, gainAtMinDb),
                defaults.gainAtMinDb, -60, 12, 0.5, "Impact colour at quiet", 3, true, false);
            row("fGainAtMaxDb", ParamType::kFloat, offsetof(SurfaceSkinConfig, gainAtMaxDb),
                defaults.gainAtMaxDb, -60, 12, 0.5, "Impact colour at loud", 4, false, false);
            row("bOnTaps", ParamType::kBool, offsetof(SurfaceSkinConfig, onTaps),
                defaults.onTaps ? 1.0 : 0.0, 0, 1, 1, "Colour taps too", 5, false, true);
            row("fTapGainAtMinDb", ParamType::kFloat, offsetof(SurfaceSkinConfig, tapGainAtMinDb),
                defaults.tapGainAtMinDb, -60, 12, 0.5, "Tap colour at quiet", 6, true, false);
            row("fTapGainAtMaxDb", ParamType::kFloat, offsetof(SurfaceSkinConfig, tapGainAtMaxDb),
                defaults.tapGainAtMaxDb, -60, 12, 0.5, "Tap colour at loud", 7, false, false);
        }
        return out;
    }();
    return rows;
}

/// The whole table: the literal rows, then the generated ones.
///
/// Contiguous and in that order on purpose - it is what lets the two file spans
/// below be sub-spans of this rather than copies, so a pointer into
/// `AlgorithmParams()` and a pointer into `SurfaceParams()` compare equal for
/// the same row.
[[nodiscard]] const std::vector<ParamDesc>& AllAlgorithmRows() {
    static const std::vector<ParamDesc> all = [] {
        std::vector<ParamDesc> out{std::begin(kAlgorithmParams), std::end(kAlgorithmParams)};
        const auto& surfaces = SurfaceRows();
        out.insert(out.end(), surfaces.begin(), surfaces.end());
        return out;
    }();
    return all;
}

}  // namespace

std::span<const ParamDesc> AlgorithmParams() {
    const auto& all = AllAlgorithmRows();
    return std::span<const ParamDesc>{all};
}

std::span<const ParamDesc> AlgorithmFileParams() {
    const auto& all = AllAlgorithmRows();
    return std::span<const ParamDesc>{all}.first(std::size(kAlgorithmParams));
}

std::span<const ParamDesc> SurfaceParams() {
    const auto& all = AllAlgorithmRows();
    return std::span<const ParamDesc>{all}.subspan(std::size(kAlgorithmParams));
}

std::size_t SurfaceRowsPerClass() { return 8; }

SurfaceClass SurfaceClassOfParam(const ParamDesc& p) {
    constexpr std::string_view kPrefix = "Surface.";
    if (p.section.size() <= kPrefix.size() || p.section.substr(0, kPrefix.size()) != kPrefix) {
        return SurfaceClass::kCount;
    }
    return SurfaceClassFrom(p.section.substr(kPrefix.size()));
}

std::vector<ParamDesc> OpenedSurfaceParams(const AlgorithmConfig& config) {
    // What actually gets written. A closed class contributes nothing, which is
    // the whole of "an unopened surface costs no ini lines" - and because the
    // reader discovers `opened` from the section headers that are present, not
    // writing a block is also how it stays closed on the next load.
    const auto rows = SurfaceParams();
    const std::size_t perClass = SurfaceRowsPerClass();
    std::vector<ParamDesc> out;
    for (std::size_t i = 0; i < SurfaceConfig::kClasses; ++i) {
        if (!config.surfaces.opened[i]) {
            continue;
        }
        const std::size_t first = i * perClass;
        for (std::size_t j = 0; j < perClass && first + j < rows.size(); ++j) {
            out.push_back(rows[first + j]);
        }
    }
    return out;
}

std::span<const ParamDesc> GeneralParams() { return std::span<const ParamDesc>{kGeneralParams}; }

double GetParam(const void* root, const ParamDesc& p) {
    const void* member = MemberPtr(root, p);
    switch (p.type) {
        case ParamType::kBool: {
            bool value{};
            std::memcpy(&value, member, sizeof(value));
            return value ? 1.0 : 0.0;
        }
        case ParamType::kInt:
        case ParamType::kEnum: {
            // Every integral field in Config.h is four bytes wide, enums
            // included, so one path covers both. rngSeed is unsigned; its schema
            // maximum keeps it inside the signed range rather than costing a
            // second storage kind for one field.
            std::int32_t value{};
            std::memcpy(&value, member, sizeof(value));
            return static_cast<double>(value);
        }
        case ParamType::kFloat: {
            float value{};
            std::memcpy(&value, member, sizeof(value));
            return static_cast<double>(value);
        }
        case ParamType::kString:
            // Not a number. Answering 0 rather than asserting keeps every walk
            // over the table - the delta log, the export, the ini writer - one
            // loop with no guard in front of it.
            break;
    }
    return 0.0;
}

void SetParam(void* root, const ParamDesc& p, double value) {
    void* member = MemberPtr(root, p);
    const double coerced = CoerceParam(p, value);
    switch (p.type) {
        case ParamType::kBool: {
            const bool stored = coerced != 0.0;
            std::memcpy(member, &stored, sizeof(stored));
            return;
        }
        case ParamType::kInt:
        case ParamType::kEnum: {
            const auto stored = static_cast<std::int32_t>(coerced);
            std::memcpy(member, &stored, sizeof(stored));
            return;
        }
        case ParamType::kFloat: {
            const auto stored = static_cast<float>(coerced);
            std::memcpy(member, &stored, sizeof(stored));
            return;
        }
        case ParamType::kString:
            return;  // SetParamString is the door for these
    }
}

std::string_view GetParamString(const void* root, const ParamDesc& p) {
    if (p.type != ParamType::kString || p.capacity == 0) {
        return {};
    }
    const auto* text = static_cast<const char*>(MemberPtr(root, p));
    return std::string_view{text, std::strlen(text)};
}

void SetParamString(void* root, const ParamDesc& p, std::string_view text) {
    if (p.type != ParamType::kString || p.capacity == 0) {
        return;
    }
    auto* buffer = static_cast<char*>(MemberPtr(root, p));
    const std::size_t take = std::min(text.size(), p.capacity - 1);
    std::memcpy(buffer, text.data(), take);
    std::memset(buffer + take, 0, p.capacity - take);
}

double CoerceParam(const ParamDesc& p, double value) {
    if (p.type == ParamType::kString) {
        return 0.0;
    }
    if (!std::isfinite(value)) {
        return p.defaultValue;
    }
    double clamped = std::clamp(value, p.minValue, p.maxValue);
    if (p.type != ParamType::kFloat) {
        clamped = std::round(clamped);
    }
    if (p.type == ParamType::kBool) {
        clamped = clamped != 0.0 ? 1.0 : 0.0;
    }
    return clamped;
}

std::string FormatParam(const ParamDesc& p, double value) {
    switch (p.type) {
        case ParamType::kBool:
            return value != 0.0 ? "1" : "0";
        case ParamType::kInt:
        case ParamType::kEnum:
            return std::format("{}", static_cast<std::int64_t>(std::llround(value)));
        case ParamType::kString:
            return {};
        case ParamType::kFloat:
            break;
    }
    // A short decimal rather than every digit of the float: the ini is meant to
    // be read and edited by hand, and 46 is a friendlier number to meet than
    // 46.00000762939453.
    std::string text = std::format("{:.4f}", value);
    while (text.size() > 1 && text.back() == '0') {
        text.pop_back();
    }
    if (!text.empty() && text.back() == '.') {
        text.pop_back();
    }
    return text;
}

double ParseParam(const ParamDesc& p, std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() &&
           (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    if (text.empty() || p.type == ParamType::kString) {
        return p.defaultValue;
    }

    // Enums accept their name as well as their number, because "info" is what
    // somebody will actually type into a log level.
    if (p.type == ParamType::kEnum) {
        for (std::size_t i = 0; i < p.enumNames.size(); ++i) {
            if (EqualsIgnoreCase(text, p.enumNames[i])) {
                return static_cast<double>(i);
            }
        }
    }
    if (p.type == ParamType::kBool) {
        if (EqualsIgnoreCase(text, "true") || EqualsIgnoreCase(text, "yes")) return 1.0;
        if (EqualsIgnoreCase(text, "false") || EqualsIgnoreCase(text, "no")) return 0.0;
    }

    double parsed{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (result.ec != std::errc{}) {
        // A hand-mangled file degrades to defaults rather than to zeroes, which
        // is the difference between "somebody typo'd a number" and "the mod went
        // silent and nobody can say why".
        return p.defaultValue;
    }
    return parsed;
}

std::string QualifiedKey(const ParamDesc& p) { return std::format("{}:{}", p.section, p.key); }

std::vector<std::string> Deltas(const void* root, std::span<const ParamDesc> params) {
    std::vector<std::string> out;
    for (const auto& p : params) {
        if (p.type == ParamType::kString) {
            // A path has no default worth quoting, so the delta is the value or
            // nothing. It is still worth a line: "the testbench could not start
            // OBS" is answered by seeing which path the mod was holding.
            const std::string_view text = GetParamString(root, p);
            if (!text.empty()) {
                out.push_back(std::format("{}={}", QualifiedKey(p), text));
            }
            continue;
        }
        const double current = GetParam(root, p);
        const double base = CoerceParam(p, p.defaultValue);
        if (p.type == ParamType::kFloat) {
            // 0.35 does not survive the trip through a float exactly, so compare
            // at the precision the file carries rather than bit for bit -
            // otherwise every float in the table reports itself as changed.
            if (std::fabs(current - base) <= 1e-4 * std::max(1.0, std::fabs(base))) {
                continue;
            }
        } else if (current == base) {
            continue;
        }
        out.push_back(std::format("{}={} (default {})", QualifiedKey(p), FormatParam(p, current),
                                  FormatParam(p, base)));
    }
    return out;
}

}  // namespace rds
