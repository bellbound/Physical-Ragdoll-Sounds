#include "AudioBlobs.h"

#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <intrin.h>
#include <limits>
#include <memory>
#include <mutex>
#include <vector>

#include "rds/ConfigManager.h"

namespace rds::game {
namespace {

/// `BSExternalAudioIO::ExternalIOInterface`, mirrored because CommonLib forward
/// declares it without a vtable. Slot 0 is the destructor, slot 1 is Open.
/// Vanilla installs a LipAudioInterface here (VR 0x1415BFE80).
class ExternalIOInterface {
public:
    virtual ~ExternalIOInterface() = default;  // 00

    /// 0 on success, with `*a_blobOut` populated and its refCount incremented.
    /// 6 declines the id silently; anything else the engine treats as an error.
    virtual std::uint32_t Open(const RE::BSResource::ID* a_id, ExternalAudioBlob** a_blobOut) = 0;  // 01
};

constexpr std::uint32_t kOpenSuccess = 0;
constexpr std::uint32_t kOpenNotHandled = 6;

/// Where `externalAudioIO` sits inside BSAudioManager, per CommonLib's own
/// layout. Checked rather than assumed: a mismatch means the struct drifted and
/// we would be writing over an unrelated field.
constexpr std::uintptr_t kExpectedSlotOffset = 0x138;

/// How long after the engine's refcount reaches zero the bytes may be freed.
///
/// The refcount alone is not enough. It drops in Close, which can precede the
/// source voice draining its last buffer, and XAudio2 is reading our memory in
/// place - so freeing on zero is a use-after-free with the audio thread on the
/// other end of it.
constexpr auto kFreeGrace = std::chrono::milliseconds(1000);

void IncrementRefCount(ExternalAudioBlob& blob) {
    _InterlockedIncrement(reinterpret_cast<volatile long*>(&blob.refCount));
}

[[nodiscard]] std::int32_t ReadRefCount(const ExternalAudioBlob& blob) {
    return static_cast<std::int32_t>(_InterlockedCompareExchange(
        reinterpret_cast<volatile long*>(const_cast<std::int32_t*>(&blob.refCount)), 0, 0));
}

/// The single interface instance handed to the engine. Static storage, because
/// the engine keeps the pointer for the process lifetime and vanilla's own
/// instance is a static too.
class RagdollAudioIO final : public ExternalIOInterface {
public:
    std::uint32_t Open(const RE::BSResource::ID* a_id, ExternalAudioBlob** a_blobOut) override;

    ExternalIOInterface* previous = nullptr;
};

RagdollAudioIO g_audioIO;

std::uint32_t RagdollAudioIO::Open(const RE::BSResource::ID* a_id, ExternalAudioBlob** a_blobOut) {
    // Audio manager thread, so keep it short. Guarded because an exception
    // escaping this virtual would unwind into engine code with no handler above
    // it.
    try {
        if (a_id == nullptr || a_blobOut == nullptr) {
            return kOpenNotHandled;
        }
        if (a_id->dir == BlobRegistry::kDirId) {
            const std::uint32_t result = BlobRegistry::Get().Resolve(a_id->file, a_id->dir, a_blobOut);
            if (result == kOpenSuccess) {
                return kOpenSuccess;
            }
            // Ours by directory but not a live id - a blob we already retired.
            // Delegate rather than decline, in case the hash ever collides with a
            // real path.
        }
        if (g_audioIO.previous != nullptr) {
            return g_audioIO.previous->Open(a_id, a_blobOut);
        }
    } catch (const std::exception& e) {
        spdlog::error("blobs: exception serving an external audio open: {}", e.what());
    } catch (...) {
        spdlog::error("blobs: unknown exception serving an external audio open");
    }
    return kOpenNotHandled;
}

}  // namespace

/// A registered blob plus the bytes it points at, kept alive until the engine
/// releases it.
struct BlobRegistry::Impl {
    struct Registration {
        std::uint64_t token = 0;  // 0 = free slot
        ExternalAudioBlob blob{};
        std::vector<std::uint8_t> bytes;
        std::uint32_t fileId = 0;
        bool retired = false;

        /// When the engine's refcount was first seen back at zero. The grace runs
        /// from here rather than from the retire, because the engine can hold a
        /// blob long after we are done with it.
        std::chrono::steady_clock::time_point releasedAt{};
    };

    mutable std::mutex mutex;
    std::mutex installMutex;

    /// unique_ptr, not by value: the engine holds a raw pointer to
    /// Registration::blob for the whole playback, so its address must never move.
    std::vector<std::unique_ptr<Registration>> registrations;
    std::uint64_t nextToken = 1;
    std::uint32_t nextFileId = 0;

    std::atomic<bool> installed{false};

    /// Freed only once the engine has let go and the grace has passed. Caller
    /// holds `mutex`.
    ///
    /// Buffers are NOT released: the slot keeps its vector so the next
    /// registration reuses the capacity. A knockdown registers a composite every
    /// few frames and the steady path must not allocate.
    void CollectLocked() {
        const auto now = std::chrono::steady_clock::now();
        for (auto& reg : registrations) {
            if (reg->token == 0 || !reg->retired) {
                continue;
            }
            const std::int32_t refs = ReadRefCount(reg->blob);
            if (refs > 0) {
                // Still held. Reset the stamp so a refcount that goes back up
                // restarts the grace rather than inheriting an older observation.
                reg->releasedAt = {};
                continue;
            }
            if (reg->releasedAt == std::chrono::steady_clock::time_point{}) {
                reg->releasedAt = now;
                continue;
            }
            if (now - reg->releasedAt < kFreeGrace) {
                continue;
            }
            reg->token = 0;
            reg->retired = false;
            reg->releasedAt = {};
            reg->blob.data = nullptr;
            reg->blob.size = 0;
            reg->bytes.clear();  // capacity kept on purpose
        }
    }
};

BlobRegistry& BlobRegistry::Get() {
    static BlobRegistry instance;
    return instance;
}

BlobRegistry::Impl& BlobRegistry::Storage() {
    static Impl impl;
    return impl;
}

const BlobRegistry::Impl& BlobRegistry::Storage() const {
    return const_cast<BlobRegistry*>(this)->Storage();
}

bool BlobRegistry::Installed() const { return Storage().installed.load(std::memory_order_acquire); }

bool BlobRegistry::Install() {
    Impl& impl = Storage();

    // Everything below is one read-modify-write on the engine's interface slot.
    // Two threads interleaving here can chain us to ourselves - which recurses
    // until the audio thread's stack blows - or drop vanilla's lip-sync
    // interface, muting every FUZ voice in the game.
    std::lock_guard lock{impl.installMutex};

    auto* manager = RE::BSAudioManager::GetSingleton();

    // Re-check the slot rather than trusting the flag: a rebuilt BSAudioInit
    // would silently put vanilla's interface back and we would never know.
    if (impl.installed.load(std::memory_order_acquire)) {
        if (manager == nullptr) {
            return true;
        }
        auto** current = reinterpret_cast<ExternalIOInterface**>(&manager->initSettings.externalAudioIO);
        if (*current == &g_audioIO) {
            return true;
        }
        spdlog::warn("blobs: the external audio interface was replaced (now {:X}); reinstalling",
                     reinterpret_cast<std::uintptr_t>(*current));
        impl.installed.store(false, std::memory_order_release);
    }

    if (manager == nullptr) {
        spdlog::error("blobs: no BSAudioManager, so no sound can be played at all");
        return false;
    }

    auto** slot = reinterpret_cast<ExternalIOInterface**>(&manager->initSettings.externalAudioIO);
    if (*slot == &g_audioIO) {
        impl.installed.store(true, std::memory_order_release);
        return true;
    }

    const auto slotOffset =
        reinterpret_cast<std::uintptr_t>(slot) - reinterpret_cast<std::uintptr_t>(manager);
    if (slotOffset != kExpectedSlotOffset) {
        spdlog::warn("blobs: externalAudioIO is at manager+0x{:X}, expected +0x{:X} - CommonLib's "
                     "layout may have drifted",
                     slotOffset, kExpectedSlotOffset);
    }

    // Chain rather than replace. Vanilla's lip-sync interface lives here, and
    // SkyrimNet may already have chained onto it; either order has to work, which
    // is why Open delegates anything that is not ours instead of declining it.
    // The identity check above is what stops us chaining to ourselves.
    g_audioIO.previous = *slot;
    *slot = &g_audioIO;
    impl.installed.store(true, std::memory_order_release);

    spdlog::info("blobs: external audio interface installed at manager+0x{:X}; previous interface "
                 "{:X} ({})",
                 slotOffset, reinterpret_cast<std::uintptr_t>(g_audioIO.previous),
                 g_audioIO.previous != nullptr ? "chained, foreign ids delegate to it"
                                               : "none, nothing was installed before us");
    return true;
}

std::uint64_t BlobRegistry::Register(std::span<const std::uint8_t> wav, std::uint32_t& fileIdOut,
                                     std::uint32_t& dirIdOut) {
    if (wav.empty()) {
        return 0;
    }
    if (wav.size() > std::numeric_limits<std::uint32_t>::max()) {
        spdlog::error("blobs: refusing a {} byte blob, past the engine's 32 bit size field",
                      wav.size());
        return 0;
    }

    Impl& impl = Storage();
    std::lock_guard lock{impl.mutex};
    impl.CollectLocked();

    Impl::Registration* reg = nullptr;
    for (auto& existing : impl.registrations) {
        if (existing->token == 0) {
            reg = existing.get();
            break;
        }
    }
    if (reg == nullptr) {
        impl.registrations.push_back(std::make_unique<Impl::Registration>());
        reg = impl.registrations.back().get();
    }

    reg->token = impl.nextToken++;
    reg->fileId = ++impl.nextFileId;
    reg->retired = false;
    reg->releasedAt = {};
    reg->bytes.assign(wav.begin(), wav.end());

    // Plain store, and only here: the slot was free, so the engine has no
    // reference to it and nothing else can be touching this field yet.
    reg->blob.refCount = 0;
    reg->blob.size = static_cast<std::uint32_t>(reg->bytes.size());
    reg->blob.reserved = 0;
    reg->blob.data = reg->bytes.data();

    fileIdOut = reg->fileId;
    dirIdOut = kDirId;
    return reg->token;
}

std::uint32_t BlobRegistry::Resolve(std::uint32_t fileId, std::uint32_t dirId,
                                    ExternalAudioBlob** out) {
    Impl& impl = Storage();
    std::lock_guard lock{impl.mutex};

    for (auto& reg : impl.registrations) {
        if (reg->token != 0 && reg->fileId == fileId && dirId == kDirId && !reg->retired) {
            // Pre-increment exactly as vanilla's producer does; the engine
            // decrements when the data source closes. Interlocked because that
            // decrement does not happen under our mutex.
            IncrementRefCount(reg->blob);
            *out = &reg->blob;
            return kOpenSuccess;
        }
    }
    return kOpenNotHandled;
}

void BlobRegistry::Retire(std::uint64_t token) {
    if (token == 0) {
        return;
    }
    Impl& impl = Storage();
    std::lock_guard lock{impl.mutex};
    for (auto& reg : impl.registrations) {
        if (reg->token == token) {
            reg->retired = true;
            reg->releasedAt = {};
            break;
        }
    }
    impl.CollectLocked();
}

void BlobRegistry::Collect() {
    Impl& impl = Storage();
    std::lock_guard lock{impl.mutex};
    impl.CollectLocked();
}

void BlobRegistry::Stats(std::size_t& liveOut, std::size_t& bytesOut) const {
    const Impl& impl = Storage();
    std::lock_guard lock{impl.mutex};
    liveOut = 0;
    bytesOut = 0;
    for (const auto& reg : impl.registrations) {
        bytesOut += reg->bytes.capacity();
        if (reg->token != 0) {
            ++liveOut;
        }
    }
}

namespace {

/// Look one output model up, saying which key named it so a load-order problem
/// can be traced back to a line of the ini rather than to a bare form id.
const RE::BSISoundOutputModel* ResolveOutputModel(std::int32_t configured, const char* key) {
    const auto formId = static_cast<RE::FormID>(configured);
    auto* form = RE::TESForm::LookupByID(formId);
    if (form == nullptr) {
        spdlog::error("blobs: {} names {:08X}, which does not exist in this load order",
                      key, formId);
        return nullptr;
    }
    auto* output = form->As<RE::BGSSoundOutput>();
    if (output == nullptr) {
        spdlog::error("blobs: {} names {:08X}, which is a {}, not a sound output model",
                      key, formId, static_cast<int>(form->GetFormType()));
        return nullptr;
    }
    spdlog::info("blobs: {} = {:08X} resolved", key, formId);
    return static_cast<const RE::BSISoundOutputModel*>(output);
}

}  // namespace

const RE::BSISoundOutputModel* DefaultOutputModel() {
    static const RE::BSISoundOutputModel* model = [] () -> const RE::BSISoundOutputModel* {
        const RE::BSISoundOutputModel* resolved = ResolveOutputModel(
            ConfigManager::Get().General().audio.outputModelFormId, "iOutputModelFormID");
        if (resolved == nullptr) {
            spdlog::error("blobs: every voice will be flat and follow the listener around");
        }
        return resolved;
    }();
    return model;
}

namespace {

/// The plugin the two category records live in, and their ids inside it. Local
/// plus name rather than a whole form id, because the load index is not known
/// until runtime. Kept in step with tools/make_categories_esp.py.
constexpr const char* kCategoryPlugin = "RagdollSounds.esp";
constexpr RE::FormID kRagdollCategoryLocalFormID = 0x000800;
constexpr RE::FormID kGoreCategoryLocalFormID = 0x000801;

/// One category, resolved once and then cached, retrying for as long as it comes
/// back null.
///
/// NOT a magic static, and that is the whole point. A magic static keeps whatever
/// the first call produced, so one lookup before the load order is up would pin
/// "no category" for the session - and the symptom, sliders that work on some
/// launches and not others, points nowhere near the cause. `reported` keeps the
/// retry from printing a line per voice on an install that really is missing the
/// esp; a lost race repeats it at most once per racing thread and stores the same
/// pointer twice, so relaxed ordering is enough.
const RE::BSISoundCategory* ResolveCategory(std::atomic<const void*>& cache,
                                            std::atomic<bool>& reported, RE::FormID localFormID,
                                            const char* what) {
    if (const auto* cached = cache.load(std::memory_order_acquire)) {
        return static_cast<const RE::BSISoundCategory*>(cached);
    }

    auto* dataHandler = RE::TESDataHandler::GetSingleton();
    auto* form = dataHandler ? dataHandler->LookupForm<RE::BGSSoundCategory>(localFormID,
                                                                            kCategoryPlugin)
                             : nullptr;
    if (form == nullptr) {
        if (!reported.exchange(true, std::memory_order_relaxed)) {
            spdlog::warn("blobs: no {} sound category ({} {:06X}) - the mod still sounds, but it "
                         "has no slider on the Audio settings page. Check the esp is installed and "
                         "ticked. Said once.",
                         what, kCategoryPlugin, localFormID);
        }
        return nullptr;
    }

    // The engine wants the BSISoundCategory subobject, which sits at +0x30 inside
    // the form; the static_cast is what applies that offset. Handing it the
    // TESForm pointer instead compiles and misreads every field.
    const auto* resolved = static_cast<const RE::BSISoundCategory*>(form);
    spdlog::info("blobs: {} sound category {:08X} '{}' resolved", what, form->GetFormID(),
                 form->GetFullName() != nullptr ? form->GetFullName() : "");
    cache.store(resolved, std::memory_order_release);
    return resolved;
}

std::atomic<const void*> g_ragdollCategory{nullptr};
std::atomic<const void*> g_goreCategory{nullptr};
std::atomic<bool> g_ragdollReported{false};
std::atomic<bool> g_goreReported{false};

}  // namespace

const RE::BSISoundCategory* CategoryFor(SoundBus bus) {
    switch (bus) {
        case SoundBus::kGore:
            return ResolveCategory(g_goreCategory, g_goreReported, kGoreCategoryLocalFormID,
                                   "gore");
        case SoundBus::kMain:
        default:
            return ResolveCategory(g_ragdollCategory, g_ragdollReported,
                                   kRagdollCategoryLocalFormID, "ragdoll");
    }
}

const RE::BSISoundOutputModel* TapOutputModel() {
    static const RE::BSISoundOutputModel* model = [] () -> const RE::BSISoundOutputModel* {
        const std::int32_t configured = ConfigManager::Get().General().audio.tapOutputModelFormId;
        if (configured == 0) {
            return DefaultOutputModel();
        }
        // A bad id here falls back rather than failing: the split is a
        // refinement, and losing it is worth much less than losing the model
        // altogether, which is what returning null would cost the taps.
        const RE::BSISoundOutputModel* resolved =
            ResolveOutputModel(configured, "iTapOutputModelFormID");
        if (resolved == nullptr) {
            spdlog::warn("blobs: taps will use the same model as everything else");
            return DefaultOutputModel();
        }
        return resolved;
    }();
    return model;
}

}  // namespace rds::game
