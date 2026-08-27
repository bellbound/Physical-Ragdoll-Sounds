#pragma once

// The queue between the Havok solver and the game thread.
//
// A bounded multi-producer, single-consumer ring, Vyukov's algorithm. Each slot
// carries a sequence stamp rather than a ready flag, which is the whole point: an
// overflowing producer fails its claim outright instead of burning a position. A
// flag-per-slot ring leaves the dropped position un-published forever and the
// consumer stops dead at it, so one overflow costs the session.
//
// Producers are Havok worker threads inside the solver, so pushing is lock-free
// and bounded: one relaxed load, one CAS, one release store, no allocation.
//
// Lifted from skse/QuickModMenuNG/src/debug/ImpactRecorder.cpp.

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

#include "rds/Feed.h"

namespace rds::game {

/// 8192 events. A knockdown produces a few hundred contacts, so this is a wide
/// margin rather than a tuned number, and the drop counter says when it was wrong.
inline constexpr std::size_t kRingCapacity = 8192;
static_assert((kRingCapacity & (kRingCapacity - 1)) == 0, "the mask needs a power of two");

class ContactRing {
public:
    ContactRing() { Reset(); }

    /// Never blocks - the alternative is stalling the physics step. A full ring
    /// counts the event as dropped and throws it away.
    void Push(const FeedEvent& event) {
        auto position = m_write.load(std::memory_order_relaxed);
        Slot* slot = nullptr;
        while (true) {
            slot = &m_slots[position & kMask];
            const auto sequence = slot->sequence.load(std::memory_order_acquire);
            const auto difference =
                static_cast<std::intptr_t>(sequence) - static_cast<std::intptr_t>(position);
            if (difference == 0) {
                if (m_write.compare_exchange_weak(position, position + 1,
                                                  std::memory_order_relaxed)) {
                    break;
                }
            } else if (difference < 0) {
                m_dropped.fetch_add(1, std::memory_order_relaxed);
                return;
            } else {
                position = m_write.load(std::memory_order_relaxed);
            }
        }
        slot->event = event;
        slot->sequence.store(position + 1, std::memory_order_release);
    }

    /// Append everything published so far to `out`, oldest first. One consumer
    /// only - the game thread.
    void Drain(std::vector<FeedEvent>& out) {
        while (true) {
            auto& slot = m_slots[m_read & kMask];
            const auto sequence = slot.sequence.load(std::memory_order_acquire);
            const auto difference =
                static_cast<std::intptr_t>(sequence) - static_cast<std::intptr_t>(m_read + 1);
            if (difference != 0) {
                return;  // empty, or the producer of this slot is still writing
            }
            out.push_back(slot.event);
            slot.sequence.store(m_read + kRingCapacity, std::memory_order_release);
            ++m_read;
        }
    }

    [[nodiscard]] std::uint64_t Dropped() const { return m_dropped.load(std::memory_order_relaxed); }

    /// Only with no producers running - between sessions, or on a load screen
    /// after every listener has been detached.
    void Reset() {
        for (std::size_t index = 0; index < kRingCapacity; ++index) {
            m_slots[index].sequence.store(index, std::memory_order_relaxed);
        }
        m_write.store(0, std::memory_order_relaxed);
        m_read = 0;
        m_dropped.store(0, std::memory_order_relaxed);
    }

private:
    static constexpr std::size_t kMask = kRingCapacity - 1;

    struct Slot {
        std::atomic<std::size_t> sequence{};
        FeedEvent event;
    };

    std::array<Slot, kRingCapacity> m_slots{};
    std::atomic<std::size_t> m_write{0};
    std::size_t m_read{0};  // drain thread only
    std::atomic<std::uint64_t> m_dropped{0};
};

}  // namespace rds::game
