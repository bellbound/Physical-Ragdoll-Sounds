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
/// A wrapper rather than a twelfth macro argument, because five rows in nine
/// hundred have ever moved: `Renamed(RDS_PARAM(...), "HeadImpact", "fLeadClearMs")`.
[[nodiscard]] constexpr ParamDesc Renamed(ParamDesc p, std::string_view section,
                                          std::string_view key) {
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
              "Silences vanilla's own ragdoll body impacts so ours are the whole mix. Turn it off "
              "and every contact doubles, with half of it playing vanilla's dirt sample whatever "
              "the actual surface was."),
    RDS_PARAM(GeneralConfig, "Audio", "iOutputModelFormID", kInt, audio.outputModelFormId, 0,
              2147483647, 1, "Audio", "Output model",
              "The sound output model every voice is opened with, in decimal. This is what gives a "
              "sound its distance falloff, its reverb send and its VR spatialisation; without one "
              "everything plays flat and follows you around the room. 741764 (0x000B5184) is "
              "Skyrim's dialogue model and is the verified default."),
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
};

const ParamDesc kAlgorithmParams[] = {
    // -- Stage 0: Ingest ------------------------------------------------------
    RDS_PARAM(AlgorithmConfig, "Ingest", "fMinImpactSpeed", kFloat, ingest.minImpactSpeed, 0, 400,
              1, "Ingest", "Minimum impact speed",
              "The quietest contact allowed to make any sound at all. Raise it and the small taps "
              "that fill out a tumble disappear; lower it and the settle at the end of a fall "
              "starts chattering."),
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
    RDS_HRULE(AlgorithmConfig, "Ingest", "fGrazeRatio", kFloat, ingest.grazeRatio, 0.2, 10, 0.1,
              "Ingest", "Graze ratio",
              "How much faster a contact must be sliding along a surface than into it before it is "
              "heard as a scrape rather than a thud. Lower sends more of a tumble to the scrape "
              "loop and takes the punch out of it."),
    RDS_PARAM(AlgorithmConfig, "Ingest", "fGrazeMaxImpactSpeed", kFloat,
              ingest.grazeMaxImpactSpeed, 0, 4000, 10, "Ingest", "Graze speed ceiling",
              "Above this closing speed a contact is always a thud, however much it is also "
              "sliding. Raise it and hard skids go quiet as the scrape path swallows them; lower "
              "it and genuine slides start thudding."),

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
    Renamed(RDS_HRULE(AlgorithmConfig, "Motion", "fSlideMinTangentSpeed", kFloat,
                      motion.slideMinTangentSpeed, 0, 2000, 10, "Motion", "Slide speed",
                      "How fast a body has to be travelling along the ground before the slide "
                      "state opens and the grinding loop is allowed."),
            "Phase", "fSlideMinTangentSpeed"),
    Renamed(RDS_PARAM(AlgorithmConfig, "Motion", "fSlideMinDurationMs", kFloat,
                      motion.slideMinDurationMs, 0, 3000, 10, "Motion", "Slide duration",
                      "And for how long, so a single glancing blow does not read as a slide."),
            "Phase", "fSlideMinDurationMs"),
    Renamed(RDS_PAIRS(AlgorithmConfig, "Motion", "fSlideMinDistance", kFloat,
                      motion.slideMinDistance, 0, 2000, 5, "Motion", "Slide distance",
                      "Or how far the body has to have travelled, if it gets there before the "
                      "duration above. A fast skid crosses two metres in a few frames; without "
                      "this only slow grinding ever counted as a slide."),
            "ScrapeLoop", "fMinDistance"),
    RDS_PARAM(AlgorithmConfig, "Motion", "fSlideGraceMs", kFloat, motion.slideGraceMs, 0, 2000, 10,
              "Motion", "Slide grace",
              "How long the grazing may stop before the slide is over. This does not decide "
              "*how* it ended - the body does that, and the three ways out are coming to rest, "
              "going airborne, and hitting something - only that it has. Short, and a couple of "
              "quiet frames in a long grind break it into pieces; long, and the grind carries on "
              "past the moment the body was stopped."),
    RDS_PARAM(AlgorithmConfig, "Motion", "bSlideEndImpact", kBool, motion.slideEndImpact, 0, 1, 1,
              "Motion", "Slide-end impact",
              "Place an impact where a slide ended by hitting something. The collision that "
              "stops a slide is regularly missing from the contact stream - the limb catches, "
              "reports one glancing row, and the body is simply stopped - so without this the "
              "loudest moment of a slide is a loop fading out over silence."),
    Renamed(RDS_HRULE(AlgorithmConfig, "Motion", "fSettleEnergyFloor", kFloat,
                      motion.settleEnergyFloor, 0, 1000, 5, "Motion", "Settle floor",
                      "The energy the fall has to drop below before it is considered over. Raise "
                      "it and knockdowns close early, cutting off the last real bounce; lower it "
                      "and the closing cue arrives late."),
            "Phase", "fSettleEnergyFloor"),
    Renamed(RDS_PARAM(AlgorithmConfig, "Motion", "fSettleQuietMs", kFloat, motion.settleQuietMs, 0,
                      5000, 10, "Motion", "Settle quiet",
                      "How much quiet is needed before the event closes. This is what stops a fall "
                      "that pauses mid-tumble playing its closing cue too early."),
            "Phase", "fSettleQuietMs"),
    RDS_PARAM(AlgorithmConfig, "Motion", "fRestingExitPeakFrac", kFloat, motion.restingExitPeakFrac,
              0, 2.0, 0.05, "Motion", "Resting exit",
              "What it takes to wake a fall that had gone quiet, as a share of that fall's own "
              "hardest contact. The old machine had no way out of Settle at all, so the six "
              "loudest contacts of a knockdown arriving after it closed were all judged at the "
              "settle budget and every one was dropped. Lower this and the tail of a fall starts "
              "rattling again; raise it and a genuine second landing is silenced."),
    Renamed(RDS_HRULE(AlgorithmConfig, "Motion", "fGetUpBlendMs", kFloat, motion.getUpBlendMs, 0,
                      3000, 10, "Motion", "Get-up silence",
                      "Silence held after a ragdoll formally ends, covering the blend back to "
                      "animation: for this long, nothing wakes the fall again. A guess - every "
                      "capture take was paralysed and none of them ever got up."),
            "Phase", "fGetUpBlendMs"),
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
    Renamed(RDS_PAIRS(AlgorithmConfig, "Motion", "fSlideTrimDb", kFloat, motion.slide.gainTrimDb,
                      -60, 12, 0.5, "Motion budgets", "Slide trim",
                      "How loud the impacts during a slide are. The grinding loop has its own "
                      "gain."),
            "Phase", "fSlideTrimDb"),
    Renamed(RDS_PARAM(AlgorithmConfig, "Motion", "iSlideMaxCues", kInt, motion.slide.maxCuesPerBurst,
                      0, 16, 1, "Motion budgets", "Slide grains",
                      "How many one-shots may punctuate a slide, for the moments a limb catches."),
            "Phase", "iSlideMaxCues"),
    Renamed(RDS_PAIRS(AlgorithmConfig, "Motion", "fRestingTrimDb", kFloat,
                      motion.resting.gainTrimDb, -60, 12, 0.5, "Motion budgets", "Resting trim",
                      "The last twenty contacts of every knockdown are limbs flopping and should "
                      "be nearly silent. This one number is what keeps a fall from ending in a "
                      "rattle."),
            "Phase", "fSettleTrimDb"),
    Renamed(RDS_PARAM(AlgorithmConfig, "Motion", "iRestingMaxCues", kInt,
                      motion.resting.maxCuesPerBurst, 0, 16, 1, "Motion budgets", "Resting grains",
                      "How many sounds the resting tail may spend. One is the design."),
            "Phase", "iSettleMaxCues"),

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
    RDS_PARAM(AlgorithmConfig, "Hero", "fSlideEndFrac", kFloat, hero.slideEndFrac, 0, 3, 0.01,
              "Hero", "Slide end at",
              "How fast the body still had to be going when something stopped its slide for that "
              "to be a hero moment, as a fraction of the loud anchor. At the shipped anchor of "
              "960 the default is 288 u/s - a body still travelling about as fast as an ordinary "
              "shove. Zero switches the clause off and leaves the slide-end impact to be judged "
              "like any other contact."),
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
    RDS_PARAM(AlgorithmConfig, "Hero", "fBurstGapFrac", kFloat, hero.burstGapFrac, 0, 1, 0.05,
              "Hero", "Waive the gap",
              "How much of the enforced silence between bursts a hero moment is excused, so the "
              "landing is not dropped for arriving too soon after the burst it just closed. 1.0 "
              "waives it entirely, 0 waives nothing."),
    RDS_PARAM(AlgorithmConfig, "Hero", "bIgnoreRateCap", kBool, hero.ignoreRateCap, 0, 1, 1, "Hero",
              "Waive the rate cap",
              "Whether a hero moment's contacts also ignore the 46 ms minimum between onsets."),
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
              "here is audible on every impact in the mod."),
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
    RDS_PARAM(AlgorithmConfig, "SlotGain", "fGoreWet", kFloat, slotGains.goreWet, -24, 12, 0.5,
              "Slot gains", "gore_wet",
              "The squelch, obliterate tier only. It plays rarely enough that it is easy to leave "
              "far too hot without ever noticing."),
    RDS_PARAM(AlgorithmConfig, "SlotGain", "fScrapeLoop", kFloat, slotGains.scrapeLoop, -24,
              12, 0.5, "Slot gains", "scrape_loop",
              "The grinding loop. Sits about 20 dB under the impacts; if it reads as a hiss rather "
              "than as a rumble the file is wrong, not the gain."),
    RDS_PARAM(AlgorithmConfig, "SlotGain", "fFoleyCloth", kFloat, slotGains.foleyCloth, -24,
              12, 0.5, "Slot gains", "foley_cloth",
              "The cloth bed. It is what papers over the one-shots, so it wants to be felt as "
              "continuity rather than heard as a sound."),
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
                      "Global trim on the wood, stone and soft colour together. Up makes the "
                      "floor material obvious; down makes every surface sound the same, which is "
                      "what vanilla does."),
            "Mix", "fSurfaceTrimDb"),
    Renamed(RDS_PAIRS(AlgorithmConfig, "Surfaces", "fWoodTrimDb", kFloat, surfaces.woodTrimDb, -24,
                      12, 0.5, "Surfaces", "surf_wood",
                      "The hollow knock, alone. Wood is the surface most likely to arrive louder "
                      "than the other two, because a hollow knock is easy to record hot."),
            "SlotGain", "fSurfWood"),
    Renamed(RDS_PARAM(AlgorithmConfig, "Surfaces", "fStoneTrimDb", kFloat, surfaces.stoneTrimDb,
                      -24, 12, 0.5, "Surfaces", "surf_stone",
                      "The hard short skin, alone. Short files read quiet at the same peak, so "
                      "this is usually the one that needs lifting."),
            "SlotGain", "fSurfStone"),
    Renamed(RDS_PARAM(AlgorithmConfig, "Surfaces", "fSoftTrimDb", kFloat, surfaces.softTrimDb, -24,
                      12, 0.5, "Surfaces", "surf_soft",
                      "The dull skin, and the one anything unresolved falls back to - so it plays "
                      "far more often than the other two and is worth setting last."),
            "SlotGain", "fSurfSoft"),
    Renamed(RDS_HPAIR(AlgorithmConfig, "Surfaces", "bWood", kBool, surfaces.wood, 0, 1, 1,
                      "Surfaces", "Play surf_wood",
                      "The hollow knock layered on when a body lands on wood. Muted at render, "
                      "so every arbitration decision stays identical and only the sound goes."),
            "Layers", "bSurfWood"),
    Renamed(RDS_PARAM(AlgorithmConfig, "Surfaces", "bStone", kBool, surfaces.stone, 0, 1, 1,
                      "Surfaces", "Play surf_stone",
                      "The hard, short skin for stone. Vanilla plays the same dirt sample for "
                      "stone, ice and carpet alike, so this is one of the differences the mod "
                      "exists to make."),
            "Layers", "bSurfStone"),
    Renamed(RDS_PARAM(AlgorithmConfig, "Surfaces", "bSoft", kBool, surfaces.soft, 0, 1, 1,
                      "Surfaces", "Play surf_soft",
                      "The dull skin, and the fallback for every surface we cannot resolve - "
                      "which today means all natural ground, since no dirt, grass or snow contact "
                      "exists in the capture set."),
            "Layers", "bSurfSoft"),

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

    // -- Stage 3: HeadImpact, damage -------------------------------------------
    RDS_HRULE(AlgorithmConfig, "HeadImpact", "bDamageEnabled", kBool,
              strategies.head.damageEnabled, 0, 1, 1, "Head impact", "Head damage",
              "Let a hard head strike break something, on top of the accent. Two thresholds: the "
              "first places a crunch just after the skull, the second adds the wet layer with it. "
              "Deterministic, unlike the body's crunch, which is a probability gate - how bad a "
              "head landing was is not a matter of chance."),
    RDS_PAIRS(AlgorithmConfig, "HeadImpact", "fCrunchAtFrac", kFloat,
              strategies.head.crunchAtFrac, 0, 3, 0.01, "Head impact", "Crunch threshold",
              "Where bone starts breaking, as a multiple of the loud anchor. Under the body's own "
              "crunch gate and every hard head landing cracks; well over it and only a real fall "
              "does."),
    RDS_PARAM(AlgorithmConfig, "HeadImpact", "fGoreAtFrac", kFloat, strategies.head.goreAtFrac, 0,
              3, 0.01, "Head impact", "Gore threshold",
              "Where the wet layer joins it. Also the point the crunch ramp stops climbing, so "
              "the two hand over rather than both getting louder over the top of the range."),
    RDS_PARAM(AlgorithmConfig, "HeadImpact", "fGoreFullFrac", kFloat,
              strategies.head.goreFullFrac, 0, 4, 0.01, "Head impact", "Gore ramp ceiling",
              "Where the gore reaches its loud end. Past it a harder strike is bigger through the "
              "composite and nothing here changes."),
    RDS_HPAIR(AlgorithmConfig, "HeadImpact", "fCrunchDelayMs", kFloat,
              strategies.head.crunchDelayMs, 0, 100, 1, "Head impact", "Crunch delay",
              "How long after the skull the crunch lands. Zero puts it inside the impact, where "
              "it reads as texture; a beat later and it reads as consequence."),
    RDS_PARAM(AlgorithmConfig, "HeadImpact", "fGoreDelayMs", kFloat, strategies.head.goreDelayMs,
              0, 100, 1, "Head impact", "Gore delay",
              "The same for the wet layer. Behind the crunch by default, so the pair lands as one "
              "gesture rather than one sound."),
    RDS_HPAIR(AlgorithmConfig, "HeadImpact", "fCrunchQuietDb", kFloat,
              strategies.head.crunchQuietDb, -60, 12, 0.5, "Head impact", "Crunch at threshold",
              "How loud the crunch is the moment it opens. Well down, or crossing the threshold "
              "becomes an event in itself instead of the start of a ramp."),
    RDS_PARAM(AlgorithmConfig, "HeadImpact", "fCrunchLoudDb", kFloat,
              strategies.head.crunchLoudDb, -60, 12, 0.5, "Head impact", "Crunch at the cap",
              "...and how loud it is by the time the gore threshold takes over. It climbs no "
              "further than this."),
    RDS_PAIRS(AlgorithmConfig, "HeadImpact", "fGoreQuietDb", kFloat, strategies.head.goreQuietDb,
              -60, 12, 0.5, "Head impact", "Gore at threshold",
              "The same pair for the wet layer: barely there where it opens..."),
    RDS_PARAM(AlgorithmConfig, "HeadImpact", "fGoreLoudDb", kFloat, strategies.head.goreLoudDb,
              -60, 12, 0.5, "Head impact", "Gore at the ceiling",
              "...and what it comes to at the top of its own ramp."),

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

    // -- Stage 3: CrunchGore --------------------------------------------------
    RDS_PARAM(AlgorithmConfig, "CrunchGore", "bCrunchEnabled", kBool,
              strategies.crunch.crunchEnabled, 0, 1, 1, "Crunch and gore", "Crunch enabled",
              "The granular bone-break layer. This is most of what makes the mod gnarly."),
    RDS_PARAM(AlgorithmConfig, "CrunchGore", "bGoreEnabled", kBool, strategies.crunch.goreEnabled,
              0, 1, 1, "Crunch and gore", "Gore enabled",
              "The wet layer, above the obliterate tier. Nothing a fall can produce reaches it."),
    RDS_HPAIR(AlgorithmConfig, "CrunchGore", "fCrunchGateFrac", kFloat,
              strategies.crunch.crunchGateFrac, 0, 3, 0.01, "Crunch and gore", "Crunch gate",
              "Where a crunch becomes possible, as a multiple of the loud anchor. An ordinary "
              "shove peaks at about half of it and a three-metre fall at three quarters, so the "
              "default has an ordinary knockdown crack occasionally and a real fall crack "
              "always."),
    RDS_PARAM(AlgorithmConfig, "CrunchGore", "fCrunchCertainFrac", kFloat,
              strategies.crunch.crunchCertainFrac, 0, 4, 0.01, "Crunch and gore", "Crunch certain",
              "Where a crunch becomes guaranteed, on the same scale. The span between this and the "
              "gate is the probability ramp - soften a gate with probability, never with volume. "
              "At or below the gate it is a hard threshold instead."),
    RDS_PARAM(AlgorithmConfig, "CrunchGore", "fCrunchGateProbability", kFloat,
              strategies.crunch.crunchGateProbability, 0, 1, 0.01, "Crunch and gore",
              "Chance at the gate",
              "How likely a crunch is at the gate itself, before the ramp adds anything. At zero "
              "the bottom of the ramp is dead and crunches really start well above where the gate "
              "says; at one the ramp is bypassed and the gate is hard."),
    RDS_PARAM(AlgorithmConfig, "CrunchGore", "fCrunchHysteresisFrac", kFloat,
              strategies.crunch.crunchHysteresisFrac, 0, 1, 0.01, "Crunch and gore",
              "Crunch hysteresis",
              "How far back below the gate the impact has to fall before another crunch is "
              "possible, on the same scale as the gate, so it does not flicker on and off during "
              "a tumble."),
    RDS_PARAM(AlgorithmConfig, "CrunchGore", "iMaxCrunchesPerEvent", kInt,
              strategies.crunch.maxCrunchesPerEvent, 0, 16, 1, "Crunch and gore",
              "Crunches per fall",
              "The ceiling per knockdown, so a long tumble does not turn into a bag of breaking "
              "sticks."),
    RDS_HRULE(AlgorithmConfig, "CrunchGore", "fGoreGateFrac", kFloat,
              strategies.crunch.goreGateFrac, 0.2, 6, 0.02, "Crunch and gore", "Gore gate",
              "Where the wet layer opens, as a multiple of the loud anchor. Should sit above "
              "anything a fall can produce, and the obliterate point has to be cleared as well."),
    RDS_HPAIR(AlgorithmConfig, "CrunchGore", "fCrunchGainDb", kFloat,
              strategies.crunch.crunchGainDb, -40, 12, 0.5, "Crunch and gore", "Crunch level",
              "How loud a crunch is against the impact it rides on."),
    RDS_PARAM(AlgorithmConfig, "CrunchGore", "fGoreGainDb", kFloat, strategies.crunch.goreGainDb,
              -40, 12, 0.5, "Crunch and gore", "Gore level", "How loud the wet layer is."),

    // -- Stage 3: ScrapeLoop --------------------------------------------------
    RDS_PARAM(AlgorithmConfig, "ScrapeLoop", "bEnabled", kBool, strategies.scrape.enabled, 0, 1, 1,
              "Scrape loop", "Enabled",
              "The sustained grinding loop for a body dragging along a surface. When a slide "
              "starts and stops is not decided here - it is the motion axis' Slide state, under "
              "[Motion] - and this drawer only decides what one sounds like."),
    RDS_HPAIR(AlgorithmConfig, "ScrapeLoop", "fStartFadeMs", kFloat, strategies.scrape.startFadeMs,
              0, 1000, 5, "Scrape loop", "Fade in",
              "How gently the grind arrives. Too short and it clicks in at the start of a slide. "
              "The fades are the only thing here that is not the body's speed, and their job is "
              "to hide the transition rather than to shape the level."),
    RDS_PARAM(AlgorithmConfig, "ScrapeLoop", "fStopFadeMs", kFloat, strategies.scrape.stopFadeMs, 0,
              2000, 5, "Scrape loop", "Fade out",
              "How gently it leaves when the slide ends on the ground - because the body stopped, "
              "or because something stopped it. Longer than the fade in, or a slide ends "
              "abruptly."),
    RDS_PARAM(AlgorithmConfig, "ScrapeLoop", "fLaunchFadeMs", kFloat,
              strategies.scrape.launchFadeMs, 0, 2000, 5, "Scrape loop", "Launch fade",
              "And how gently it leaves when the slide ended because the body went airborne, "
              "which is faster: the surface is simply gone. Set this as long as the fade out and "
              "a grinding rumble trails a body that is already in the air."),
    RDS_HRULE(AlgorithmConfig, "ScrapeLoop", "fGainDb", kFloat, strategies.scrape.gainDb, -60, 6,
              0.5, "Scrape loop", "Level",
              "How loud the grind sits under the impacts at full speed. The references put a "
              "slide 15-25 dB under them."),
    RDS_PAIRS(AlgorithmConfig, "ScrapeLoop", "fSpeedForMinGain", kFloat,
              strategies.scrape.speedForMinGain, 0, 2000, 10, "Scrape loop", "Quiet at",
              "The body speed the loop is quietest at. Level and pitch track the measured centre "
              "of mass, so a slide is exactly as loud as the body is fast."),
    RDS_PARAM(AlgorithmConfig, "ScrapeLoop", "fSpeedForMaxGain", kFloat,
              strategies.scrape.speedForMaxGain, 0, 4000, 10, "Scrape loop", "Loud at",
              "And the speed it reaches full level at, so the grind tracks the slide rather than "
              "sitting at one level throughout."),
    RDS_PARAM(AlgorithmConfig, "ScrapeLoop", "fSpeedRangeDb", kFloat,
              strategies.scrape.speedRangeDb, -60, 0, 0.5, "Scrape loop", "Speed range",
              "How far under its level the loop sits at the quiet end - the depth of the speed "
              "dependence. At 0 the slide is one level whatever it is doing; deep enough that a "
              "crawl is inaudible is what keeps a body settling on its side from sounding like it "
              "is being dragged."),
    RDS_PARAM(AlgorithmConfig, "ScrapeLoop", "fPitchPerThousandUnits", kFloat,
              strategies.scrape.pitchPerThousandUnits, 0, 2, 0.01, "Scrape loop",
              "Pitch with speed",
              "How much faster the grind plays as the slide speeds up, per thousand units/s of "
              "measured body speed. A little sells the acceleration; a lot sounds like a tape "
              "being spooled."),

    // -- Stage 3: MotionFoley -------------------------------------------------
    RDS_PARAM(AlgorithmConfig, "MotionFoley", "bEnabled", kBool, strategies.foley.enabled, 0, 1, 1,
              "Motion foley", "Enabled",
              "The continuous cloth bed under everything. It is what papers over the one-shots and "
              "makes a fall sound like one continuous thing."),
    RDS_PARAM(AlgorithmConfig, "MotionFoley", "fBedGainDb", kFloat, strategies.foley.bedGainDb, -80,
              6, 0.5, "Motion foley", "Bed level",
              "How loud the bed is. The references put it 30-36 dB under the hero hit; bring it up "
              "and it stops being a bed and starts being a sound."),
    RDS_PAIRS(AlgorithmConfig, "MotionFoley", "fSpeedForMinGain", kFloat,
              strategies.foley.speedForMinGain, 0, 1000, 5, "Motion foley", "Quiet at",
              "The body speed the bed is quietest at."),
    RDS_PARAM(AlgorithmConfig, "MotionFoley", "fSpeedForMaxGain", kFloat,
              strategies.foley.speedForMaxGain, 0, 3000, 10, "Motion foley", "Loud at",
              "And the speed it reaches full level at, so the cloth tracks how fast the body is "
              "actually moving."),
    RDS_HRULE(AlgorithmConfig, "MotionFoley", "bAirborneRise", kBool, strategies.foley.airborneRise,
              0, 1, 1, "Motion foley", "Airborne rise",
              "The anticipation whoosh while a body is in the air. On by default at a low level: "
              "it tells the ear something is about to land."),
    RDS_PARAM(AlgorithmConfig, "MotionFoley", "fAirborneRiseGainDb", kFloat,
              strategies.foley.airborneRiseGainDb, -80, 6, 0.5, "Motion foley", "Rise level",
              "How loud the whoosh is. It should be felt rather than heard."),
    RDS_HRULE(AlgorithmConfig, "MotionFoley", "bPreImpactDuck", kBool,
              strategies.foley.preImpactDuck, 0, 1, 1, "Motion foley", "Pre-impact duck",
              "Drop the bed for a moment just before a big landing so the hit lands harder. Three "
              "of four reference clips do it, which is not enough to be sure it was deliberate, so "
              "it ships off and is meant to be tuned by ear."),
    RDS_PARAM(AlgorithmConfig, "MotionFoley", "fPreImpactDuckDb", kFloat,
              strategies.foley.preImpactDuckDb, -40, 0, 0.5, "Motion foley", "Duck depth",
              "How far the bed drops for the duck. The references measure 8-15 dB."),
    RDS_PARAM(AlgorithmConfig, "MotionFoley", "fPreImpactDuckMs", kFloat,
              strategies.foley.preImpactDuckMs, 0, 500, 5, "Motion foley", "Duck length",
              "How long before the landing the duck starts."),

    // -- Stage 3: SettleClose -------------------------------------------------
    RDS_PARAM(AlgorithmConfig, "SettleClose", "bEnabled", kBool, strategies.settle.enabled, 0, 1, 1,
              "Settle", "Enabled",
              "One quiet cue when the fall is over. Falls that just trail off feel unfinished."),
    RDS_PARAM(AlgorithmConfig, "SettleClose", "fGainDb", kFloat, strategies.settle.gainDb, -60, 6,
              0.5, "Settle", "Level", "How loud the closing flop is. It is punctuation, not an event."),
    RDS_PARAM(AlgorithmConfig, "SettleClose", "fDelayMs", kFloat, strategies.settle.delayMs, 0,
              2000, 10, "Settle", "Delay",
              "How long after the body goes quiet the closing cue lands. Too soon and it sounds "
              "like another impact; too late and the fall has already been forgotten."),

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
              "Below this a sound is not worth a voice and is dropped before the cap ever sees it. "
              "Raise it to keep the voice budget for things that can actually be heard."),

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
    RDS_HRULE(AlgorithmConfig, "Compress", "fImpactDb", kFloat, compress.impactDb, -60, 0, 0.5,
              "Compress", "Impact",
              "Where a four-layer impact starts being held, in dB under the loudest thing this "
              "mod can produce. 0 is that top, so it holds nothing. The whole stack comes down "
              "together, so a held impact keeps its transient/body/sub balance and loses only "
              "level. Its surface skin is part of the same moment and rides on this."),
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
    RDS_PARAM(AlgorithmConfig, "Compress", "fFoleyDb", kFloat, compress.foleyDb, -60, 0, 0.5,
              "Compress", "Foley bed",
              "Where the cloth bed starts being held, in dB under the mod's loudest."),
    RDS_PARAM(AlgorithmConfig, "Compress", "fAirborneDb", kFloat, compress.airborneDb, -60, 0, 0.5,
              "Compress", "Airborne rise",
              "Where the airborne whoosh starts being held, in dB under the mod's loudest."),
    RDS_PARAM(AlgorithmConfig, "Compress", "fSettleDb", kFloat, compress.settleDb, -60, 0, 0.5,
              "Compress", "Settle",
              "Where the closing cue starts being held, in dB under the mod's loudest."),

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
              "The granular low-mid crackle that is what a bone break actually is - ten times the transient density of a plain thud in that band, not a snap sample."),
    RDS_PARAM(AlgorithmConfig, "Layers", "bGoreWet", kBool, layers.goreWet, 0, 1, 1, "Layers",
              "gore_wet",
              "The wet squelch, obliterate tier only. The fastest way to check the gore gate is not opening on ordinary knockdowns."),
    RDS_PARAM(AlgorithmConfig, "Layers", "bScrapeLoop", kBool, layers.scrapeLoop, 0, 1, 1, "Layers",
              "scrape_loop",
              "The sliding rumble. The A/B for the slide as a whole: mute it and the state, the "
              "budget and the slide-end impact all still happen, so what you are hearing is "
              "everything the slide does apart from the grind itself."),
    RDS_PARAM(AlgorithmConfig, "Layers", "bFoleyCloth", kBool, layers.foleyCloth, 0, 1, 1, "Layers",
              "foley_cloth",
              "The continuous cloth bed under everything, 30-36 dB down. Mute it to hear how much work it does papering over the one-shots - the references say a great deal."),
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

}  // namespace

std::span<const ParamDesc> AlgorithmParams() {
    return std::span<const ParamDesc>{kAlgorithmParams};
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
