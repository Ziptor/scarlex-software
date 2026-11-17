#pragma once

#include "game_entities.h"

#include <array>
#include <atomic>
#include <chrono>
#include <mutex>

// One producer and one render thread. A callback borrows a slot for its duration.
class SnapshotBuffer final {
public:
    using Clock = std::chrono::steady_clock;

    struct Timing {
        Clock::time_point started_at{};
        Clock::time_point ready_at{};
        std::uint64_t read_calls{};
        std::uint64_t ioctl_count{};
        std::uint64_t discarded_retries{};
    };

    explicit SnapshotBuffer(const game_entities::GameEntities& prototype)
        : slots_{Slot{prototype}, Slot{prototype}} {}

    template<class Update>
    bool try_publish(Update&& update) {
        const int next = 1 - current_index_.load(std::memory_order_relaxed);
        auto& slot = slots_[next];
        std::unique_lock lock(slot.mutex, std::try_to_lock);
        if (!lock.owns_lock()) {
            return false;
        }
        Timing timing;
        timing.started_at = Clock::now();
        if (!update(slot.snapshot, timing)) {
            return false;
        }
        timing.ready_at = Clock::now();
        slot.timing = timing;
        lock.unlock();
        current_index_.store(next, std::memory_order_release);
        return true;
    }

    template<class Render>
    bool try_read(Render&& render) {
        // Publication can race the initial index load. Retry the newest slot once.
        for (int attempt = 0; attempt < 2; ++attempt) {
            const int current = current_index_.load(std::memory_order_acquire);
            auto& slot = slots_[current];
            const std::unique_lock lock(slot.mutex, std::try_to_lock);
            if (lock.owns_lock() && current == current_index_.load(std::memory_order_acquire)) {
                render(static_cast<const game_entities::GameEntities&>(slot.snapshot),
                       static_cast<const Timing&>(slot.timing));
                return true;
            }
        }
        return false;
    }

private:
    struct Slot {
        explicit Slot(const game_entities::GameEntities& prototype) : snapshot(prototype) {}

        game_entities::GameEntities snapshot;
        Timing timing{};
        std::mutex mutex;
    };

    std::array<Slot, 2> slots_;
    std::atomic<int> current_index_{0};
};
