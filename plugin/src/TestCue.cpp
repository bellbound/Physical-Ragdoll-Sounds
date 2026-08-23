#include "TestCue.h"

#include <spdlog/spdlog.h>

#include <atomic>

#include "GameRenderer.h"
#include "rds/Cue.h"

namespace rds::game {
namespace {

/// The layer stack, straight out of the design's section 5 with the levels
/// 03-Asset-Status settled on: mixed loudest the sub buries everything, and
/// solved against the reference band curve it sits 8 dB under the transient.
struct Layer {
    SlotId slot;
    double offsetMs;
    float gainDb;
    CueReason reason;
};

constexpr Layer kLayers[] = {
    {SlotId::kImpTransient, 0.0, 0.0f, CueReason::kImpactComposite},
    {SlotId::kSurfStone, 8.0, -6.0f, CueReason::kSurfaceSkin},
    {SlotId::kImpBody, 20.0, -3.0f, CueReason::kImpactComposite},
    {SlotId::kImpSub, 65.0, -8.0f, CueReason::kImpactComposite},
};

std::atomic<bool> g_fireRequested{false};
std::int32_t g_scanCode = 0;

class TestCueSink final : public RE::BSTEventSink<RE::InputEvent*> {
public:
    static TestCueSink* GetSingleton() {
        static TestCueSink instance;
        return &instance;
    }

    RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* a_event,
                                          RE::BSTEventSource<RE::InputEvent*>*) override {
        if (a_event == nullptr || g_scanCode <= 0) {
            return RE::BSEventNotifyControl::kContinue;
        }
        for (auto* event = *a_event; event != nullptr; event = event->next) {
            const auto* button = event->AsButtonEvent();
            if (button == nullptr || !button->IsDown()) {
                continue;
            }
            if (button->GetDevice() != RE::INPUT_DEVICE::kKeyboard) {
                continue;
            }
            if (static_cast<std::int32_t>(button->GetIDCode()) == g_scanCode) {
                // Only flagged here. The input sink is not the game thread's
                // frame, and everything the renderer touches - the audio manager,
                // the player's position - wants to be read from the tick.
                g_fireRequested.store(true, std::memory_order_release);
            }
        }
        return RE::BSEventNotifyControl::kContinue;
    }
};

}  // namespace

void InstallTestCue(GameRenderer* renderer, std::int32_t scanCode) {
    if (renderer == nullptr || scanCode <= 0) {
        return;
    }
    g_scanCode = scanCode;

    auto* manager = RE::BSInputDeviceManager::GetSingleton();
    if (manager == nullptr) {
        spdlog::warn("test cue: no input device manager, key {} will do nothing", scanCode);
        return;
    }
    manager->AddEventSink(TestCueSink::GetSingleton());
    spdlog::info("test cue: scancode {} plays one canned impact where you stand", scanCode);
}

void FireTestCue(GameRenderer& renderer, double nowMs) {
    Vec3 position{};
    if (auto* player = RE::PlayerCharacter::GetSingleton()) {
        const auto p = player->GetPosition();
        position = {p.x, p.y, p.z};
    }

    for (const Layer& layer : kLayers) {
        Cue cue{};
        cue.timeMs = nowMs + layer.offsetMs;
        cue.op = CueOp::kPlayOneShot;
        cue.slot = layer.slot;
        cue.variant = 0;
        cue.gainDb = layer.gainDb;
        cue.pitch = 1.0f;
        cue.position = position;
        cue.boneIndex = -1;
        // One actor and one sequence, so the four land in one group and mix into
        // one voice - which is the thing being tested.
        cue.actorId = 0;
        cue.sourceSeq = 0;
        cue.reason = layer.reason;
        cue.intensity = 0.8f;
        renderer.Emit(cue);
    }
    spdlog::info("test cue: fired a {}-layer composite at ({:.0f}, {:.0f}, {:.0f})",
                 std::size(kLayers), position.x, position.y, position.z);
}

bool TakeTestCueRequest() {
    return g_fireRequested.exchange(false, std::memory_order_acq_rel);
}

}  // namespace rds::game
