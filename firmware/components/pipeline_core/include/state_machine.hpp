#pragma once
#include <cstdint>
#include "pipeline_types.hpp"

namespace demo {
class StateMachine {
public:
    SystemState state() const { return state_; }
    void transition(SystemState next, uint32_t image_id, TriggerSource source);
private:
    SystemState state_{SystemState::Initializing};
};
}  // namespace demo

