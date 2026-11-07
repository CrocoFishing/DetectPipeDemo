#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include "event_types.hpp"

namespace demo {
// Synchronization belongs to the caller. No allocation, filesystem or subsystem calls.
class EventLedger {
public:
    static constexpr size_t Capacity = 16;
    explicit EventLedger(uint32_t boot_session = 0) : boot_session_(boot_session) {}
    EventContext* open(int64_t timestamp_us, uint64_t sample) {
        if (!boot_session_ || sequence_ == std::numeric_limits<uint32_t>::max()) return nullptr;
        for (auto& c : contexts_) {
            if (c.event_id) continue;
            c = {};
            c.event_id = (static_cast<uint64_t>(boot_session_) << 32) | ++sequence_;
            c.vad_start_sample = sample;
            c.vad_start_timestamp_us = timestamp_us;
            ++active_;
            if (active_ > high_water_) high_water_ = active_;
            return &c;
        }
        return nullptr;
    }
    EventContext* find(uint64_t id) {
        if (!id) return nullptr;
        for (auto& c : contexts_) if (c.event_id == id) return &c;
        return nullptr;
    }
    bool release(uint64_t id) {
        auto* c = find(id);
        if (!c || (c->state != EventState::Complete && c->state != EventState::CompleteWithError)) return false;
        *c = {};
        --active_;
        return true;
    }
    std::array<EventContext, Capacity>& contexts() { return contexts_; }
    size_t active_count() const { return active_; }
    size_t high_water() const { return high_water_; }
private:
    uint32_t boot_session_{0}, sequence_{0};
    size_t active_{0}, high_water_{0};
    std::array<EventContext, Capacity> contexts_{};
};
} // namespace demo
