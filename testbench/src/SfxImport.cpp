#include "SfxImport.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <format>
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

/// Convert to mono / 48 kHz / 16-bit PCM. `extraFilter` lets the correlation
/// probe ask for the same conversion at two channels instead.
[[nodiscard]] bool Convert(const fs::path& source, const fs::path& destination, int channels,
                           std::string& error) {
    const Tools& tools = FindTools();
    if (tools.ffmpeg.empty()) {
        error = "ffmpeg is not on PATH, so " + source.extension().string() +
                " cannot be converted. wav sources still import.";
        return false;
    }
    std::wostringstream cmd;
    cmd << L'"' << Widen(tools.ffmpeg) << L'"' << L" -y -v error -i \"" << source.wstring() << L'"'
        << L" -ac " << channels << L" -ar " << kTargetRate << L" -c:a pcm_s16le -map_metadata -1 \""
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
                                     "expect it to sound dull against the rest of the pack.",
                                     info.sampleRate));
    }
    if (info.Lossy()) {
        const long long kbps = info.bitrate / 1000;
        if (kbps > 0 && kbps < 96) {
            warn("low bitrate",
                 std::format("{} at {} kbps. Lossy encoders spend their bits on tone and throw away "
                             "exactly the transient detail an impact is made of.",
                             info.codec, kbps));
        } else {
            warn("lossy", std::format("the source was {}. Fine for a bed, worth re-sourcing for a "
                                      "transient layer.",
                                      info.codec));
        }
    }
    if (info.channels > 2) {
        warn("downmixed", std::format("{} channels folded to mono. Check it still sounds like what "
                                      "you picked.",
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

    if (AlreadyCorrect(info, source)) {
        fs::copy_file(source, destination, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            outcome.error = "could not copy: " + ec.message();
            return outcome;
        }
    } else {
        std::string error;
        if (!Convert(source, destination, kTargetChannels, error)) {
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

    JudgeSfx(entry);
    JudgeSource(info, entry);
    if (haveCorrelation && correlation < 0.6f) {
        entry.warnings.push_back(
            {"uncorrelated",
             std::format("the source's two channels correlate at {:.2f}. Below about 0.6 they comb-"
                         "filter when summed, and summing is what the mono fold does - this will "
                         "sound thinner and hollower than the stereo file did.",
                         correlation),
             false});
    }
    entry.suggested = SuggestSlots(entry);

    library.Upsert(entry);
    outcome.file = filename;

    spdlog::info("sfx: imported {} -> {} ({:.0f} ms, {}{}{} warning(s))", source.filename().string(),
                 filename, entry.durationMs, entry.loops ? "loops, " : "",
                 outcome.converted ? "converted, " : "", entry.warnings.size());
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
    entry.suggested = SuggestSlots(entry);

    library.Upsert(entry);
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
