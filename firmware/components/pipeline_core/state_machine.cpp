#include "state_machine.hpp"
#include "esp_log.h"

namespace demo {
const char* state_name(SystemState s) {
    static constexpr const char* names[] = {"INITIALIZING","IDLE","CAPTURING","DETECTING","CROPPING","ENCODING","TRANSMITTING","WAITING_RESULT","COMPLETED","NO_FACE","ERROR_RECOVERY"};
    return names[static_cast<unsigned>(s)];
}
const StatePolicy& state_policy(SystemState s) {
    static constexpr StatePolicy p[] = {
        {15000,false,"boot and initialize modules","IDLE","ERROR_RECOVERY","reinitialize failing module or reboot on fatal"},
        {0,true,"wait on CaptureRequest queue","CAPTURING","IDLE","none"},
        {4000,false,"acquire camera frame (esp32-camera bounded wait)","DETECTING","ERROR_RECOVERY","return frame; camera reinitialize"},
        {6000,false,"run fixed face detector","CROPPING/NO_FACE","ERROR_RECOVERY","return frame; detector reload"},
        {1000,false,"expand 12.5% per side, clamp, copy","ENCODING","ERROR_RECOVERY","release crop slot"},
        {3000,false,"JPEG encode at centralized quality","TRANSMITTING","ERROR_RECOVERY","release JPEG slot"},
        {15000,false,"send begin/chunks/end with backpressure","WAITING_RESULT","ERROR_RECOVERY","abort transfer and reclaim all buffers"},
        {15000,false,"validate recognition response IDs","COMPLETED","ERROR_RECOVERY","report timeout/stale result"},
        {250,false,"log result and metrics","IDLE","ERROR_RECOVERY","release request ownership"},
        {250,false,"emit NO_FACE","IDLE","ERROR_RECOVERY","release frame and request"},
        {3000,false,"release owned buffers and reset module","IDLE","INITIALIZING","module-specific recovery; reboot only if fatal"},
    };
    return p[static_cast<unsigned>(s)];
}
void StateMachine::transition(SystemState next, uint32_t image_id, TriggerSource source) {
    ESP_LOGI("state", "[STATE] from=%s to=%s image_id=%lu trigger=%s", state_name(state_), state_name(next),
             static_cast<unsigned long>(image_id), trigger_source_name(source)); state_ = next;
}
}  // namespace demo
