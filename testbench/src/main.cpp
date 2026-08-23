// The testbench entry point.
//
//   RagdollSoundsTestbench                       the UI
//   RagdollSoundsTestbench --verify              headless, over every recording
//   RagdollSoundsTestbench --smoke               headless, plays one take for a second
//   RagdollSoundsTestbench --export              headless, dumps every take's state to a file
//                          (--take <match> to dump just one)
//   RagdollSoundsTestbench --import <file>...    headless, brings files into the sfx library
//                          (--no-fmts to keep the site stamp in the name)
//   RagdollSoundsTestbench --take log_7          open that take instead of the first
//   RagdollSoundsTestbench --play                start playing straight away
//   RagdollSoundsTestbench --recordings <dir> --configs <dir> --sounds <dir>
//                          --general <RagdollSounds.ini>
//
// The GUI also listens for a running game on the port `[Devbench] iDevbenchPort`
// names in RagdollSounds.ini, pushes the sliders at it, and can record a take
// out of it with OBS driven alongside. See AppLive.cpp.
//
// --verify is how this program is tested: it runs rds::Verify() over the whole
// research folder and exits non-zero if any of the design's own numbers has
// drifted.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

#include <GL/gl.h>

#include <GLFW/glfw3.h>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include "App.h"
#include "Export.h"
#include "SfxImport.h"

#include "rds/ConfigManager.h"
#include "rds/ConfigSchema.h"
#include "rds/Log.h"
#include "rds/Offline.h"
#include "rds/Recording.h"
#include "rds/Sfx.h"
#include "rds/SlotManifest.h"

namespace fs = std::filesystem;

namespace {

struct Args {
    fs::path recordings{RDS_DEFAULT_RECORDINGS};
    fs::path configs{RDS_DEFAULT_CONFIGS};
    fs::path frameCache{RDS_DEFAULT_FRAMECACHE};
    fs::path sounds;
    fs::path library;
    fs::path sfxIni;
    /// RagdollSounds.ini in the deployed mod. Read for `[Devbench]` - the port
    /// the game listens for us on, and where OBS lives.
    fs::path generalIni;
    fs::path configFile;
    std::string take;  ///< substring of the stem to open first
    bool play{};
    bool verify{};
    bool smoke{};
    bool exportAll{};
    /// Files to bring into the library. The button's headless twin, for the
    /// same reason --export is: importing forty files should not be forty
    /// clicks, and a batch that can only be done by clicking is one nobody
    /// does from a script.
    std::vector<fs::path> importFiles;
    bool fixNames{true};
};

Args ParseArgs(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        const std::string s = argv[i];
        auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : std::string(); };
        if (s == "--verify")
            a.verify = true;
        else if (s == "--smoke")
            a.smoke = true;
        else if (s == "--export")
            a.exportAll = true;
        else if (s == "--recordings")
            a.recordings = next();
        else if (s == "--configs")
            a.configs = next();
        else if (s == "--framecache")
            a.frameCache = next();
        else if (s == "--sounds")
            a.sounds = next();
        else if (s == "--library")
            a.library = next();
        else if (s == "--sfx")
            a.sfxIni = next();
        else if (s == "--general")
            a.generalIni = next();
        else if (s == "--config")
            a.configFile = next();
        else if (s == "--take")
            a.take = next();
        else if (s == "--play")
            a.play = true;
        else if (s == "--import")
            a.importFiles.emplace_back(next());
        else if (s == "--no-fmts")
            a.fixNames = false;
        else if (!s.empty() && s.front() != '-' && !a.importFiles.empty())
            // Bare paths after the first --import, so a shell glob works:
            //   --import sfx/*.wav
            a.importFiles.emplace_back(s);
        else
            std::fprintf(stderr, "unknown argument: %s\n", s.c_str());
    }
    if (a.sounds.empty()) a.sounds = a.configs.parent_path() / "sounds";
    if (a.library.empty()) a.library = RDS_DEFAULT_LIBRARY;
    if (a.sfxIni.empty()) a.sfxIni = RDS_DEFAULT_SFX_INI;
    if (a.generalIni.empty()) a.generalIni = a.sfxIni.parent_path() / "RagdollSounds.ini";
    return a;
}

/// Fill `bank` the way the GUI fills it: from the library and the assignment
/// ini, falling back per slot to the built pack's filenames.
///
/// Shared by every headless mode, because a dump that names different files
/// than the window is playing is worse than no dump. The library and the
/// assignments are out-parameters rather than locals: the bank holds paths into
/// the library and both have to outlive it.
void LoadBank(const Args& args, rds::SoundBank& bank, rds::SfxLibrary& library,
              rds::SfxAssignments& assignments) {
    library.Load(args.library);
    if (assignments.Load(args.sfxIni) == 0) {
        assignments.SeedFromNames(library);
    }
    bank.LoadAssigned(library, assignments, args.sounds.string());
}

/// Every take's CSV, ignoring the _sync.csv sidecars.
std::vector<fs::path> RecordingCsvs(const fs::path& dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    if (!fs::exists(dir, ec)) return out;
    for (const fs::directory_entry& e : fs::directory_iterator(dir, ec)) {
        if (e.path().extension() != ".csv") continue;
        const std::string stem = e.path().stem().string();
        if (stem.size() > 5 && stem.compare(stem.size() - 5, 5, "_sync") == 0) continue;
        out.push_back(e.path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

int RunVerify(const Args& args) {
    rds::AlgorithmConfig cfg{};
    cfg.slots.rngSeed = 1;
    if (!args.configFile.empty()) {
        const std::size_t keys = rds::ConfigManager::LoadInto(args.configFile, &cfg, rds::AlgorithmParams());
        std::printf("config: %s (%zu keys)\n", args.configFile.string().c_str(), keys);
    } else {
        std::printf("config: defaults\n");
    }

    rds::SoundBank bank;
    rds::SfxLibrary library;
    rds::SfxAssignments assignments;
    LoadBank(args, bank, library, assignments);

    const std::vector<fs::path> csvs = RecordingCsvs(args.recordings);
    if (csvs.empty()) {
        std::fprintf(stderr, "no recordings under %s\n", args.recordings.string().c_str());
        return 2;
    }
    std::printf("recordings: %s (%zu takes)\n\n", args.recordings.string().c_str(), csvs.size());

    std::printf("%-46s %7s %6s %6s %7s %8s  %s\n", "take", "contacts", "cues", "bursts", "ratio", "checks",
                "result");
    std::printf("%s\n", std::string(112, '-').c_str());

    int failures = 0;
    std::vector<std::string> details;

    for (const fs::path& csv : csvs) {
        rds::Recording rec;
        std::string error;
        if (!rec.Load(csv, error)) {
            std::printf("%-46s %s\n", csv.stem().string().c_str(), ("LOAD FAILED: " + error).c_str());
            ++failures;
            continue;
        }

        rds::OfflineOptions opt;
        opt.seed = 1;
        opt.trace = false;

        bank.Seed(1);
        rec.Rewind();
        const rds::VerifyReport report = rds::Verify(rec, cfg, bank, opt);

        int passed = 0;
        for (const rds::VerifyExpectation& c : report.checks)
            if (c.passed) ++passed;
        const int total = static_cast<int>(report.checks.size());
        const bool ok = report.Passed();
        if (!ok) ++failures;

        std::printf("%-46s %7u %6u %6u %6.1f:1 %4d/%-3d  %s\n", report.recordingStem.c_str(),
                    report.stats.contactsIn, report.stats.emittedCues, report.stats.bursts,
                    report.stats.ReductionRatio(), passed, total, ok ? "pass" : "FAIL");

        for (const rds::VerifyExpectation& c : report.checks) {
            if (c.passed) continue;
            details.push_back("  " + report.recordingStem + " :: " + c.name + " - " + c.detail);
        }
    }

    if (!details.empty()) {
        std::printf("\nfailures:\n");
        for (const std::string& d : details) std::printf("%s\n", d.c_str());
    }

    std::printf("\n%d of %zu takes failed\n", failures, csvs.size());
    return failures == 0 ? 0 : 1;
}

/// Everything the GUI does to make sound, without a window: load a take, run it
/// through the backend, mix it, open the device, play for a second and report
/// that the cursor moved and the buffer was not silent.
///
/// This exists because "launch it and listen" is not a check anybody else can
/// repeat, and because a silent bug in the mixer looks exactly like a working
/// program from the outside.
int RunSmoke(const Args& args) {
    const std::vector<fs::path> csvs = RecordingCsvs(args.recordings);
    if (csvs.empty()) {
        std::fprintf(stderr, "no recordings under %s\n", args.recordings.string().c_str());
        return 2;
    }

    rds::Recording rec;
    std::string error;
    if (!rec.Load(csvs.front(), error)) {
        std::fprintf(stderr, "load failed: %s\n", error.c_str());
        return 2;
    }
    std::printf("take: %s (%.0f ms, %u impacts)\n", rec.Info().stem.c_str(), rec.Info().durationMs,
                rec.Info().impacts);

    rds::AlgorithmConfig cfg{};
    cfg.slots.rngSeed = 1;
    rds::SoundBank bank;
    rds::SfxLibrary library;
    rds::SfxAssignments assignments;
    LoadBank(args, bank, library, assignments);
    bank.Seed(1);

    rds::OfflineOptions opt;
    opt.seed = 1;
    opt.trace = true;
    const rds::OfflineResult result = rds::RunOffline(rec, cfg, bank, opt);
    std::printf("cues: %zu, trace: %zu, audio %.0f ms\n", result.cues.size(), result.trace.size(),
                result.audioDurationMs);

    tb::SoundSource sources;
    sources.SetBank(&bank, 48000);
    auto audio = std::make_shared<tb::MixedAudio>(
        tb::MixCues(result.cues, result.audioDurationMs, rec.Listener(), sources, 48000, -3.0f, true));
    std::printf("mix: %zu frames, peak %.3f, raw peak %.3f\n", audio->Frames(), audio->peak, audio->rawPeak);
    if (audio->Frames() == 0) {
        std::fprintf(stderr, "mixed buffer is empty\n");
        return 4;
    }

    tb::Player player;
    if (!player.Start(48000)) {
        std::fprintf(stderr, "audio device: %s\n", player.LastError());
        return 5;
    }
    player.SetBuffer(0, audio);
    player.SetLoop(true);
    player.SeekMs(0.0);
    player.Play();

    std::printf("playing for 1200 ms on %d Hz...\n", player.SampleRate());
    float loudest = 0.0f;
    for (int i = 0; i < 12; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        loudest = std::max(loudest, player.Level());
    }
    const double moved = player.PositionMs();
    player.Pause();
    player.Stop();

    std::printf("position advanced to %.0f ms, loudest block %.3f\n", moved, loudest);
    if (moved < 200.0) {
        std::fprintf(stderr, "the transport did not advance\n");
        return 6;
    }
    std::printf("smoke: ok\n");
    return 0;
}

/// The button's headless twin: run every take (or the one `--take` names) and
/// write the same dump the GUI writes.
///
/// Same code path as the button, deliberately - a dump you can only get by
/// clicking is a dump nobody has when they need it.
int RunExport(const Args& args) {
    const auto csvs = RecordingCsvs(args.recordings);
    if (csvs.empty()) {
        std::fprintf(stderr, "no recordings in %s\n", args.recordings.string().c_str());
        return 2;
    }

    rds::AlgorithmConfig cfg{};
    if (!args.configFile.empty()) {
        const auto found =
            rds::ConfigManager::LoadInto(args.configFile, &cfg, rds::AlgorithmParams());
        std::printf("config %s: %zu keys\n", args.configFile.string().c_str(), found);
    }
    cfg.slots.rngSeed = 1;

    rds::SoundBank bank;
    rds::SfxLibrary library;
    rds::SfxAssignments assignments;
    LoadBank(args, bank, library, assignments);

    tb::SoundSource sources;
    sources.SetBank(&bank, 48000);

    // The same file the GUI reads. Without it the two disagreed: the GUI used
    // the measured per-take number and the export printed the raw sync fit, so
    // a dump pasted into a conversation named an offset nobody was listening to.
    tb::OffsetStore offsets;
    offsets.Load(args.frameCache / "video-offsets.ini");

    const auto outDir = args.configs.parent_path() / "exports";
    int written = 0;
    for (const fs::path& csv : csvs) {
        if (!args.take.empty() && csv.stem().string().find(args.take) == std::string::npos) {
            continue;
        }
        rds::Recording rec;
        std::string err;
        if (!rec.Load(csv, err)) {
            std::fprintf(stderr, "%s: %s\n", csv.stem().string().c_str(), err.c_str());
            continue;
        }
        bank.Seed(1);
        rds::OfflineOptions opt;
        opt.seed = 1;
        opt.trace = true;
        const rds::OfflineResult result = rds::RunOffline(rec, cfg, bank, opt);
        const auto audio = std::make_shared<tb::MixedAudio>(tb::MixCues(
            result.cues, result.audioDurationMs, rec.Listener(), sources, 48000, -3.0f, true));

        const tb::SyncModel sync =
            tb::FitSync(csv.parent_path() / (csv.stem().string() + "_sync.csv"));

        tb::ExportRequest req;
        req.recording = &rec;
        req.info = &rec.Info();
        req.config = &cfg;
        req.result = &result;
        req.bank = &bank;
        req.audio = audio.get();
        req.sync = &sync;
        req.configName = args.configFile.empty() ? "(defaults)" : args.configFile.stem().string();
        req.videoOffsetMs =
            offsets.Get(csv.stem().string(), sync.valid ? sync.intercept : rec.Info().videoOffsetMs);
        req.offsetMeasured = offsets.Has(csv.stem().string());
        req.seed = 1;
        req.limiter = true;

        std::string writeErr;
        const auto path = tb::WriteExport(req, outDir, writeErr);
        if (path.empty()) {
            std::fprintf(stderr, "%s: %s\n", csv.stem().string().c_str(), writeErr.c_str());
            continue;
        }
        std::printf("%-46s -> %s\n", csv.stem().string().c_str(),
                    path.filename().string().c_str());
        ++written;
    }
    std::printf("\n%d export%s in %s\n", written, written == 1 ? "" : "s",
                outDir.string().c_str());
    return written > 0 ? 0 : 3;
}

/// The import button's headless twin.
///
/// Same call, same measurements, same sidecars - so what a script brings in is
/// indistinguishable from what the window brings in, and a library half filled
/// each way is still one library.
int RunImport(const Args& args) {
    rds::SfxLibrary library;
    library.Load(args.library);
    const std::size_t before = library.Size();

    tb::ImportOptions options;
    options.fixNames = args.fixNames;
    if (!tb::FfmpegAvailable()) {
        std::printf("no ffmpeg on PATH - only wav sources will import, and none will be "
                    "converted to the pack's 48 kHz mono\n");
    }

    int failed = 0;
    for (const fs::path& file : args.importFiles) {
        const tb::ImportOutcome outcome = tb::ImportSfx(file, library, options);
        if (outcome.file.empty()) {
            std::fprintf(stderr, "%-44s FAILED: %s\n", file.filename().string().c_str(),
                         outcome.error.c_str());
            ++failed;
            continue;
        }
        const rds::SfxEntry* entry = library.Find(outcome.file);
        std::printf("%-44s -> %-36s %6.0f ms%s%s", file.filename().string().c_str(),
                    outcome.file.c_str(), entry ? entry->durationMs : 0.0f,
                    outcome.converted ? "  converted" : "", entry && entry->loops ? "  loop" : "");
        if (entry != nullptr) {
            for (const rds::SlotId slot : entry->suggested) {
                std::printf("  [%s]", std::string(rds::Slot(slot).name).c_str());
            }
            for (const rds::SfxWarning& w : entry->warnings) {
                std::printf("  !%s", w.code.c_str());
            }
        }
        std::printf("\n");
    }

    std::printf("\nlibrary: %zu -> %zu file(s), %d failed\n", before, library.Size(), failed);
    return failed == 0 ? 0 : 3;
}

void GlfwError(int code, const char* description) {
    std::fprintf(stderr, "glfw error %d: %s\n", code, description);
}

int RunGui(const Args& args) {
    glfwSetErrorCallback(&GlfwError);
    if (!glfwInit()) {
        std::fprintf(stderr, "glfwInit failed\n");
        return 3;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_COMPAT_PROFILE);

    GLFWwindow* window = glfwCreateWindow(1720, 980, "Ragdoll Sounds Testbench", nullptr, nullptr);
    if (!window) {
        std::fprintf(stderr, "glfwCreateWindow failed\n");
        glfwTerminate();
        return 3;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGui::GetStyle().WindowRounding = 0.0f;
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 130");

    // Scoped, so the app's GL textures and audio device are released while the
    // context and the window are still alive.
    {
    tb::App app;
    tb::App::Paths paths;
    paths.recordings = args.recordings;
    paths.configs = args.configs;
    paths.frameCache = args.frameCache;
    paths.sounds = args.sounds;
    paths.library = args.library;
    paths.sfxIni = args.sfxIni;
    paths.generalIni = args.generalIni;
    app.Init(paths);
    if (!args.take.empty()) app.SelectRecordingByName(args.take);
    if (args.play) app.Transport().Play();

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        if (glfwGetWindowAttrib(window, GLFW_ICONIFIED)) {
            glfwWaitEventsTimeout(0.1);
            continue;
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        app.Draw();

        ImGui::Render();
        int w = 0, h = 0;
        glfwGetFramebufferSize(window, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.09f, 0.10f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    app.Shutdown();
    app.Transport().Stop();
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const Args args = ParseArgs(argc, argv);

    rds::log::Options logOptions;
    logOptions.directory = fs::current_path();
    logOptions.name = "RagdollSoundsTestbench";
    logOptions.level = rds::LogLevel::kInfo;
    logOptions.rotate = true;
    logOptions.alsoStdout = args.verify;
    rds::log::Setup(logOptions);

    if (args.verify) return RunVerify(args);
    if (args.smoke) return RunSmoke(args);
    if (args.exportAll) return RunExport(args);
    if (!args.importFiles.empty()) return RunImport(args);
    return RunGui(args);
}
