// RagdollSounds.dll - the game half.
//
// Everything interesting is in core/. This file is the wiring: read the config,
// open the log, build the engine, hand it a feed and a sink, and tick it once a
// frame. The three game-facing pieces it wires up are deliberately stubbed until
// the testbench has tuned the algorithm - see GameFeed.h, GameRenderer.h and
// VanillaSuppression.h for what each one will do and why.

#include <spdlog/spdlog.h>

#include <chrono>
#include <format>
#include <memory>
#include <string>

#include "AudioBlobs.h"
#include "Benchmark.h"
#include "DevLink.h"
#include "FrameHook.h"
#include "GameFeed.h"
#include "GameRenderer.h"
#include "HiggsLink.h"
#include "TestCue.h"
#include "VanillaGate.h"
#include "VanillaImpactHook.h"
#include "VanillaSuppression.h"
#include "rds/ConfigManager.h"
#include "rds/Engine.h"
#include "rds/Log.h"
#include "rds/Sfx.h"
#include "rds/SlotManifest.h"

namespace {

/// Where the ini files and the sound bank live, matching the other mods here.
constexpr auto kDataDirectory = "Data/SKSE/Plugins/RagdollSounds";
constexpr auto kSoundDirectory = "Data/SKSE/Plugins/RagdollSounds/sounds";
/// The named library RagdollSounds_SFX.ini assigns from. Beside the pack rather
/// than instead of it: a slot with no assignment still falls back to
/// `sounds\<slot>_<NN>.wav`, so an install that predates the ini is unchanged.
constexpr auto kLibraryDirectory = "Data/SKSE/Plugins/RagdollSounds/sounds/library";

struct Mod {
    rds::SoundBank bank;
    rds::SfxLibrary library;
    rds::game::GameFeed feed;
    rds::game::GameRenderer renderer;
    rds::Engine engine;
    rds::game::DevLink link;
    rds::game::Benchmark bench;
    /// The feed the engine actually reads. Either `feed` itself, or `feed`
    /// wrapped in the tee that copies each drained batch to the testbench - so
    /// with the link off there is not so much as an extra virtual call.
    std::unique_ptr<rds::game::LinkTap> tap;
    rds::IFeed* engineFeed{};
    /// Where the sfx library is read from. Empty means the shipped one; the
    /// testbench pushes its own so a file being auditioned is heard in game
    /// before it has been copied into the pack.
    std::string libraryOverride;
    bool running{};
    bool wasTracking{};
};

Mod& Get() {
    static Mod mod;
    return mod;
}

/// Where the vanilla impact hook puts what it saw.
///
/// A free function rather than a lambda because the hook takes a plain function
/// pointer: it runs on the impact manager's thread and a std::function there
/// would be an indirect call through a heap object nobody owns.
///
/// It goes on the contact ring, so a vanilla row is drained in time order beside
/// the contacts it belongs with and reaches the testbench through the tap that is
/// already there. The engine ignores the kind (Engine.cpp's Ingest), which is
/// what keeps this a recording and not an input.
void PushVanillaSound(const rds::FeedEvent& event) { Get().feed.PushEvent(event); }

/// The engine's clock lives in GameFeed.h - milliseconds since the session
/// opened, monotonic, the same clock a recording's t_ms is. It has to be one
/// definition rather than one per file: a contact's timestamp is stamped in the
/// Havok callback and a cue's is stamped here, and the two are subtracted from
/// each other to place the sub layer.
using rds::game::NowMs;

/// Rebuild the sound bank from whatever the library path and the sfx table
/// currently are. Every path that changes either ends here, which is what makes
/// a change audible on the next contact rather than on the next launch.
void ReloadBank(Mod& mod) {
    const std::string directory =
        mod.libraryOverride.empty() ? std::string{kLibraryDirectory} : mod.libraryOverride;
    mod.library.Load(directory);
    mod.bank.LoadAssigned(mod.library, rds::ConfigManager::Get().Sfx(), kSoundDirectory);
    // Re-set rather than left alone: the renderer's PCM cache holds decoded
    // samples keyed by (slot, variant), and every one of those keys now points
    // at a different file. SetSoundBank is what drops them.
    mod.renderer.SetSoundBank(&mod.bank);
    mod.engine.SetSoundBank(&mod.bank);
}

/// Anything the testbench pushed since the last frame, applied here on the game
/// thread. Nothing in this function may run on the socket thread: reloading the
/// bank races the audio path, and swapping the engine's config mid-tick would
/// tear one.
void ApplyDevbench(Mod& mod) {
    rds::game::DevLink::Pending pending;
    if (!mod.link.Take(pending)) {
        return;
    }
    auto& config = rds::ConfigManager::Get();

    if (pending.clear) {
        config.ClearOverride();
        config.ClearSfxOverride();
        mod.libraryOverride.clear();
        spdlog::info("devbench: overrides cleared, back to the inis");
    }
    if (pending.library) {
        mod.libraryOverride = pending.libraryPath;
        spdlog::info("devbench: sfx library is now {}", mod.libraryOverride);
    }
    if (pending.algorithm) {
        config.PushOverride(pending.config);
        mod.engine.SetConfig(pending.config);
        // The feed's radius is read once rather than per frame, so a pushed
        // config that widens it has to say so - otherwise the engine would be
        // willing to hear an actor the feed had already stopped tracking.
        mod.feed.SetCullRadius(pending.config.distance.simplifiedRadius);
        mod.feed.SetBodySampleEveryNTicks(pending.config.ingest.bodySampleEveryNTicks);
        mod.feed.SetGameIntegration(pending.config.game);
    }
    if (pending.sfx) {
        config.PushSfxOverride(pending.sfxTable);
    }
    if (pending.audioMode) {
        // The A/B switch. Vanilla's body impacts come back and ours stop, or the
        // other way round - both halves together, because either one on its own
        // is a comparison against silence.
        //
        // Going back to ours re-reads the ini rather than assuming suppression:
        // an install that deliberately runs with vanilla underneath ours must be
        // left the way it was, not "fixed" by having used the switch once.
        //
        // Through the hook when it is installed, which is a flag rather than a
        // form edit - so the switch no longer has to put anything back, and the
        // vanilla track is recorded either way. Nulling is still driven the old
        // way on a runtime where the hook could not be placed.
        if (rds::game::VanillaImpactHookInstalled()) {
            rds::game::SetVanillaImpactsSuppressed(!pending.useVanillaAudio);
            mod.renderer.SetMuted(pending.useVanillaAudio);
        } else if (pending.useVanillaAudio) {
            mod.renderer.SetMuted(true);
            rds::game::RestoreVanillaBodyImpacts();
        } else {
            if (config.General().suppression.suppressVanillaBodyImpacts) {
                rds::game::SuppressVanillaBodyImpacts();
            }
            mod.renderer.SetMuted(false);
        }
        spdlog::info("devbench: audio mode is now {}",
                     pending.useVanillaAudio ? "vanilla" : "ours");
    }
    if (pending.sfx || pending.library || pending.clear) {
        ReloadBank(mod);
    }
    if (pending.clear && !pending.algorithm) {
        const auto algorithm = config.Algorithm();
        mod.engine.SetConfig(algorithm);
        mod.feed.SetCullRadius(algorithm.distance.simplifiedRadius);
        mod.feed.SetBodySampleEveryNTicks(algorithm.ingest.bodySampleEveryNTicks);
        mod.feed.SetGameIntegration(algorithm.game);
    }
}

/// The heartbeat, about once a second. Everything the testbench's connection row
/// shows that is not a contact.
void PublishDevbenchStatus(Mod& mod, rds::TimeMs now) {
    static rds::TimeMs lastMs = 0.0;
    if (now - lastMs < 1000.0) {
        return;
    }
    lastMs = now;

    rds::link::StatusPacket status;
    status.sessionMs = now;
    status.trackedActors = static_cast<std::uint32_t>(mod.engine.TrackedActors());
    std::uint64_t cuesIn = 0;
    std::uint64_t voicesOut = 0;
    mod.renderer.Counters(cuesIn, voicesOut);
    status.cuesEmitted = static_cast<std::uint32_t>(cuesIn);
    status.voicesOut = static_cast<std::uint32_t>(voicesOut);
    status.droppedContacts = static_cast<std::uint32_t>(mod.feed.Dropped());
    status.eventsSent = static_cast<std::uint32_t>(mod.engine.Stats().eventsIn);
    status.overrideFlags = mod.link.OverrideFlags();
    mod.link.PushStatus(status);

    // The cell travels with the next profile, so the take the testbench writes
    // can say where it happened. Read here because only the game thread may.
    if (auto* player = RE::PlayerCharacter::GetSingleton()) {
        if (auto* cell = player->GetParentCell()) {
            mod.link.SetCell(cell->GetName());
        }
    }
}

void OnFrame() {
    Mod& mod = Get();
    if (!mod.running) {
        return;
    }
    const rds::TimeMs now = NowMs();

    if (rds::game::TakeTestCueRequest()) {
        rds::game::FireTestCue(mod.renderer, now);
    }

    const float frameSeconds = rds::game::FrameHook::DeltaSeconds();

    // The publisher half of the phase gate: only the game thread may ask an actor
    // whether it is ragdolling, and the contact callback runs on a Havok worker
    // (07 §1). This is where that answer is written down.
    //
    // The three benchmark scopes below bound the mod's whole per-frame cost, in
    // the three pieces it comes in. ApplyDevbench and PublishDevbenchStatus sit
    // deliberately outside them: they are a development link that is off in
    // every shipping install, and timing them would report a cost no player
    // pays.
    {
        rds::game::BenchScope scope{mod.bench, rds::game::Benchmark::Span::kFeed};
        mod.feed.PublishTick(frameSeconds);
    }

    // Before the tick, so a config that arrived this frame is the one this
    // frame's contacts are judged by.
    ApplyDevbench(mod);

    {
        rds::game::BenchScope scope{mod.bench, rds::game::Benchmark::Span::kEngine};
        mod.engine.Tick(*mod.engineFeed, now);
    }

    PublishDevbenchStatus(mod, now);

    // After the engine, because a cue emitted this frame with a zero offset is
    // due this frame. The ears go over first so the renderer's placement line can
    // say how far our audio ended up from them - the feed has just refreshed them
    // from the VR camera root, which is where the game's own listener sits.
    mod.renderer.SetListener(mod.feed.Listener().position);
    {
        rds::game::BenchScope scope{mod.bench, rds::game::Benchmark::Span::kRender};
        mod.renderer.Update(now);
    }

    // One line when the last tracked actor lets go. The engine writes its own
    // per-knockdown summary from Release; this is the half it cannot see - how
    // many engine voices the cues actually cost, and whether the ring had to
    // throw anything away.
    //
    // The two voice numbers are worth having side by side: the engine's cap
    // counts cues, and one composite is now one voice, so the real cost sits well
    // under what the config budgets.
    const std::size_t tracked = mod.engine.TrackedActors();

    // EndFrame before OnTracking, and the order is load-bearing. OnTracking is
    // what opens and closes sampling, so calling it first would fold a frame of
    // zeros into the spans on the frame a knockdown starts, and would close
    // sampling before the last frame of one had been banked.
    // What the frame was carrying, for the benchmark's slow-frame lines. Three
    // getters and a pair of counters, all of them reads the frame has already
    // paid for elsewhere - and `Sampling()` is false in every shipping install,
    // so this is a bool test and a return in the normal case.
    rds::game::FrameLoad load{};
    if (mod.bench.Sampling()) {
        std::uint64_t benchCuesIn = 0;
        std::uint64_t benchVoicesOut = 0;
        mod.renderer.Counters(benchCuesIn, benchVoicesOut);
        load.trackedActors = static_cast<std::uint32_t>(tracked);
        load.liveVoices = static_cast<std::uint32_t>(mod.engine.LiveVoices());
        load.contactsIn = mod.engine.Stats().contactsIn;
        load.cuesOut = static_cast<std::uint32_t>(benchCuesIn);
        load.voicesOut = static_cast<std::uint32_t>(benchVoicesOut);
    }
    mod.bench.EndFrame(now, frameSeconds, mod.engine.Stats(), load);
    mod.bench.OnTracking(tracked != 0, now, mod.engine.Stats());

    if (tracked == 0 && mod.wasTracking) {
        std::uint64_t cuesIn = 0;
        std::uint64_t voicesOut = 0;
        mod.renderer.Counters(cuesIn, voicesOut);
        const std::uint64_t dropped = mod.feed.Dropped();
        const std::size_t liveVoices = mod.engine.LiveVoices();
        spdlog::info("idle: {} cues became {} engine voices; {} still booked{}", cuesIn, voicesOut,
                     liveVoices,
                     dropped != 0 ? std::format("; {} contact(s) were dropped by a full ring - "
                                                "raise kRingCapacity", dropped)
                                  : std::string{});
        if (liveVoices != 0) {
            // Nothing is tracked, so nothing can be playing on our account. A
            // non-zero count here is a voice booked and never given back. It no
            // longer silences anything, now that nothing is capped against it,
            // but it is still a bug and this line is where it shows.
            spdlog::warn("idle: {} voice(s) are still booked with no actor tracked - something was "
                         "taken and not given back", liveVoices);
        }
    }
    mod.wasTracking = tracked != 0;
}

void OnDataLoaded() {
    Mod& mod = Get();
    const auto& general = rds::ConfigManager::Get().General();

    // One trampoline for every hook this DLL writes. Allocated here rather than
    // inside either of them because SKSE::AllocTrampoline *releases* the block it
    // already held: a second caller frees the branch the first one is running
    // through, and the crash lands a frame later somewhere else entirely.
    SKSE::AllocTrampoline(1 << 6);

    // The set of records the body impact sets reach. Wanted whether or not
    // anything is suppressed - it is the hook's filter, and without it every
    // impact in the game would look like ours.
    rds::game::BuildBodyImpactIndex();

    // How wide a downed actor's claim on vanilla's body impacts is. Read here
    // rather than per play: the gate is asked on the impact manager's thread and
    // the config lives behind a mutex, which is the same reason the feed
    // publishes the phase instead of looking it up.
    rds::game::SetVanillaGateRadius(general.suppression.suppressionRadius);

    const bool suppress = general.suppression.suppressVanillaBodyImpacts;
    if (rds::game::InstallVanillaImpactHook(&PushVanillaSound)) {
        // Per call. Nothing is mutated, a set we do not own keeps its sound, and
        // vanilla's own decision is written down on the way past even while it is
        // being dropped.
        rds::game::SetVanillaImpactsSuppressed(suppress);
        if (!suppress) {
            spdlog::info("vanilla body impacts left alone; ours will layer on top of them - the "
                         "hook is still recording what they were");
        }
    } else if (suppress) {
        // The fallback, and it is a worse trade: global, and it silences the
        // records rather than the calls, so there is nothing left to record.
        spdlog::warn("vanilla hook unavailable on this runtime - falling back to nulling the "
                     "impact records, which also means no vanilla track in a take");
        rds::game::SuppressVanillaBodyImpacts();
    } else {
        spdlog::info("vanilla body impacts left alone; ours will layer on top of them");
    }

    // The interface the renderer hands its mixed composites to. Installed before
    // anything can want to play, and after data load because it needs the audio
    // manager to exist.
    if (!rds::game::BlobRegistry::Get().Install()) {
        spdlog::error("the external audio interface could not be installed; this mod cannot play "
                      "anything and the rest of the startup is pointless");
        return;
    }

    // The ini's table when there is one, the filename convention when there is
    // not - decided inside LoadAssigned per slot, so a half-filled ini is a
    // half-reassigned pack rather than a broken one.
    ReloadBank(mod);
    mod.engine.SetSink(&mod.renderer);

    const auto algorithm = rds::ConfigManager::Get().Algorithm();
    mod.engine.SetConfig(algorithm);

    // Anything past the engine's own Simplified radius is culled by the engine,
    // so there is nothing to be gained by still listening to it. Read once:
    // Algorithm() hands back a copy of a large struct and this runs every frame.
    mod.feed.SetCullRadius(algorithm.distance.simplifiedRadius);
    mod.feed.SetBodySampleEveryNTicks(algorithm.ingest.bodySampleEveryNTicks);
    mod.feed.SetGameIntegration(algorithm.game);
    mod.feed.Install();

    // The renderer knows a cue wants a bone; only the feed knows how that actor's
    // limbs were resolved. This is the seam between them.
    mod.renderer.SetBoneResolver([](rds::ActorId actor, std::int32_t limbIndex) {
        return Get().feed.BoneNode(actor, limbIndex);
    });
    mod.renderer.SetRootResolver(
        [](rds::ActorId actor) { return Get().feed.RootNode(actor); });
    mod.renderer.SetActorPositionResolver([](rds::ActorId actor, rds::Vec3& out) {
        return Get().feed.ActorPosition(actor, out);
    });

    rds::game::ResetClock();

    rds::game::InstallTestCue(&mod.renderer, general.audio.testCueKey);

    // Read once, here, and never again: a benchmark that could be switched on
    // mid-session would be one whose first sampled knockdown is not the session's
    // first, which is the one thing it is measuring. See Benchmark.h.
    mod.bench.Configure(general.benchmark);

    // The testbench link, and the tee that feeds it. Both behind bEnableDevbench:
    // with the flag off the engine reads the feed directly and nothing here
    // opens a socket, so a shipping install pays a branch a frame and nothing
    // else. See DevLink.h.
    mod.engineFeed = &mod.feed;
    if (general.devbench.enabled) {
        mod.link.Start(general.devbench);
        mod.tap = std::make_unique<rds::game::LinkTap>(mod.feed, mod.link);
        mod.engineFeed = mod.tap.get();
    }

    // The frame hook, and not SKSE::GetTaskInterface: SKSE's drain loop keeps
    // popping until the queue is empty, so a task added from inside a task runs in
    // the same frame and a tick that queues the next tick would never give the
    // frame back. See FrameHook.h.
    if (!rds::game::FrameHook::Install(&OnFrame)) {
        spdlog::error("the frame hook could not be installed; the engine will never be ticked and "
                      "no sound will play");
        return;
    }

    mod.running = true;
    spdlog::info("running");
}

void MessageHandler(SKSE::MessagingInterface::Message* message) {
    if (message == nullptr) {
        return;
    }
    switch (message->type) {
        case SKSE::MessagingInterface::kPostPostLoad:
            // The one message HIGGS answers its interface query on, and it has to
            // be asked before anything wants to know whether a body is held. A
            // no-op outside VR - see HiggsLink.h.
            rds::game::higgs::Acquire(SKSE::GetMessagingInterface());
            break;
        case SKSE::MessagingInterface::kDataLoaded:
            OnDataLoaded();
            break;
        case SKSE::MessagingInterface::kPreLoadGame:
        case SKSE::MessagingInterface::kNewGame:
            // Drop every tracked actor and every running loop. A scrape that
            // survives a load screen plays in the main menu.
            Get().engine.Reset();
            Get().renderer.StopAll();
            // Detach every listener before the world it points into goes away.
            Get().feed.Clear();
            break;
        default:
            break;
    }
}

}  // namespace

SKSEPluginLoad(const SKSE::LoadInterface* skse) {
    SKSE::Init(skse);

    // Config first, log second. rds::log::Setup keeps the logger it already has,
    // so opening a default one here and reopening it below silently threw away
    // EnableLogRotation and MaxLogFiles - the log always ran on the defaults and
    // the ini appeared to do nothing.
    auto& config = rds::ConfigManager::Get();
    config.Initialize(kDataDirectory);
    config.Load();

    const auto& general = config.General();

    const auto logDirectory = SKSE::log::log_directory();
    rds::log::Setup({.directory = logDirectory ? *logDirectory : std::filesystem::path{"."},
                     .name = "RagdollSounds",
                     .level = general.logLevel,
                     .rotate = general.enableLogRotation,
                     .maxFiles = static_cast<std::size_t>(std::max(0, general.maxLogFiles))});

    if (!general.enabled) {
        // Off means the plugin loads, says so plainly, and hooks nothing - so it
        // can be disabled without uninstalling.
        spdlog::info("Enabled=0 in RagdollSounds.ini: nothing hooked, no sounds will play");
        return true;
    }

    auto* messaging = SKSE::GetMessagingInterface();
    if (messaging == nullptr || !messaging->RegisterListener("SKSE", MessageHandler)) {
        spdlog::error("could not register the SKSE message listener; the mod will not start");
        return false;
    }

    spdlog::info("Physical Ragdoll Sounds loaded");
    return true;
}
