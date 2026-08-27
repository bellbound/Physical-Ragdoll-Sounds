#include "VanillaImpactHook.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <span>
#include <string_view>

#include "GameFeed.h"
#include "VanillaGate.h"
#include "VanillaSuppression.h"

namespace rds::game {
namespace {

using ImpactSoundData = RE::BGSImpactManager::ImpactSoundData;

/// `PlayImpactDataSounds`. In every address library on every runtime, which is
/// what makes it usable as the anchor the call site is measured from and as the
/// target the call site is checked against.
[[nodiscard]] std::uintptr_t AnchorAddress() {
    static const std::uintptr_t address =
        REL::VariantID(35317, 36212, 0x5A9FF0).address();
    return address;
}

/// Anchor to call site, per runtime. Zero means "not measured on this one", which
/// the signature scan covers. VR is 0x5ABF54 - 0x5A9FF0; SE and AE are zero
/// because neither binary could be read here. Filling either in is one number, and
/// the verification below is what makes guessing safe to try.
[[nodiscard]] std::ptrdiff_t CallSiteFromAnchor() {
    return REL::Relocate<std::ptrdiff_t>(0, 0, 0x1F64);
}

/// The prologue of the per-pair collision helper, the function the call lives in:
///
///     40 53              push rbx
///     48 83 EC 60        sub  rsp, 0x60
///     48 8B 49 48        mov  rcx, [rcx + 0x48]     ; manager -> impact set map
///     49 8B D8           mov  rbx, r8               ; the collision event
///     48 85 C9           test rcx, rcx
///
/// Sixteen bytes with no relocation in them, unique in VR's `.text`, with the call
/// at +0x84. Used only when the offset above is zero or fails to verify: a
/// signature that matches twice is worse than one that matches never.
constexpr std::uint8_t kPrologue[] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x60, 0x48, 0x8B,
                                      0x49, 0x48, 0x49, 0x8B, 0xD8, 0x48, 0x85, 0xC9};
constexpr std::ptrdiff_t kCallFromPrologue = 0x84;

std::atomic<bool> g_suppressed{true};
std::atomic<std::uint64_t> g_seen{};
std::atomic<std::uint64_t> g_dropped{};
VanillaSoundSink g_sink = nullptr;
bool g_installed = false;

/// Is `address` a five-byte relative call to `target`? Both halves matter: the
/// opcode alone is enough where the site is an offset into a function we
/// identified, but here the offset crosses functions, so the target check is what
/// turns "this is a call" into "this is *the* call".
[[nodiscard]] bool IsCallTo(std::uintptr_t address, std::uintptr_t target) {
    if (*reinterpret_cast<const std::uint8_t*>(address) != 0xE8) {
        return false;
    }
    const auto displacement = *reinterpret_cast<const std::int32_t*>(address + 1);
    return address + 5 + static_cast<std::ptrdiff_t>(displacement) == target;
}

/// The one prologue match in the module's text, or 0 for none and 0 for several -
/// deliberately the same answer, because two matches means the signature no longer
/// identifies one function.
[[nodiscard]] std::uintptr_t ScanForCallSite(std::uintptr_t target) {
    const auto& module = REL::Module::get();  // a singleton, and not copyable
    const auto text = module.segment(REL::Segment::textx);
    if (text.size() == 0) {
        return 0;
    }
    const std::span haystack{reinterpret_cast<const std::uint8_t*>(text.address()), text.size()};

    std::uintptr_t found = 0;
    std::size_t matches = 0;
    const std::size_t last = haystack.size() - sizeof(kPrologue);
    for (std::size_t i = 0; i <= last; ++i) {
        if (std::memcmp(haystack.data() + i, kPrologue, sizeof(kPrologue)) != 0) {
            continue;
        }
        const std::uintptr_t site = text.address() + i + kCallFromPrologue;
        if (!IsCallTo(site, target)) {
            continue;  // the prologue without the call is a different function
        }
        ++matches;
        found = site;
    }

    if (matches == 1) {
        return found;
    }
    if (matches > 1) {
        spdlog::warn("vanilla hook: the collision helper's signature matched {} times, so it no "
                     "longer names one function; not scanning further",
                     matches);
    }
    return 0;
}

// ── reading the play ─────────────────────────────────────────────────────────

/// The descriptor that is about to sound, and which of the pair it was.
/// `playSound1`/`playSound2` are the caller's light/heavy decision and exactly one
/// is set - read rather than assumed, because a mod ahead of us may set both and
/// the honest record of a double play is the heavy one.
[[nodiscard]] RE::BGSSoundDescriptorForm* Firing(const ImpactSoundData& data, bool& heavy) {
    RE::BGSImpactData* impact = data.impactData;
    if (impact == nullptr) {
        heavy = false;
        return nullptr;
    }
    if (data.playSound2 && impact->sound2 != nullptr) {
        heavy = true;
        return impact->sound2;
    }
    heavy = false;
    return data.playSound1 ? impact->sound1 : nullptr;
}

/// Fill the gain and variance block off a standard sound descriptor. `skyrim_cast`
/// rather than an unchecked cast: a descriptor form can hold a non-standard
/// `BGSSoundDescriptor`, and reading `soundCharacteristics` off one would read a
/// neighbouring field as a volume. A failed cast leaves the block zeroed.
void ReadCharacteristics(RE::BGSSoundDescriptorForm* form, VanillaSoundInfo& out) {
    if (form == nullptr) {
        return;
    }
    out.descriptorFormId = form->GetFormID();

    auto* standard = skyrim_cast<RE::BGSStandardSoundDef*>(form->soundDescriptor);
    if (standard == nullptr) {
        return;
    }
    const auto& characteristics = standard->soundCharacteristics;
    out.staticAttenuation = characteristics.staticAttenuation;
    out.dbVariance = characteristics.dbVariance;
    out.frequencyShift = characteristics.frequencyShift;
    out.frequencyVariance = characteristics.frequencyVariance;
    out.priority = characteristics.priority;
    out.fileCount = static_cast<std::uint8_t>(
        std::min<std::size_t>(standard->soundFiles.size(), 255));
}

/// What vanilla's own call would have answered. Only consulted while suppressing,
/// so the caller's cooldown map is armed as it would have been (see the header).
/// It can differ in one direction - the audio engine can refuse a sound it had a
/// descriptor for - and erring that way keeps the throttle on.
[[nodiscard]] bool WouldHavePlayed(const ImpactSoundData& data) {
    const RE::BGSImpactData* impact = data.impactData;
    if (impact == nullptr) {
        return false;
    }
    return (data.playSound1 && impact->sound1 != nullptr) ||
           (data.playSound2 && impact->sound2 != nullptr);
}

/// Is this play one we are answering for?
///
/// The master flag says whether we may drop anything at all; the gate says whether
/// *this* one is ours. Both, because either alone gets it wrong - the flag alone
/// silences the game outside a knockdown, the gate alone ignores an install that
/// deliberately runs vanilla underneath us.
///
/// A play with no position is left alone: a doubled impact is a mix we did not
/// want, a wrongly dropped one is a sound the player never gets back.
[[nodiscard]] bool ShouldDrop(const ImpactSoundData& data) {
    if (!g_suppressed.load(std::memory_order_relaxed)) {
        return false;
    }
    if (data.position == nullptr) {
        return false;
    }
    return VanillaGateCovers(data.position->x, data.position->y, data.position->z);
}

/// Said once, the first time a descriptor turns out to have no name - this fires
/// per collision, and a warning per contact is a log nobody reads. From the hook's
/// own thread, so a plain atomic exchange rather than anything that allocates.
void WarnOnceAboutNames() {
    static std::atomic<bool> said{false};
    if (said.exchange(true, std::memory_order_relaxed)) {
        return;
    }
    spdlog::warn(
        "vanilla hook: the game has no editor id for these sound descriptors, so the vanilla "
        "track will name none of them and the testbench will not be able to find their wav "
        "files. BGSSoundDescriptorForm does not keep a name of its own - po3_Tweaks (Powerof3's "
        "Tweaks) is what normally provides one. The rows are still written and still carry the "
        "form id, so nothing is lost but the playback");
}

class Hook {
public:
    static bool Play(RE::BGSImpactManager* manager, ImpactSoundData& data) {
        // Anything that is not one of ours goes straight through, untouched and
        // unrecorded. This is the whole difference from nulling: a weapon set
        // reaching a record the body sets also reach keeps its sound.
        if (!IsBodyImpact(data.impactData)) {
            return s_original(manager, data);
        }

        g_seen.fetch_add(1, std::memory_order_relaxed);

        // Recorded either way, and recorded before the decision: a take carries
        // what vanilla was going to do here whether or not we let it.
        const bool suppressed = ShouldDrop(data);
        Record(data, suppressed);

        if (!suppressed) {
            return s_original(manager, data);
        }
        g_dropped.fetch_add(1, std::memory_order_relaxed);
        return WouldHavePlayed(data);
    }

    static inline REL::Relocation<decltype(Play)> s_original;

private:
    static void Record(const ImpactSoundData& data, bool suppressed) {
        if (g_sink == nullptr) {
            return;
        }

        bool heavy = false;
        RE::BGSSoundDescriptorForm* form = Firing(data, heavy);
        if (form == nullptr) {
            // Vanilla reached the play with nothing to play - an impact record
            // whose branch is empty, or one another mod has already silenced.
            // Not a row: a take that carried it would claim a sound the player
            // could never have heard.
            return;
        }

        FeedEvent event{};
        event.timeMs = NowMs();
        event.kind = EventKind::kVanillaSound;
        if (data.position != nullptr) {
            event.position = Vec3{data.position->x, data.position->y, data.position->z};
        }

        VanillaSoundInfo& vanilla = event.vanilla;
        vanilla.impactFormId = data.impactData->GetFormID();
        vanilla.soundLevel = static_cast<std::uint8_t>(data.impactData->data.soundLevel);
        ReadCharacteristics(form, vanilla);
        if (heavy) {
            vanilla.flags |= static_cast<std::uint8_t>(VanillaSoundFlag::kHeavy);
        }
        if (suppressed) {
            vanilla.flags |= static_cast<std::uint8_t>(VanillaSoundFlag::kSuppressed);
        }

        // The descriptor's editor id, because that is the name the testbench
        // resolves wav files from. Truncation is flagged rather than hidden: a
        // cut name resolves to nothing, and "no files found" and "the name was
        // cut" are different bugs.
        const char* editorId = form->GetFormEditorID();
        if (editorId == nullptr || editorId[0] == '\0') {
            // No name. See VanillaSoundFlag::kNameMissing - the descriptor form
            // does not keep one and po3_Tweaks is what usually does. The row is
            // still worth writing: it says vanilla played *something* here, and
            // the form id says which.
            vanilla.flags |= static_cast<std::uint8_t>(VanillaSoundFlag::kNameMissing);
            WarnOnceAboutNames();
        } else {
            if (std::strlen(editorId) >= sizeof(event.text)) {
                vanilla.flags |= static_cast<std::uint8_t>(VanillaSoundFlag::kNameTruncated);
            }
            std::snprintf(event.text, sizeof(event.text), "%s", editorId);
        }

        g_sink(event);
    }
};

}  // namespace

bool InstallVanillaImpactHook(VanillaSoundSink sink) {
    if (g_installed) {
        return true;
    }
    g_sink = sink;

    const std::uintptr_t anchor = AnchorAddress();
    if (anchor == 0) {
        spdlog::error("vanilla hook: PlayImpactDataSounds has no address on this runtime");
        return false;
    }

    std::uintptr_t site = 0;
    const std::ptrdiff_t offset = CallSiteFromAnchor();
    if (offset != 0 && IsCallTo(anchor + offset, anchor)) {
        site = anchor + offset;
        spdlog::info("vanilla hook: call site at anchor+0x{:x} ({:x})", offset, site);
    } else {
        if (offset != 0) {
            spdlog::warn("vanilla hook: anchor+0x{:x} is not a call to PlayImpactDataSounds; "
                         "falling back to the signature scan",
                         offset);
        }
        site = ScanForCallSite(anchor);
        if (site != 0) {
            spdlog::info("vanilla hook: call site found by signature at {:x} (anchor+0x{:x})", site,
                         site - anchor);
        }
    }

    if (site == 0) {
        spdlog::warn("vanilla hook: the collision path's call to PlayImpactDataSounds could not be "
                     "found on this runtime; nothing was patched");
        return false;
    }

    // Not SKSE::AllocTrampoline here - see FrameHook.cpp for why the allocation
    // is plugin.cpp's and why a second one would break the first hook.
    Hook::s_original = SKSE::GetTrampoline().write_call<5>(site, &Hook::Play);
    g_installed = true;
    spdlog::info("vanilla hook: installed - vanilla's body impacts are now dropped per call, on "
                 "the actors we are playing and nowhere else, and every one of them is recorded");
    return true;
}

bool VanillaImpactHookInstalled() { return g_installed; }

void SetVanillaImpactsSuppressed(bool suppressed) {
    g_suppressed.store(suppressed, std::memory_order_relaxed);
}

bool VanillaImpactsSuppressed() { return g_suppressed.load(std::memory_order_relaxed); }

void VanillaImpactCounters(std::uint64_t& seen, std::uint64_t& suppressed) {
    seen = g_seen.load(std::memory_order_relaxed);
    suppressed = g_dropped.load(std::memory_order_relaxed);
}

}  // namespace rds::game
