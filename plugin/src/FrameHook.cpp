#include "FrameHook.h"

#include <spdlog/spdlog.h>

namespace rds::game {
namespace {

/// The game's per-frame delta, in seconds.
///
/// Resolved on first use rather than at namespace scope: static initialisers run
/// at DLL load, before SKSE has told REL which runtime this is, and an address
/// taken there is taken against nothing.
[[nodiscard]] const float* DeltaTimeAddress() {
    static const float* address = [] {
        const auto id = REL::VariantID(523660, 410199, 0x30C3A08);
        return reinterpret_cast<const float*>(id.address());
    }();
    return address;
}

}  // namespace

bool FrameHook::Install(void (*onFrame)()) {
    if (s_installed) {
        return true;
    }
    if (onFrame == nullptr) {
        return false;
    }
    s_onFrame = onFrame;

    // The trampoline is allocated once, by plugin.cpp, before either hook is
    // installed. Not here: SKSE::AllocTrampoline *releases* whatever block it
    // held, so a second caller would free the branch the first one is already
    // running through.
    auto& trampoline = SKSE::GetTrampoline();

    const auto address = REL::VariantID(35565, 36564, 0x5BAB10).address();
    const auto offset = REL::VariantOffset(0x748, 0xC26, 0x7EE).offset();

    // The call site is a five-byte relative call on every runtime. Verifying the
    // opcode before writing turns "some future patch moved this" into a log line
    // and a silent mod, rather than a trampoline pointed at the middle of an
    // instruction.
    constexpr std::uint8_t kCallOpcode = 0xE8;
    if (!REL::verify_code(address + offset, &kCallOpcode, 1)) {
        spdlog::error("FrameHook: the main update call site at {:x}+0x{:x} is not a call; "
                      "not hooking, and the engine will not be ticked",
                      address, offset);
        return false;
    }

    s_original = trampoline.write_call<5>(address + offset, &FrameHook::OnMainThreadUpdate);
    s_installed = true;
    spdlog::info("FrameHook: installed at {:x}+0x{:x}", address, offset);
    return true;
}

bool FrameHook::Installed() { return s_installed; }

float FrameHook::DeltaSeconds() {
    const float* delta = DeltaTimeAddress();
    if (delta == nullptr) {
        return 0.0f;
    }
    // Clamped, not trusted. A load screen or a breakpoint hands back a delta of
    // whole seconds, and a window scaled against that would swallow a knockdown
    // whole. 24 to 144 fps is the design's range; anything outside it is the
    // frame that did not really happen.
    const float value = *delta;
    if (!(value > 0.0f) || value > 0.5f) {
        return 0.0f;
    }
    return value;
}

void FrameHook::OnMainThreadUpdate() {
    s_original();
    if (s_onFrame != nullptr) {
        // Nothing here may throw into the frame loop: there is no handler above
        // us and an escaping exception is a crash with the engine's name on it.
        try {
            s_onFrame();
        } catch (const std::exception& e) {
            spdlog::error("FrameHook: the tick threw: {}", e.what());
        } catch (...) {
            spdlog::error("FrameHook: the tick threw something that is not an exception");
        }
    }
}

}  // namespace rds::game
