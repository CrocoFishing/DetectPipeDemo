#pragma once
#include <cstdint>

namespace demo {
enum class TriggerSource : uint8_t { ExternalButton = 1, BleClient = 2 };
struct CaptureRequest { uint32_t request_id; TriggerSource trigger_source; int64_t created_at_us; };
inline const char* trigger_source_name(TriggerSource s) { return s == TriggerSource::ExternalButton ? "EXTERNAL_BUTTON" : "BLE_CLIENT"; }

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

