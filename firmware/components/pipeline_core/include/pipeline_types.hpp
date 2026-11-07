#pragma once
#include <cstdint>

namespace demo {
enum class TriggerSource : uint8_t { ExternalButton = 1, BleClient = 2, EventVad = 3 };
struct CaptureRequest {
    uint32_t request_id;
    TriggerSource trigger_source;
    int64_t created_at_us;
    bool has_event{false};
    uint64_t event_id{0};
};
inline const char* trigger_source_name(TriggerSource s) {
    switch (s) {
    case TriggerSource::ExternalButton: return "EXTERNAL_BUTTON";
    case TriggerSource::BleClient: return "BLE_CLIENT";
    case TriggerSource::EventVad: return "EVENT_VAD";
    }
    return "UNKNOWN";
}

enum class SystemState : uint8_t {
    Initializing, Idle, Capturing, Detecting, Cropping, Encoding, Transmitting,
    WaitingResult, Completed, NoFace, ErrorRecovery,
};
const char* state_name(SystemState state);
struct StatePolicy {
    uint32_t timeout_ms; bool accepts_trigger; const char* entry; const char* success; const char* failure; const char* recovery;
};
const StatePolicy& state_policy(SystemState state);
}  // namespace demo

