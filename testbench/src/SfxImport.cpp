#include "SfxImport.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <format>
#include <fstream>
#include <numbers>
#include <span>
#include <sstream>

#include <windows.h>

#include <commdlg.h>
// WIN32_LEAN_AND_MEAN keeps shellapi.h out of windows.h, and shlobj.h does not
// bring it back: SHFileOperationW and its struct live there.
#include <shellapi.h>
#include <shlobj.h>

#include <spdlog/spdlog.h>

#include "Mixer.h"
#include "SfxAnalysis.h"
#include "rds/Pcm.h"

namespace fs = std::filesystem;

namespace tb {
namespace {

/// What the pack is, and therefore what an import converts to. Slots.md §5.
constexpr int kTargetRate = 48000;
constexpr int kTargetChannels = 1;
constexpr int kTargetBits = 16;

// ── running a child process ──────────────────────────────────────────────────

/// Run a command line and hand back its stdout.
///
/// CreateProcess rather than std::system for the same reason Video.cpp uses it:
/// cmd.exe's quoting rules mangle paths with spaces, and every path here has
/// spaces in it. The pipe is what Video.cpp does not need and ffprobe does -
/// its answer *is* its output.
int RunCapture(const std::wstring& commandLine, std::string& output) {
    output.clear();

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE readEnd = nullptr;
    HANDLE writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &sa, 0)) {
        return -1;
    }
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writeEnd;
    si.hStdError = writeEnd;
    si.hStdInput = nullptr;

    PROCESS_INFORMATION pi{};
    std::wstring mutableLine = commandLine;
    if (!CreateProcessW(nullptr, mutableLine.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &pi)) {
        CloseHandle(readEnd);
        CloseHandle(writeEnd);
        return -1;
    }
    // The parent's copy has to go before the read, or the read never sees EOF.
    CloseHandle(writeEnd);

    char buffer[4096];
    DWORD got = 0;
    while (ReadFile(readEnd, buffer, sizeof(buffer), &got, nullptr) && got > 0) {
        output.append(buffer, got);
    }
    CloseHandle(readEnd);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return static_cast<int>(code);
}

[[nodiscard]] std::wstring Widen(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int needed = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0);
    std::wstring out(static_cast<std::size_t>(std::max(needed, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), needed);
    return out;
}

[[nodiscard]] std::string Narrow(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int needed = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(std::max(needed, 0)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), needed,
                        nullptr, nullptr);
    return out;
}

// ── finding the tools ────────────────────────────────────────────────────────

[[nodiscard]] std::string Which(const wchar_t* exe) {
    wchar_t found[MAX_PATH]{};
    wchar_t* filePart = nullptr;
    const DWORD length = SearchPathW(nullptr, exe, L".exe", MAX_PATH, found, &filePart);
    if (length == 0 || length >= MAX_PATH) {
        return {};
    }
    return Narrow(std::wstring_view(found, length));
}

struct Tools {
    std::string ffmpeg;
    std::string ffprobe;
};

[[nodiscard]] const Tools& FindTools() {
    // Looked up once. The answer cannot change while the program runs, and this
    // is asked on every frame the import button is drawn.
    static const Tools tools = [] {
        Tools t{Which(L"ffmpeg"), Which(L"ffprobe")};
        if (t.ffmpeg.empty() || t.ffprobe.empty()) {
            spdlog::warn("sfx: no ffmpeg/ffprobe on PATH - imports can only take wav files, and "
                         "nothing will be converted to the pack's 48 kHz mono");
        } else {
            spdlog::info("sfx: ffmpeg at {}", t.ffmpeg);
        }
        return t;
    }();
    return tools;
}

// ── probing the source ───────────────────────────────────────────────────────

struct SourceInfo {
    bool ok{};
    int sampleRate{};
    int channels{};
    int bitsPerSample{};
    long long bitrate{};
    std::string codec;
    std::string container;
    double durationMs{};

    /// True for anything that has been through a lossy encoder. The distinction
    /// matters because a 96 kbps mp3 of a good recording is a bad sfx and there
    /// is nothing an import can do about it.
    [[nodiscard]] bool Lossy() const {
        return codec == "mp3" || codec == "aac" || codec == "vorbis" || codec == "opus" ||
               codec == "wmav2" || codec == "wmav1";
    }
};

/// ffprobe's `key=value` output for the first audio stream, plus the format's
/// own bit rate when the stream does not carry one.
[[nodiscard]] SourceInfo ProbeSource(const fs::path& path) {
    SourceInfo info;
    const Tools& tools = FindTools();
    if (tools.ffprobe.empty()) {
        return info;
    }

    std::wostringstream cmd;
    cmd << L'"' << Widen(tools.ffprobe) << L'"'
        << L" -v error -select_streams a:0 -show_entries"
           L" stream=sample_rate,channels,bits_per_raw_sample,bits_per_sample,codec_name,bit_rate"
           L":format=duration,bit_rate,format_name"
           L" -of default=noprint_wrappers=1 \""
        << path.wstring() << L'"';

    std::string out;
    if (RunCapture(cmd.str(), out) != 0) {
        return info;
    }

    std::istringstream lines(out);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string key = line.substr(0, eq);
        const std::string value = line.substr(eq + 1);
        if (value.empty() || value == "N/A") {
            continue;
        }
        if (key == "sample_rate") info.sampleRate = std::atoi(value.c_str());
        else if (key == "channels") info.channels = std::atoi(value.c_str());
        else if (key == "codec_name") info.codec = value;
        else if (key == "format_name") info.container = value;
        else if (key == "duration") info.durationMs = std::atof(value.c_str()) * 1000.0;
        // Both the stream's and the format's land on the same field; whichever
        // arrives last wins, and either is the number worth judging.
        else if (key == "bit_rate") info.bitrate = std::atoll(value.c_str());
        else if (key == "bits_per_raw_sample" || key == "bits_per_sample") {
            const int bits = std::atoi(value.c_str());
            if (bits > 0) {
                info.bitsPerSample = bits;
            }
        }
    }
    info.ok = info.sampleRate > 0 && info.channels > 0;
    return info;
}

// ── filenames ────────────────────────────────────────────────────────────────

/// Anything a Windows filename cannot carry, plus the ones that are legal and
/// still a bad idea in a comma-separated ini value.
[[nodiscard]] std::string SanitiseFilename(std::string_view name) {
    std::string out;
    out.reserve(name.size());
    for (const char c : name) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20 || c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' ||
            c == '|' || c == '?' || c == '*' || c == ',' || c == ';') {
            out += '-';
        } else {
            out += c;
        }
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '.' || out.back() == '-')) {
        out.pop_back();
    }
    return out.empty() ? std::string("sfx") : out;
}

/// `<stem>.wav`, or `<stem>_2.wav` when that is taken.
///
/// Never overwrites. The library is addressed by filename, so writing over an
/// existing file would silently change what every slot naming it plays.
[[nodiscard]] std::string UniqueFilename(const fs::path& directory, std::string_view stem) {
    std::error_code ec;
    std::string candidate = std::format("{}.wav", stem);
    for (int n = 2; fs::exists(directory / candidate, ec) && n < 1000; ++n) {
        candidate = std::format("{}_{}.wav", stem, n);
    }
    return candidate;
}

// ── conversion ───────────────────────────────────────────────────────────────

/// True when the source is already exactly what the pack wants and can be
/// copied rather than re-encoded. A copy is not an optimisation here: every
/// re-encode of an already-correct file is a resample nobody asked for.
[[nodiscard]] bool AlreadyCorrect(const SourceInfo& info, const fs::path& source) {
    auto extension = source.extension().string();
    std::ranges::transform(extension, extension.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension == ".wav" && info.ok && info.sampleRate == kTargetRate &&
           info.channels == kTargetChannels && info.codec == "pcm_s16le";
}

/// Convert to mono / 48 kHz / 16-bit PCM.
///
/// `channels` is 2 for the correlation probe, which wants the same conversion
/// without the fold. `filter` is the one case where the fold itself is wrong:
/// `pan=mono|c0=c0` keeps the left channel instead of summing two that would
/// comb-filter each other.
[[nodiscard]] bool Convert(const fs::path& source, const fs::path& destination, int channels,
                           std::string& error, std::string_view filter = {}) {
    const Tools& tools = FindTools();
    if (tools.ffmpeg.empty()) {
        error = "ffmpeg is not on PATH, so " + source.extension().string() +
                " cannot be converted. wav sources still import.";
        return false;
    }
    std::wostringstream cmd;
    cmd << L'"' << Widen(tools.ffmpeg) << L'"' << L" -y -v error -i \"" << source.wstring() << L'"';
    if (!filter.empty()) {
        cmd << L" -af \"" << Widen(filter) << L'"';
    }
    cmd << L" -ac " << channels << L" -ar " << kTargetRate << L" -c:a pcm_s16le -map_metadata -1 \""
        << destination.wstring() << L'"';

    std::string out;
    const int rc = RunCapture(cmd.str(), out);
    if (rc != 0) {
        error = std::format("ffmpeg failed (rc {}){}{}", rc, out.empty() ? "" : ": ", out);
        return false;
    }
    std::error_code ec;
    if (!fs::exists(destination, ec) || fs::file_size(destination, ec) == 0) {
        error = "ffmpeg wrote nothing";
        return false;
    }
    return true;
}

/// How alike the two channels of a stereo source are.
///
/// 03-Asset-Status.md §3 has this as the most common failure of all: below 0.6
/// the channels comb-filter when summed, and summing is exactly what the mono
/// conversion does. Measured on a two-channel copy of the converted file and
/// then thrown away, because the library file itself is mono by then and the
/// question can no longer be asked of it.
[[nodiscard]] bool MeasureCorrelation(const fs::path& source, float& correlation) {
    std::error_code ec;
    const fs::path temp = fs::temp_directory_path(ec) / "rds_sfx_corr.wav";
    std::string error;
    if (!Convert(source, temp, 2, error)) {
        return false;
    }
    std::vector<float> interleaved;
    int rate = 0;
    const bool decoded = DecodeInterleaved(temp.string(), 2, interleaved, rate);
    fs::remove(temp, ec);
    if (!decoded || interleaved.size() < 4) {
        return false;
    }

    double sumLL = 0.0;
    double sumRR = 0.0;
    double sumLR = 0.0;
    for (std::size_t i = 0; i + 1 < interleaved.size(); i += 2) {
        const double l = interleaved[i];
        const double r = interleaved[i + 1];
        sumLL += l * l;
        sumRR += r * r;
        sumLR += l * r;
    }
    const double denominator = std::sqrt(sumLL * sumRR);
    if (denominator < 1e-12) {
        return false;
    }
    correlation = static_cast<float>(sumLR / denominator);
    return true;
}

// ── repair ───────────────────────────────────────────────────────────────────

/// Write mono float samples as the pack's 16-bit PCM wav.
///
/// Through rds::EncodeWavPcm16 rather than ffmpeg, because the samples are
/// already decoded and in hand: bouncing them out to a temp file so ffmpeg can
/// apply a `volume` filter and re-quantise them a second time is two conversions
/// to do arithmetic we have already done.
[[nodiscard]] bool WriteMonoWav(const fs::path& path, std::span<const float> mono, int sampleRate) {
    const std::vector<std::uint8_t> bytes = rds::EncodeWavPcm16(mono, sampleRate);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    return out.good();
}

/// The delivery rules that are mechanical (Slots.md §5), applied to the samples.
///
/// Returns what it changed, empty when there was nothing to change - which is
/// the normal answer for a file that came out of `sfx.py make`, and is what
/// makes running this over a whole library safe.
///
/// `measured` is the file as it stands. Order is not free: trimming does not
/// move the peak, subtracting DC does, and the fade has to happen before the
/// normalise or the last 6 ms would be scaled by a gain worked out from a peak
/// the fade then removed. So it goes trim, DC, fade, normalise, and the peak is
/// exact afterwards.
///
/// Loops are trimmed and faded by nothing at all. A loop has no head silence by
/// definition - it measured as a texture - and a fade at the end of one is not a
/// fix, it is a hole that arrives once a second. Their seam is `sfx.py make`'s
/// job and it stays a warning.
[[nodiscard]] std::vector<std::string> RepairSamples(std::vector<float>& mono, int sampleRate,
                                                     const rds::SfxEntry& measured) {
    std::vector<std::string> did;
    if (mono.empty() || sampleRate <= 0) {
        return did;
    }
    const auto perMs = static_cast<double>(sampleRate) / 1000.0;

    if (!measured.loops) {
        // Head silence. The analysis measured it against an absolute -60 dBFS
        // floor in 2 ms hops, so the count is hops of silence and cutting all of
        // them lands on the first hop that had something in it; a millisecond
        // stays behind so a hop boundary cannot eat the front of the attack.
        if (measured.leadInMs > sfxrepair::kLeadInMs) {
            const auto cut = static_cast<std::size_t>((measured.leadInMs - 1.0f) * perMs);
            if (cut > 0 && cut < mono.size()) {
                mono.erase(mono.begin(), mono.begin() + static_cast<std::ptrdiff_t>(cut));
                did.push_back(std::format("trimmed {:.0f} ms of head silence", measured.leadInMs));
            }
        }

        // Digital silence at the end. Not the room tail - where a decay stops
        // being the sound is a judgement, and it stays a warning - only the
        // nothing that a bad export leaves after it.
        constexpr float kFloor = 0.001f;  // -60 dBFS, the same floor as the lead-in
        std::size_t last = mono.size();
        while (last > 0 && std::fabs(mono[last - 1]) < kFloor) {
            --last;
        }
        const auto guard = static_cast<std::size_t>(sfxrepair::kTailGuardMs * perMs);
        const std::size_t keep = std::min(mono.size(), last + guard);
        const auto trailingMs = static_cast<float>((mono.size() - keep) / perMs);
        if (last > 0 && trailingMs > sfxrepair::kTrailingMs) {
            mono.resize(keep);
            did.push_back(std::format("trimmed {:.0f} ms of trailing silence", trailingMs));
        }
    }

    // DC, measured here rather than taken from `measured`: that number is the
    // mean of the file as it arrived, and what is left after the trim is a
    // different file. A take with 300 ms of digital silence in front of 200 ms
    // of offset tone averages its offset down by the ratio of the two, so
    // subtracting the whole file's mean from the trimmed part leaves most of the
    // offset in place - and the normalise below then multiplies what is left by
    // however much gain the file needed.
    {
        double sum = 0.0;
        for (const float s : mono) {
            sum += s;
        }
        const auto mean = static_cast<float>(sum / static_cast<double>(mono.size()));
        if (std::fabs(mean) > sfxrepair::kDcOffset) {
            for (float& s : mono) {
                s -= mean;
            }
            did.push_back(std::format("subtracted {:+.3f} DC", mean));
        }
    }

    // The end fade, which is `sfx.py make`'s: 6 ms of cos^2. Only for a one-shot
    // that stops while it is still loud - a sound that has already decayed into
    // the floor has nothing to click.
    if (!measured.loops && mono.size() > static_cast<std::size_t>(sfxrepair::kEndFadeMs * perMs)) {
        float peak = 0.0f;
        for (const float s : mono) {
            peak = std::max(peak, std::fabs(s));
        }
        // The question is "does this file stop mid-waveform", and only the very
        // last samples answer it. Level does not: `imp_sub` is specified to end
        // on a quiet tail holding its settled pitch, so 0.5 ms from the end it
        // is still well above any level threshold worth setting - and fading it
        // again over the fade it already has is a change to a file that verify
        // passes. The final samples of a cosine fade are zero, and the final
        // sample of a hard cut is wherever the waveform happened to be, so a
        // hundredth of the file's own peak separates the two by orders.
        const auto window = std::min<std::size_t>(mono.size(), 5);
        float last = 0.0f;
        for (std::size_t i = mono.size() - window; i < mono.size(); ++i) {
            last = std::max(last, std::fabs(mono[i]));
        }
        if (last > peak * sfxrepair::kEndFadeRatio) {
            const auto fade = static_cast<std::size_t>(sfxrepair::kEndFadeMs * perMs);
            for (std::size_t i = 0; i < fade; ++i) {
                const auto t = static_cast<float>(i) / static_cast<float>(fade) *
                               std::numbers::pi_v<float> * 0.5f;
                const float g = std::cos(t) * std::cos(t);
                mono[mono.size() - fade + i] *= g;
            }
            did.push_back(std::format("{:.0f} ms end fade", sfxrepair::kEndFadeMs));
        }
    }

    // Level, last, so it is exact. The band in the middle is left alone on
    // purpose: it holds both of Slots.md §3's targets, so adopting the built
    // pack does not pull `imp_sub` off its own -1.0 on the way in.
    float peak = 0.0f;
    for (const float s : mono) {
        peak = std::max(peak, std::fabs(s));
    }
    const float peakDb = 20.0f * std::log10(std::max(peak, 1e-9f));
    if (peakDb > sfxrepair::kPeakFloorDb &&
        (peakDb > sfxrepair::kPeakHotDb || peakDb < sfxrepair::kPeakQuietDb)) {
        const float gain = std::pow(10.0f, (sfxrepair::kPeakTargetDb - peakDb) / 20.0f);
        for (float& s : mono) {
            s *= gain;
        }
        did.push_back(std::format("normalised {:.1f} -> {:.1f} dBFS", peakDb,
                                  sfxrepair::kPeakTargetDb));
    }
    return did;
}

/// Warnings JudgeSfx cannot recompute, because they are about where the file
/// came from rather than about what it holds.
///
/// A re-measure replaces the warning list wholesale, which is right for
/// everything measured and wrong for these: the mp3 an entry was decoded from is
/// gone, and nothing in the samples says it was 96 kbps. Without this, pressing
/// `repair` on a lossy import quietly clears the badge that says it is lossy.
void CarryOverSourceWarnings(const rds::SfxEntry& from, rds::SfxEntry& to) {
    static constexpr std::string_view kSourceCodes[] = {"lossy", "low bitrate", "low rate",
                                                        "downmixed", "uncorrelated"};
    for (const rds::SfxWarning& w : from.warnings) {
        if (std::ranges::find(kSourceCodes, w.code) != std::ranges::end(kSourceCodes)) {
            to.warnings.push_back(w);
        }
    }
}

/// The same sound already in the library under another name.
///
/// 03-Asset-Status.md §7 hit this twice in one batch of 102 - a copy of the
/// damp-soil take filed as a limb tap, and a duplicated download. Two files that
/// hash the same are one sound taking two slots in a shuffle bag, which is a
/// variant that is not a variant.
void FlagDuplicate(const rds::SfxLibrary& library, rds::SfxEntry& entry) {
    if (entry.contentHash == 0) {
        return;
    }
    for (const rds::SfxEntry& other : library.Entries()) {
        if (other.file == entry.file || other.contentHash != entry.contentHash) {
            continue;
        }
        entry.warnings.push_back(
            {"duplicate",
             std::format("the same samples as `{}`, which is already in the library. Two names for "
                         "one sound, and a slot that plays both draws the same file twice as often "
                         "as it looks like it does - a variant that is not a variant.\n\nDelete one "
                         "of them. Nothing is lost: they are identical.",
                         other.name),
             false, true});
        return;
    }
}

/// Everything the *source* was wrong about, as opposed to what the file now
/// measures. These survive the conversion because the conversion cannot undo
/// them: upsampling a 22 kHz recording does not put the top octave back.
void JudgeSource(const SourceInfo& info, rds::SfxEntry& entry) {
    const auto warn = [&entry](std::string code, std::string detail) {
        entry.warnings.push_back({std::move(code), std::move(detail), false});
    };

    if (!info.ok) {
        return;
    }
    if (info.sampleRate < 32000) {
        warn("low rate", std::format("the source was {} Hz. It is at 48 kHz now, but resampling "
                                     "does not put back a top octave that was never recorded - "
                                     "expect it to sound dull against the rest of the pack.\n\n"
                                     "Nothing repairs this. It is fine on `imp_body` or `imp_sub`, "
                                     "which live below 4 kHz anyway, and wrong on `imp_transient` "
                                     "or `surf_stone`, where the top end is the character.",
                                     info.sampleRate));
    }
    if (info.Lossy()) {
        const long long kbps = info.bitrate / 1000;
        if (kbps > 0 && kbps < 96) {
            warn("low bitrate",
                 std::format("{} at {} kbps. Lossy encoders spend their bits on tone and throw away "
                             "exactly the transient detail an impact is made of - and an impact is "
                             "nothing but its first 15 ms.\n\nNothing repairs it either: the "
                             "detail is not in the file. Re-source it if you can, and A/B it "
                             "against the pack before assigning it to a transient layer.",
                             info.codec, kbps));
        } else {
            warn("lossy", std::format("the source was {}. Fine for a bed, worth re-sourcing for a "
                                      "transient layer - the encoder throws away the first "
                                      "milliseconds of an attack before anything else.",
                                      info.codec));
        }
    }
    if (info.channels > 2) {
        warn("downmixed", std::format("{} channels folded to mono. Check it still sounds like what "
                                      "you picked: a surround bed summed to one channel loses "
                                      "whatever the rears were carrying.",
                                      info.channels));
    }
}

}  // namespace

bool FfmpegAvailable() {
    const Tools& tools = FindTools();
    return !tools.ffmpeg.empty() && !tools.ffprobe.empty();
}

const std::string& FfmpegPath() { return FindTools().ffmpeg; }
const std::string& FfprobePath() { return FindTools().ffprobe; }

// ═════════════════════════════════════════════════════════════════════════════
// import
// ═════════════════════════════════════════════════════════════════════════════

ImportOutcome ImportSfx(const fs::path& source, rds::SfxLibrary& library,
                        const ImportOptions& options) {
    ImportOutcome outcome;
    outcome.source = source;

    std::error_code ec;
    if (!fs::exists(source, ec)) {
        outcome.error = "no such file";
        return outcome;
    }

    const SourceInfo info = ProbeSource(source);

    const std::string rawStem = source.stem().string();
    const std::string stem = options.fixNames ? TidyName(rawStem) : rawStem;
    outcome.renamed = stem != rawStem;

    const fs::path directory = library.Directory();
    fs::create_directories(directory, ec);
    const std::string filename = UniqueFilename(directory, SanitiseFilename(stem));
    const fs::path destination = directory / filename;

    // Correlation is asked of the source, before the mono fold that would make
    // the question unanswerable.
    float correlation = 1.0f;
    const bool haveCorrelation = info.channels == 2 && MeasureCorrelation(source, correlation);

    // 03-Asset-Status.md §3.1, the most common technical failure in the whole
    // batch: two channels that do not correlate comb-filter each other when
    // summed, and summing is what the mono fold does. `make`'s fix is to take
    // the left channel instead of the sum, so that is what happens here - the
    // sound the file was picked for arrives intact, and nothing is said about
    // it because nothing needs deciding.
    const bool takeLeft = haveCorrelation && correlation < sfxrepair::kCorrelationFold;
    if (takeLeft) {
        outcome.repairs = std::format("left channel only (corr {:.2f})", correlation);
    }

    if (AlreadyCorrect(info, source) && !takeLeft) {
        fs::copy_file(source, destination, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            outcome.error = "could not copy: " + ec.message();
            return outcome;
        }
    } else {
        std::string error;
        if (!Convert(source, destination, kTargetChannels, error,
                     takeLeft ? "pan=mono|c0=c0" : std::string_view{})) {
            // No ffmpeg and a wav source is still worth a try: miniaudio decodes
            // wav on its own, so the file can be copied as it is and measured.
            // It keeps its own rate and channel count, and JudgeSfx says so.
            auto extension = source.extension().string();
            std::ranges::transform(extension, extension.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            if (extension != ".wav") {
                outcome.error = error;
                return outcome;
            }
            fs::copy_file(source, destination, fs::copy_options::overwrite_existing, ec);
            if (ec) {
                outcome.error = "could not copy: " + ec.message();
                return outcome;
            }
        } else {
            outcome.converted = true;
        }
    }

    rds::SfxEntry entry;
    entry.file = filename;
    entry.name = fs::path(filename).stem().string();
    // Here rather than after the decode, so the one file that cannot be decoded
    // still carries the evening it arrived on - it is the sound most likely to
    // be looked for by when it came in.
    entry.importedAt = static_cast<std::int64_t>(std::time(nullptr));

    std::vector<float> mono;
    int rate = 0;
    if (!DecodeMonoFile(destination.string(), mono, rate) || mono.empty()) {
        // The one blocking case. The file is left in the library so it can be
        // seen and deleted rather than vanishing with an error message.
        entry.warnings.push_back(
            {"unreadable", "nothing here could decode this file. It is in the library so you can "
                           "see it, but assigning it to a slot will play silence.",
             true});
        entry.durationMs = static_cast<float>(info.durationMs);
        library.Upsert(entry);
        outcome.file = filename;
        spdlog::warn("sfx: imported {} but could not decode it", filename);
        return outcome;
    }

    MeasureSfx(mono, rate, entry);
    // The container's own answer, which is what a warning about format should
    // quote - the decode has already normalised whatever it found.
    if (const rds::WavInfo wav = rds::ProbeWav(destination.string()); wav.Valid()) {
        entry.sampleRate = wav.sampleRate;
        entry.channels = wav.channels;
        entry.bitsPerSample = wav.bitsPerSample;
    }

    // ── the repair pass ──────────────────────────────────────────────────────
    //
    // Only on a file that reached the pack's format, which is every import that
    // had ffmpeg. Without it a wav is copied at whatever rate it was, and
    // rewriting the samples then would fold a stereo file to mono while leaving
    // its rate wrong - clearing the `stereo` badge and keeping the `rate` one,
    // for a file that is no more correct than it was.
    const bool atTargetFormat = entry.sampleRate == kTargetRate &&
                                entry.channels == kTargetChannels &&
                                entry.bitsPerSample == kTargetBits;
    if (atTargetFormat) {
        const std::vector<std::string> fixes = RepairSamples(mono, rate, entry);
        if (!fixes.empty() && WriteMonoWav(destination, mono, rate)) {
            for (const std::string& fix : fixes) {
                outcome.repairs += outcome.repairs.empty() ? fix : ", " + fix;
            }
            // Everything measured again from the samples that were written, so
            // the sidecar describes the file that is on disk rather than the one
            // that arrived.
            MeasureSfx(mono, rate, entry);
        }
    }

    JudgeSfx(entry);
    JudgeSource(info, entry);
    if (haveCorrelation && correlation < sfxrepair::kCorrelationDead) {
        // Above this the left channel is the fix and it has already been taken.
        // Below it the two channels are not one recording in stereo, they are
        // two recordings, and there is no answer to which of them is the sound.
        entry.warnings.push_back(
            {"uncorrelated",
             std::format("the source's two channels correlate at only {:.2f}. Between 0.45 and 0.6 "
                         "the import keeps the left channel rather than summing them, which is "
                         "`sfx.py make`'s own fix - under 0.45 there is nothing to keep: these are "
                         "two different recordings sharing a file, and the left one alone is not "
                         "what you auditioned.\n\n03-Asset-Status.md §3.1 calls this dead below "
                         "0.45. Re-source it.",
                         correlation),
             false, true});
    }
    FlagDuplicate(library, entry);
    entry.suggested = SuggestSlots(entry);

    library.Upsert(entry);
    outcome.file = filename;

    spdlog::info("sfx: imported {} -> {} ({:.0f} ms, {}{}{} warning(s)){}{}",
                 source.filename().string(), filename, entry.durationMs,
                 entry.loops ? "loops, " : "", outcome.converted ? "converted, " : "",
                 entry.warnings.size(), outcome.repairs.empty() ? "" : "; repaired: ",
                 outcome.repairs);
    return outcome;
}

bool MeasureExisting(rds::SfxLibrary& library, const std::string& file, std::string& error) {
    rds::SfxEntry* existing = library.Find(file);
    if (existing == nullptr) {
        error = "not in the library";
        return false;
    }
    const fs::path path = library.PathOf(file);

    std::vector<float> mono;
    int rate = 0;
    if (!DecodeMonoFile(path.string(), mono, rate) || mono.empty()) {
        error = "could not decode it";
        return false;
    }

    // The name and the mute are the user's, and the import date is when the file
    // arrived rather than anything about its samples; all three survive a
    // re-measure and everything else is replaced.
    rds::SfxEntry entry;
    entry.file = existing->file;
    entry.name = existing->name;
    entry.importedAt = existing->importedAt;
    entry.disabled = existing->disabled;

    MeasureSfx(mono, rate, entry);
    if (const rds::WavInfo wav = rds::ProbeWav(path.string()); wav.Valid()) {
        entry.sampleRate = wav.sampleRate;
        entry.channels = wav.channels;
        entry.bitsPerSample = wav.bitsPerSample;
    }
    JudgeSfx(entry);
    CarryOverSourceWarnings(*existing, entry);
    FlagDuplicate(library, entry);
    entry.suggested = SuggestSlots(entry);

    library.Upsert(entry);
    return true;
}

bool NeedsRepair(const rds::SfxEntry& entry) {
    // The codes RepairSamples and the container conversion between them clear.
    // Every other warning is either a decision (`satellite`, `tail`, `seam`) or
    // a dead end (`clipped`, `noise floor`), and offering a button that would
    // rewrite the file and change nothing about the badge is worse than not
    // offering one.
    static constexpr std::string_view kRepairable[] = {"rate", "stereo",  "bits", "hot",
                                                       "quiet", "dc", "lead-in"};
    return std::ranges::any_of(entry.warnings, [](const rds::SfxWarning& w) {
        return std::ranges::find(kRepairable, w.code) != std::ranges::end(kRepairable);
    });
}

bool RepairExisting(rds::SfxLibrary& library, const std::string& file, std::string& error,
                    std::string* repairs) {
    const rds::SfxEntry* found = library.Find(file);
    if (found == nullptr) {
        error = "not in the library";
        return false;
    }
    const rds::SfxEntry before = *found;
    const fs::path path = library.PathOf(file);
    std::vector<std::string> did;

    // ── the container ────────────────────────────────────────────────────────
    //
    // wav only. A library entry that is still an .mp3 was dropped into the
    // folder by hand, and converting it in place would leave wav samples under
    // an `.mp3` name - which the ini names, so the name cannot change either.
    // Importing it again is the answer, and that path already does all of this.
    std::error_code ec;
    auto extension = path.extension().string();
    std::ranges::transform(extension, extension.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    const rds::WavInfo wav = rds::ProbeWav(path.string());
    const bool wrongContainer = !wav.Valid() || wav.sampleRate != kTargetRate ||
                                wav.channels != kTargetChannels ||
                                wav.bitsPerSample != kTargetBits || wav.floatFormat;
    if (wrongContainer) {
        if (extension != ".wav") {
            error = "it is not a wav - import it again instead, so it can be converted under a "
                    "name that matches what is in it";
            return false;
        }
        if (!FfmpegAvailable()) {
            error = "ffmpeg is not on PATH, so the format cannot be converted";
            return false;
        }
        const fs::path temp = fs::path(path).replace_extension(".repair.wav");
        if (!Convert(path, temp, kTargetChannels, error)) {
            fs::remove(temp, ec);
            return false;
        }
        fs::rename(temp, path, ec);
        if (ec) {
            fs::remove(temp, ec);
            error = "could not replace the file: " + ec.message();
            return false;
        }
        did.emplace_back("converted to 48 kHz mono 16-bit");
    }

    std::vector<float> mono;
    int rate = 0;
    if (!DecodeMonoFile(path.string(), mono, rate) || mono.empty()) {
        error = "could not decode it";
        return false;
    }

    // The name and the mute are the user's and the import date is when the file
    // arrived; all three survive, exactly as they do a re-measure.
    rds::SfxEntry entry;
    entry.file = before.file;
    entry.name = before.name;
    entry.importedAt = before.importedAt;
    entry.disabled = before.disabled;

    MeasureSfx(mono, rate, entry);
    const std::vector<std::string> fixes = RepairSamples(mono, rate, entry);
    if (!fixes.empty()) {
        if (!WriteMonoWav(path, mono, rate)) {
            error = "could not write it";
            return false;
        }
        did.insert(did.end(), fixes.begin(), fixes.end());
        MeasureSfx(mono, rate, entry);
    }

    if (const rds::WavInfo after = rds::ProbeWav(path.string()); after.Valid()) {
        entry.sampleRate = after.sampleRate;
        entry.channels = after.channels;
        entry.bitsPerSample = after.bitsPerSample;
    }
    JudgeSfx(entry);
    CarryOverSourceWarnings(before, entry);
    FlagDuplicate(library, entry);
    entry.suggested = SuggestSlots(entry);
    library.Upsert(entry);

    std::string note;
    for (const std::string& one : did) {
        note += note.empty() ? one : ", " + one;
    }
    if (repairs != nullptr) {
        *repairs = note;
    }
    spdlog::info("sfx: repaired {}: {}", file, note.empty() ? "nothing to do" : note);
    return true;
}

// ═════════════════════════════════════════════════════════════════════════════
// the open dialog
// ═════════════════════════════════════════════════════════════════════════════

std::vector<fs::path> PickAudioFiles() {
    // Room for a long multi-select: OFN_ALLOWMULTISELECT writes the directory
    // and then every filename into this one buffer, and a truncated list is a
    // silent partial import.
    std::vector<wchar_t> buffer(64 * 1024, L'\0');

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = L"Audio files\0*.wav;*.mp3;*.ogg;*.flac;*.m4a;*.aac;*.wma;*.aif;*.aiff;*.opus\0"
                      L"All files\0*.*\0\0";
    ofn.lpstrFile = buffer.data();
    ofn.nMaxFile = static_cast<DWORD>(buffer.size());
    ofn.lpstrTitle = L"Import sfx into the library";
    ofn.Flags = OFN_EXPLORER | OFN_ALLOWMULTISELECT | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST |
                OFN_NOCHANGEDIR;

    if (!GetOpenFileNameW(&ofn)) {
        return {};
    }

    // One selection is `C:\dir\file.wav`; several are the directory, a NUL, and
    // then each filename - which is why this cannot just read one string.
    std::vector<std::wstring> parts;
    const wchar_t* cursor = buffer.data();
    while (*cursor != L'\0') {
        parts.emplace_back(cursor);
        cursor += parts.back().size() + 1;
    }
    if (parts.empty()) {
        return {};
    }

    std::vector<fs::path> files;
    if (parts.size() == 1) {
        files.emplace_back(parts[0]);
    } else {
        const fs::path directory(parts[0]);
        for (std::size_t i = 1; i < parts.size(); ++i) {
            files.push_back(directory / parts[i]);
        }
    }
    return files;
}

bool RecycleFiles(const std::vector<fs::path>& files, std::string& error) {
    error.clear();

    // pFrom is a run of NUL-terminated paths ending in a second NUL, so the
    // buffer is built by hand rather than by handing over a std::wstring - a
    // wstring's own terminator is the list's terminator and one file would look
    // like an empty list.
    std::vector<wchar_t> from;
    std::size_t queued = 0;
    for (const fs::path& file : files) {
        std::error_code ec;
        if (!fs::exists(file, ec)) {
            continue;
        }
        // Absolute, because the shell resolves a relative path against its own
        // idea of the current directory and not against ours.
        const std::wstring text = fs::absolute(file, ec).wstring();
        from.insert(from.end(), text.begin(), text.end());
        from.push_back(L'\0');
        ++queued;
    }
    if (queued == 0) {
        return true;
    }
    from.push_back(L'\0');

    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = from.data();
    // No confirmation and no progress dialog: the window has already asked, and
    // a second modal from the shell would be the same question twice. ALLOWUNDO
    // is the whole point - without it this is an unlink.
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT |
                FOF_NOCONFIRMMKDIR;

    const int result = SHFileOperationW(&op);
    if (result != 0 || op.fAnyOperationsAborted != FALSE) {
        error = result != 0 ? std::format("the shell refused it (0x{:X})", result)
                            : "the shell aborted it";
        return false;
    }
    return true;
}

}  // namespace tb
