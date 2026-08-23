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

/// Same idea for a char array. There is no default to read out of a
/// value-initialised config - it is always the empty string - and no range, so
/// the numeric columns carry the buffer size instead of a min and a max.
#define RDS_PARAM_STR(ROOT, SECTION, KEY, MEMBER, GROUP, LABEL, TOOLTIP)                        ParamDesc {                                                                                     SECTION, KEY, ParamType::kString, offsetof(ROOT, MEMBER), 0, 0, 0, 0, GROUP, LABEL,             TOOLTIP, {}, sizeof(ROOT{}.MEMBER)                                                  }

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
    RDS_PARAM(AlgorithmConfig, "Ingest", "fBlowupDisagreeFrac", kFloat, ingest.blowupDisagreeFrac,
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
    RDS_PARAM(AlgorithmConfig, "Ingest", "fSelfContactThreshold", kFloat,
              ingest.selfContactThreshold, 0, 2000, 10, "Ingest", "Self-contact threshold",
              "Half of all contacts are one limb touching another limb of the same body. Below "
              "this they only thicken the cloth bed; above it they play as real impacts. Drop it "
              "and a ragdoll rattles against itself constantly."),
    RDS_PARAM(AlgorithmConfig, "Ingest", "bDropMirroredSelfContacts", kBool,
              ingest.dropMirroredSelfContacts, 0, 1, 1, "Ingest", "Drop mirrored self-contacts",
              "Every limb-on-own-limb contact is reported twice, once from each limb. Off, they "
              "all play twice and the whole mix doubles at the moment of landing."),
    RDS_PARAM(AlgorithmConfig, "Ingest", "fFrameGapMs", kFloat, ingest.frameGapMs, 0.1, 20, 0.1,
              "Ingest", "Frame gap",
              "How large a gap between contact reports means a new frame started. Only matters on "
              "replay; the measured split is one microsecond inside a frame against 20 ms "
              "between."),
    RDS_PARAM(AlgorithmConfig, "Ingest", "bCollapseManifolds", kBool, ingest.collapseManifolds, 0,
              1, 1, "Ingest", "Collapse manifolds",
              "One collision surface reported at several points becomes one contact at the "
              "strongest of them. Off, a flat landing on a floor plays several times over."),
    RDS_PARAM(AlgorithmConfig, "Ingest", "fGrazeRatio", kFloat, ingest.grazeRatio, 0.2, 10, 0.1,
              "Ingest", "Graze ratio",
              "How much faster a contact must be sliding along a surface than into it before it is "
              "heard as a scrape rather than a thud. Lower sends more of a tumble to the scrape "
              "loop and takes the punch out of it."),
    RDS_PARAM(AlgorithmConfig, "Ingest", "fGrazeMaxImpactSpeed", kFloat,
              ingest.grazeMaxImpactSpeed, 0, 4000, 10, "Ingest", "Graze speed ceiling",
              "Above this closing speed a contact is always a thud, however much it is also "
              "sliding. Raise it and hard skids go quiet as the scrape path swallows them; lower "
              "it and genuine slides start thudding."),

    // -- Stage 2: Phase machine ----------------------------------------------
    RDS_PARAM(AlgorithmConfig, "Phase", "fAirborneMinHeight", kFloat, phase.airborneMinHeight, 0,
              500, 5, "Phase", "Airborne height",
              "How far off the ground the body has to be before the airborne rise starts. Too low "
              "and the whoosh fires during an ordinary shove."),
    RDS_PARAM(AlgorithmConfig, "Phase", "fAirborneMinTimeMs", kFloat, phase.airborneMinTimeMs, 0,
              2000, 10, "Phase", "Airborne time",
              "And for how long, so a body that bounces once does not read as a fall."),
    RDS_PARAM(AlgorithmConfig, "Phase", "fPrimaryImpactEnergyFrac", kFloat,
              phase.primaryImpactEnergyFrac, 0.01, 1.0, 0.01, "Phase", "Hero energy fraction",
              "How big a share of the fall's energy a contact must carry to count as the hero "
              "moment. Higher means fewer knockdowns get a hero treatment and more of them read as "
              "a flat tumble."),
    RDS_PARAM(AlgorithmConfig, "Phase", "fPrimaryImpactWindowMs", kFloat,
              phase.primaryImpactWindowMs, 20, 2000, 10, "Phase", "Hero window",
              "How long the hero moment stays open for its peers. A faceplant genuinely has a "
              "knee, a chest and a head arriving inside a couple of hundred milliseconds; shorten "
              "this and only the first of them is loud."),
    RDS_PARAM(AlgorithmConfig, "Phase", "fSlideMinTangentSpeed", kFloat, phase.slideMinTangentSpeed,
              0, 2000, 10, "Phase", "Slide speed",
              "How fast a body has to be travelling along the ground before the slide phase opens "
              "and the grinding loop is allowed."),
    RDS_PARAM(AlgorithmConfig, "Phase", "fSlideMinDurationMs", kFloat, phase.slideMinDurationMs, 0,
              3000, 10, "Phase", "Slide duration",
              "And for how long, so a single glancing blow does not read as a slide."),
    RDS_PARAM(AlgorithmConfig, "Phase", "fSettleEnergyFloor", kFloat, phase.settleEnergyFloor, 0,
              1000, 5, "Phase", "Settle floor",
              "The energy the fall has to drop below before it is considered over. Raise it and "
              "knockdowns close early, cutting off the last real bounce; lower it and the closing "
              "cue arrives late."),
    RDS_PARAM(AlgorithmConfig, "Phase", "fSettleQuietMs", kFloat, phase.settleQuietMs, 0, 5000, 10,
              "Phase", "Settle quiet",
              "How much quiet is needed before the event closes. This is what stops a fall that "
              "pauses mid-tumble playing its closing cue too early."),
    RDS_PARAM(AlgorithmConfig, "Phase", "fGetUpBlendMs", kFloat, phase.getUpBlendMs, 0, 3000, 10,
              "Phase", "Get-up silence",
              "Silence held after a ragdoll ends, covering the blend back to animation. A guess: "
              "every capture take was paralysed and none of them ever got up."),
    RDS_PARAM(AlgorithmConfig, "Phase", "fLaunchTrimDb", kFloat, phase.launch.gainTrimDb, -60, 12,
              0.5, "Phase budgets", "Launch trim",
              "How loud the moment of being knocked over is, before anything has landed."),
    RDS_PARAM(AlgorithmConfig, "Phase", "iLaunchMaxCues", kInt, phase.launch.maxCuesPerBurst, 0, 16,
              1, "Phase budgets", "Launch grains", "How many sounds the launch may spend at once."),
    RDS_PARAM(AlgorithmConfig, "Phase", "fAirborneTrimDb", kFloat, phase.airborne.gainTrimDb, -60,
              12, 0.5, "Phase budgets", "Airborne trim",
              "How loud anything is while the body is in the air. Should be well down: the "
              "airborne section is anticipation, not event."),
    RDS_PARAM(AlgorithmConfig, "Phase", "iAirborneMaxCues", kInt, phase.airborne.maxCuesPerBurst, 0,
              16, 1, "Phase budgets", "Airborne grains",
              "How many sounds may play while airborne."),
    RDS_PARAM(AlgorithmConfig, "Phase", "fPrimaryImpactTrimDb", kFloat,
              phase.primaryImpact.gainTrimDb, -60, 12, 0.5, "Phase budgets", "Hero trim",
              "The hero moment's own trim. This is the reference every other phase is quieter "
              "than, so moving it moves the whole knockdown."),
    RDS_PARAM(AlgorithmConfig, "Phase", "iPrimaryImpactMaxCues", kInt,
              phase.primaryImpact.maxCuesPerBurst, 0, 16, 1, "Phase budgets", "Hero grains",
              "How many grains the hero burst may contain. The references measure three to five."),
    RDS_PARAM(AlgorithmConfig, "Phase", "fTumbleTrimDb", kFloat, phase.tumble.gainTrimDb, -60, 12,
              0.5, "Phase budgets", "Tumble trim",
              "How loud the rolling section after the first landing is."),
    RDS_PARAM(AlgorithmConfig, "Phase", "iTumbleMaxCues", kInt, phase.tumble.maxCuesPerBurst, 0, 16,
              1, "Phase budgets", "Tumble grains", "How many grains a tumble burst may contain."),
    RDS_PARAM(AlgorithmConfig, "Phase", "fSlideTrimDb", kFloat, phase.slide.gainTrimDb, -60, 12,
              0.5, "Phase budgets", "Slide trim",
              "How loud the impacts during a slide are. The grinding loop has its own gain."),
    RDS_PARAM(AlgorithmConfig, "Phase", "iSlideMaxCues", kInt, phase.slide.maxCuesPerBurst, 0, 16,
              1, "Phase budgets", "Slide grains",
              "How many one-shots may punctuate a slide, for the moments a limb catches."),
    RDS_PARAM(AlgorithmConfig, "Phase", "fSettleTrimDb", kFloat, phase.settle.gainTrimDb, -60, 12,
              0.5, "Phase budgets", "Settle trim",
              "The last twenty contacts of every knockdown are settles and should be nearly "
              "silent. This one number is what keeps a fall from ending in a rattle."),
    RDS_PARAM(AlgorithmConfig, "Phase", "iSettleMaxCues", kInt, phase.settle.maxCuesPerBurst, 0, 16,
              1, "Phase budgets", "Settle grains",
              "How many sounds the settle may spend. One is the design."),

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
    RDS_PARAM(AlgorithmConfig, "Arbitration", "fChainMergeWindowMs", kFloat, arb.chainMergeWindowMs,
              0, 500, 5, "Arbitration", "Chain merge window",
              "How long one limb chain stays claimed by its strongest contact. This is what turns "
              "a hand, a wrist and an elbow landing together into one arm flop."),
    RDS_PARAM(AlgorithmConfig, "Arbitration", "fMaskDropBelowDb", kFloat, arb.maskDropBelowDb, 1,
              60, 0.5, "Arbitration", "Masking depth",
              "How far under the loudest recent sound a proposal has to be before it is dropped "
              "outright rather than played quietly. Small values let everything through and the "
              "landing turns to porridge; large values leave only the heroes."),
    RDS_PARAM(AlgorithmConfig, "Arbitration", "fMaskDecayDbPerSec", kFloat, arb.maskDecayDbPerSec,
              1, 400, 5, "Arbitration", "Masking recovery",
              "How fast the ear is assumed to recover after a loud hit. Slower means a big landing "
              "keeps the following second quiet."),
    RDS_PARAM(AlgorithmConfig, "Arbitration", "iBurstMaxGrains", kInt, arb.burstMaxGrains, 1, 16, 1,
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
    RDS_PARAM(AlgorithmConfig, "Arbitration", "bSpatialCollapseOnHero", kBool,
              arb.spatialCollapseOnHero, 0, 1, 1, "Arbitration", "Collapse hero to one point",
              "During a hero moment, place every layer at one point. Several points read as "
              "several events; one point reads as one event with detail."),
    RDS_PARAM(AlgorithmConfig, "Arbitration", "fSpatialCollapseWindowMs", kFloat,
              arb.spatialCollapseWindowMs, 0, 1000, 10, "Arbitration", "Collapse window",
              "How long the collapse holds before the sound spreads back out across the limbs for "
              "the tumble."),
    RDS_PARAM(AlgorithmConfig, "Arbitration", "iVoiceCapPerActor", kInt, arb.voiceCapPerActor, 1,
              32, 1, "Arbitration", "Voices per actor",
              "The hard ceiling on how many sounds one falling body may hold at once."),
    RDS_PARAM(AlgorithmConfig, "Arbitration", "iVoiceCapGlobal", kInt, arb.voiceCapGlobal, 1, 64, 1,
              "Arbitration", "Voices overall",
              "The same for the whole scene, so a battlefield of ragdolls stays a mix rather than "
              "a wall."),
    RDS_PARAM(AlgorithmConfig, "Arbitration", "fFrameScaleK", kFloat, arb.frameScaleK, 0, 10, 0.25,
              "Arbitration", "Frame scaling",
              "How many frames every window is worth as well as its millisecond floor, so the "
              "system behaves the same at 24 fps and at 144. Zero pins every window to its floor "
              "and makes the mod frame-rate dependent."),

    // -- Intensity ------------------------------------------------------------
    RDS_PARAM(AlgorithmConfig, "Intensity", "fSpeedRefLow", kFloat, intensity.speedRefLow, 0, 500,
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
    RDS_PARAM(AlgorithmConfig, "Intensity", "fCurveExponent", kFloat, intensity.curveExponent, 0.1,
              4.0, 0.05, "Intensity", "Curve shape",
              "Where the loudness sits between the two anchors. Under 1 fills the middle out and "
              "makes ordinary contacts more present; over 1 keeps everything quiet until it is "
              "genuinely hard."),
    RDS_PARAM(AlgorithmConfig, "Intensity", "fSoftClipKnee", kFloat, intensity.softClipKnee, 0.1,
              1.0, 0.05, "Intensity", "Soft-clip knee",
              "Where the curve stops being straight and starts compressing, so another mod's "
              "absurd impulse gets loud rather than rejected. A silent obliterate is the worst "
              "possible outcome."),
    RDS_PARAM(AlgorithmConfig, "Intensity", "fMassWeight", kFloat, intensity.massWeight, 0, 2, 0.05,
              "Intensity", "Mass weight",
              "How much a limb's own size makes it louder. At zero a fingertip and a torso at the "
              "same speed sound identical; high values make torso landings dominate everything."),
    RDS_PARAM(AlgorithmConfig, "Intensity", "fRadiusWeight", kFloat, intensity.radiusWeight, 0, 2,
              0.05, "Intensity", "Radius weight",
              "The same, from the limb's physical bounding radius rather than its mass. This is "
              "the half that still works on a skeleton we do not recognise."),
    RDS_PARAM(AlgorithmConfig, "Intensity", "fObliterateFrac", kFloat, intensity.obliterateFrac,
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
    RDS_PARAM(AlgorithmConfig, "PostIntensity", "fCurveExponent", kFloat,
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
    RDS_PARAM(AlgorithmConfig, "SlotGain", "fSurfWood", kFloat, slotGains.surfWood, -24, 12, 0.5,
              "Slot gains", "surf_wood",
              "The hollow knock, alone. Wood is the surface most likely to arrive louder than the "
              "other two, because a hollow knock is easy to record hot."),
    RDS_PARAM(AlgorithmConfig, "SlotGain", "fSurfStone", kFloat, slotGains.surfStone, -24, 12, 0.5,
              "Slot gains", "surf_stone",
              "The hard short skin, alone. Short files read quiet at the same peak, so this is "
              "usually the one that needs lifting."),
    RDS_PARAM(AlgorithmConfig, "SlotGain", "fSurfSoft", kFloat, slotGains.surfSoft, -24, 12, 0.5,
              "Slot gains", "surf_soft",
              "The dull skin, and the one anything unresolved falls back to - so it plays far more "
              "often than the other two and is worth setting last."),
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
    RDS_PARAM(AlgorithmConfig, "Limbs", "fMinBodySpeed", kFloat, limbs.minBodySpeed, 0, 2000, 10,
              "Limbs", "Minimum carrying speed",
              "A limb moving slower than this delivers nothing however straight its path is. Stops "
              "a body settling slowly onto the floor being priced as a landing."),

    // -- Stage 3: ImpactComposite --------------------------------------------
    RDS_PARAM(AlgorithmConfig, "ImpactComposite", "bEnabled", kBool, strategies.impact.enabled, 0,
              1, 1, "Impact composite", "Enabled",
              "The core layer stack. Off, the mod plays only accents and loops and every impact "
              "loses its body."),
    RDS_PARAM(AlgorithmConfig, "ImpactComposite", "fTransientOffsetMs", kFloat,
              strategies.impact.transientOffsetMs, -50, 200, 1, "Impact composite",
              "Transient offset",
              "When the bright contact click lands. This is the reference everything else is "
              "measured from and should stay at zero."),
    RDS_PARAM(AlgorithmConfig, "ImpactComposite", "fSurfaceOffsetMs", kFloat,
              strategies.impact.surfaceOffsetMs, -50, 300, 1, "Impact composite", "Surface offset",
              "When the wood, stone or soft colour arrives. Close to the transient, or it stops "
              "reading as the same event."),
    RDS_PARAM(AlgorithmConfig, "ImpactComposite", "fBodyOffsetMs", kFloat,
              strategies.impact.bodyOffsetMs, -50, 400, 1, "Impact composite", "Body offset",
              "When the low-mid flesh and mass arrives. Measured at 8-34 ms in the references."),
    RDS_PARAM(AlgorithmConfig, "ImpactComposite", "fSubOffsetMs", kFloat,
              strategies.impact.subOffsetMs, -50, 400, 1, "Impact composite", "Sub offset",
              "When the pitched boom arrives. The single most important number in the mod: at "
              "65 ms an impact reads as mass, at 0 it reads as a click with a thud stuck to it, "
              "and past 120 ms it stops belonging to the impact at all."),
    RDS_PARAM(AlgorithmConfig, "ImpactComposite", "fOffsetScatterMs", kFloat,
              strategies.impact.offsetScatterMs, 0, 50, 0.5, "Impact composite", "Offset scatter",
              "A few milliseconds of variation on each layer so no two composites have exactly the "
              "same envelope. Large values smear the shape instead of varying it."),
    RDS_PARAM(AlgorithmConfig, "ImpactComposite", "fTransientGainAtMinDb", kFloat,
              strategies.impact.transientGainAtMinDb, -60, 12, 0.5, "Impact composite",
              "Transient at quiet",
              "How much of the stack is the bright click on the lightest contact. Light contacts "
              "are almost all transient."),
    RDS_PARAM(AlgorithmConfig, "ImpactComposite", "fTransientGainAtMaxDb", kFloat,
              strategies.impact.transientGainAtMaxDb, -60, 12, 0.5, "Impact composite",
              "Transient at loud", "And on the heaviest, where it only rides on top of the sub."),
    RDS_PARAM(AlgorithmConfig, "ImpactComposite", "fBodyGainAtMinDb", kFloat,
              strategies.impact.bodyGainAtMinDb, -60, 12, 0.5, "Impact composite", "Body at quiet",
              "How much flesh a light contact has."),
    RDS_PARAM(AlgorithmConfig, "ImpactComposite", "fBodyGainAtMaxDb", kFloat,
              strategies.impact.bodyGainAtMaxDb, -60, 12, 0.5, "Impact composite", "Body at loud",
              "And a heavy one."),
    RDS_PARAM(AlgorithmConfig, "ImpactComposite", "fSubGainAtMinDb", kFloat,
              strategies.impact.subGainAtMinDb, -80, 12, 0.5, "Impact composite", "Sub at quiet",
              "How much boom a light tap gets. Should be almost none, or every tap sounds like a "
              "cannon."),
    RDS_PARAM(AlgorithmConfig, "ImpactComposite", "fSubGainAtMaxDb", kFloat,
              strategies.impact.subGainAtMaxDb, -80, 12, 0.5, "Impact composite", "Sub at loud",
              "And a hard landing, where it is the loudest thing in the stack. This is where the "
              "gnarl comes from."),
    RDS_PARAM(AlgorithmConfig, "ImpactComposite", "fSurfaceGainAtMinDb", kFloat,
              strategies.impact.surfaceGainAtMinDb, -60, 12, 0.5, "Impact composite",
              "Surface at quiet", "How audible the surface colour is on a light contact."),
    RDS_PARAM(AlgorithmConfig, "ImpactComposite", "fSurfaceGainAtMaxDb", kFloat,
              strategies.impact.surfaceGainAtMaxDb, -60, 12, 0.5, "Impact composite",
              "Surface at loud",
              "And on a heavy one. If the surface reads as a separate sound rather than as colour, "
              "it is too loud."),
    RDS_PARAM(AlgorithmConfig, "ImpactComposite", "fPitchScatterSemis", kFloat,
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
    RDS_PARAM(AlgorithmConfig, "HeadImpact", "fGainDb", kFloat, strategies.head.gainDb, -40, 12,
              0.5, "Head impact", "Accent level", "How loud the head accent is against the stack."),
    RDS_PARAM(AlgorithmConfig, "HeadImpact", "fHeadDownBonus", kFloat,
              strategies.head.headDownBonus, 0, 2, 0.05, "Head impact", "Head-down bonus",
              "How much more willing the gate is when the head is already the low point of the "
              "body - a faceplant rather than a knock."),

    // -- Stage 3: CrunchGore --------------------------------------------------
    RDS_PARAM(AlgorithmConfig, "CrunchGore", "bCrunchEnabled", kBool,
              strategies.crunch.crunchEnabled, 0, 1, 1, "Crunch and gore", "Crunch enabled",
              "The granular bone-break layer. This is most of what makes the mod gnarly."),
    RDS_PARAM(AlgorithmConfig, "CrunchGore", "bGoreEnabled", kBool, strategies.crunch.goreEnabled,
              0, 1, 1, "Crunch and gore", "Gore enabled",
              "The wet layer, above the obliterate tier. Nothing a fall can produce reaches it."),
    RDS_PARAM(AlgorithmConfig, "CrunchGore", "fCrunchGateFrac", kFloat,
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
    RDS_PARAM(AlgorithmConfig, "CrunchGore", "fGoreGateFrac", kFloat,
              strategies.crunch.goreGateFrac, 0.2, 6, 0.02, "Crunch and gore", "Gore gate",
              "Where the wet layer opens, as a multiple of the loud anchor. Should sit above "
              "anything a fall can produce, and the obliterate point has to be cleared as well."),
    RDS_PARAM(AlgorithmConfig, "CrunchGore", "fCrunchGainDb", kFloat,
              strategies.crunch.crunchGainDb, -40, 12, 0.5, "Crunch and gore", "Crunch level",
              "How loud a crunch is against the impact it rides on."),
    RDS_PARAM(AlgorithmConfig, "CrunchGore", "fGoreGainDb", kFloat, strategies.crunch.goreGainDb,
              -40, 12, 0.5, "Crunch and gore", "Gore level", "How loud the wet layer is."),

    // -- Stage 3: ScrapeLoop --------------------------------------------------
    RDS_PARAM(AlgorithmConfig, "ScrapeLoop", "bEnabled", kBool, strategies.scrape.enabled, 0, 1, 1,
              "Scrape loop", "Enabled",
              "The sustained grinding loop for a body dragging along a surface."),
    RDS_PARAM(AlgorithmConfig, "ScrapeLoop", "fMinTangentSpeed", kFloat,
              strategies.scrape.minTangentSpeed, 0, 2000, 10, "Scrape loop", "Minimum speed",
              "How fast the limb has to be travelling along the surface for the loop to be worth "
              "starting."),
    RDS_PARAM(AlgorithmConfig, "ScrapeLoop", "fStartFadeMs", kFloat, strategies.scrape.startFadeMs,
              0, 1000, 5, "Scrape loop", "Fade in",
              "How gently the grind arrives. Too short and it clicks in at the start of a slide."),
    RDS_PARAM(AlgorithmConfig, "ScrapeLoop", "fStopFadeMs", kFloat, strategies.scrape.stopFadeMs, 0,
              2000, 5, "Scrape loop", "Fade out",
              "How gently it leaves. Longer than the fade in, or a slide ends abruptly."),
    RDS_PARAM(AlgorithmConfig, "ScrapeLoop", "fMinDurationMs", kFloat,
              strategies.scrape.minDurationMs, 0, 2000, 10, "Scrape loop", "Minimum duration",
              "How long the contact has to persist before a loop opens, so a single glancing blow "
              "does not start one."),
    RDS_PARAM(AlgorithmConfig, "ScrapeLoop", "fMinDistance", kFloat,
              strategies.scrape.minDistance, 0, 2000, 5, "Scrape loop", "Slide distance",
              "How far a body must travel along a surface to open the slide loop, if it gets there before the time above. A fast skid covers ground in a few frames; without this only slow grinding ever counted as a slide."),
    RDS_PARAM(AlgorithmConfig, "ScrapeLoop", "fFadeInDistance", kFloat,
              strategies.scrape.fadeInDistance, 1, 4000, 10, "Scrape loop",
              "Slide fade-in distance",
              "How far the body slides before the loop reaches full level. Short, and a glancing skid already sounds like being dragged; long, and only a proper slide is fully audible."),
    RDS_PARAM(AlgorithmConfig, "ScrapeLoop", "fFadeInFloorDb", kFloat,
              strategies.scrape.fadeInFloorDb, -60, 0, 1, "Scrape loop",
              "Slide fade-in floor",
              "How far under its level the loop starts before distance walks it up. At 0 the fade is off and the loop opens at full level."),
    RDS_PARAM(AlgorithmConfig, "ScrapeLoop", "fGainDb", kFloat, strategies.scrape.gainDb, -60, 6,
              0.5, "Scrape loop", "Level",
              "How loud the grind sits under the impacts. The references put a slide 15-25 dB "
              "under them."),
    RDS_PARAM(AlgorithmConfig, "ScrapeLoop", "fSpeedForMinGain", kFloat,
              strategies.scrape.speedForMinGain, 0, 2000, 10, "Scrape loop", "Quiet at",
              "The sliding speed the loop is quietest at."),
    RDS_PARAM(AlgorithmConfig, "ScrapeLoop", "fSpeedForMaxGain", kFloat,
              strategies.scrape.speedForMaxGain, 0, 4000, 10, "Scrape loop", "Loud at",
              "And the speed it reaches full level at, so the grind tracks the slide rather than "
              "sitting at one level throughout."),
    RDS_PARAM(AlgorithmConfig, "ScrapeLoop", "fPitchPerThousandUnits", kFloat,
              strategies.scrape.pitchPerThousandUnits, 0, 2, 0.01, "Scrape loop",
              "Pitch with speed",
              "How much faster the grind plays as the slide speeds up. A little sells the "
              "acceleration; a lot sounds like a tape being spooled."),

    // -- Stage 3: MotionFoley -------------------------------------------------
    RDS_PARAM(AlgorithmConfig, "MotionFoley", "bEnabled", kBool, strategies.foley.enabled, 0, 1, 1,
              "Motion foley", "Enabled",
              "The continuous cloth bed under everything. It is what papers over the one-shots and "
              "makes a fall sound like one continuous thing."),
    RDS_PARAM(AlgorithmConfig, "MotionFoley", "fBedGainDb", kFloat, strategies.foley.bedGainDb, -80,
              6, 0.5, "Motion foley", "Bed level",
              "How loud the bed is. The references put it 30-36 dB under the hero hit; bring it up "
              "and it stops being a bed and starts being a sound."),
    RDS_PARAM(AlgorithmConfig, "MotionFoley", "fSpeedForMinGain", kFloat,
              strategies.foley.speedForMinGain, 0, 1000, 5, "Motion foley", "Quiet at",
              "The body speed the bed is quietest at."),
    RDS_PARAM(AlgorithmConfig, "MotionFoley", "fSpeedForMaxGain", kFloat,
              strategies.foley.speedForMaxGain, 0, 3000, 10, "Motion foley", "Loud at",
              "And the speed it reaches full level at, so the cloth tracks how fast the body is "
              "actually moving."),
    RDS_PARAM(AlgorithmConfig, "MotionFoley", "bAirborneRise", kBool, strategies.foley.airborneRise,
              0, 1, 1, "Motion foley", "Airborne rise",
              "The anticipation whoosh while a body is in the air. On by default at a low level: "
              "it tells the ear something is about to land."),
    RDS_PARAM(AlgorithmConfig, "MotionFoley", "fAirborneRiseGainDb", kFloat,
              strategies.foley.airborneRiseGainDb, -80, 6, 0.5, "Motion foley", "Rise level",
              "How loud the whoosh is. It should be felt rather than heard."),
    RDS_PARAM(AlgorithmConfig, "MotionFoley", "bPreImpactDuck", kBool,
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
    RDS_PARAM(AlgorithmConfig, "Mix", "fTransientTrimDb", kFloat, mix.transientTrimDb, -40, 20, 0.5,
              "Mix", "Transient trim",
              "Global trim on the bright contact layer. Up for clicky and present, down for "
              "distant and soft."),
    RDS_PARAM(AlgorithmConfig, "Mix", "fBodyTrimDb", kFloat, mix.bodyTrimDb, -40, 20, 0.5, "Mix",
              "Body trim", "Global trim on the flesh-and-mass layer."),
    RDS_PARAM(AlgorithmConfig, "Mix", "fSubTrimDb", kFloat, mix.subTrimDb, -40, 20, 0.5, "Mix",
              "Sub trim",
              "Global trim on the pitched boom. The one control most worth touching if the mod "
              "feels either weightless or overbearing."),
    RDS_PARAM(AlgorithmConfig, "Mix", "fSurfaceTrimDb", kFloat, mix.surfaceTrimDb, -40, 20, 0.5,
              "Mix", "Surface trim",
              "Global trim on the wood, stone and soft colour. Up makes the floor material "
              "obvious; down makes every surface sound the same, which is what vanilla does."),
    RDS_PARAM(AlgorithmConfig, "Mix", "fGrainTrimDb", kFloat, mix.grainTrimDb, -40, 20, 0.5, "Mix",
              "Grain trim",
              "Global trim on the small filler taps that give a burst its texture."),
    RDS_PARAM(AlgorithmConfig, "Mix", "fLoopTrimDb", kFloat, mix.loopTrimDb, -40, 20, 0.5, "Mix",
              "Loop trim", "Global trim on the scrape and cloth loops."),
    RDS_PARAM(AlgorithmConfig, "Mix", "fVoiceFloorDb", kFloat, mix.voiceFloorDb, -100, 0, 1, "Mix",
              "Voice floor",
              "Below this a sound is not worth a voice and is dropped before the cap ever sees it. "
              "Raise it to keep the voice budget for things that can actually be heard."),

    // -- Player ---------------------------------------------------------------
    RDS_PARAM(AlgorithmConfig, "Player", "bEnabled", kBool, player.enabled, 0, 1, 1, "Player",
              "Enabled", "Whether your own ragdoll makes any sound at all."),
    RDS_PARAM(AlgorithmConfig, "Player", "bAttachToBones", kBool, player.attachToBones, 0, 1, 1,
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
    RDS_PARAM(AlgorithmConfig, "Player", "fMasterGainDb", kFloat, player.masterGainDb, -40, 20, 0.5,
              "Player", "Master gain", "Your own ragdoll's level, separate from everyone else's."),

    // -- Distance -------------------------------------------------------------
    RDS_PARAM(AlgorithmConfig, "Distance", "fFullRadius", kFloat, distance.fullRadius, 0, 5000, 50,
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
    RDS_PARAM(AlgorithmConfig, "Layers", "bSurfWood", kBool, layers.surfWood, 0, 1, 1, "Layers",
              "surf_wood",
              "The hollow knock layered on when a body lands on wood. Mute the three surf_* slots together to hear how much of the mix is surface identity at all."),
    RDS_PARAM(AlgorithmConfig, "Layers", "bSurfStone", kBool, layers.surfStone, 0, 1, 1, "Layers",
              "surf_stone",
              "The hard, short skin for stone. Vanilla plays the same dirt sample for stone, ice and carpet alike, so this is one of the differences the mod exists to make."),
    RDS_PARAM(AlgorithmConfig, "Layers", "bSurfSoft", kBool, layers.surfSoft, 0, 1, 1, "Layers",
              "surf_soft",
              "The dull skin, and the fallback for every surface we cannot resolve - which today means all natural ground, since no dirt, grass or snow contact exists in the capture set."),
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
              "The sliding rumble. Worth muting often: nothing in the capture set is actually a slide, so this is the layer most likely to be firing when it should not."),
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
