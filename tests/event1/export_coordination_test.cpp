#include <algorithm>
#include <cassert>
#include <cstdio>
#include "export_test_shims.hpp"
#include "../../firmware/apps/event1_audio_face/main/sd_export_integration.cpp"

namespace protocol {
size_t encode_payload(const SdExportStatusPayload&, uint8_t*, size_t capacity) {
    return capacity >= SD_EXPORT_STATUS_PAYLOAD_SIZE ? SD_EXPORT_STATUS_PAYLOAD_SIZE : 0;
}
}
static void reset() {
    fake_now = owners_finish_at = 0;
    storage_mounted = audio_running = true;
    audio_paused = false;
    suspend_error = resume_error = barrier_error = ESP_OK;
    export_calls.clear();
}
static bool never_cancel(void*) { return false; }
static bool cancel_after_delay(void*) { return fake_now >= 20'000; }
static size_t call_index(const char* name) {
    auto found = std::find(export_calls.begin(), export_calls.end(), name);
    assert(found != export_calls.end());
    return static_cast<size_t>(found - export_calls.begin());
}
int main() {
    demo::SystemController controller;
    demo::EventManager events;
    demo::SdExportIntegration integration;
    assert(integration.initialize(controller, events) == ESP_OK);
    const auto hooks = demo::captured_hooks;
    reset();
    owners_finish_at = 50'000;
    assert(hooks.prepare(hooks.context, 100, never_cancel, nullptr) == ESP_OK);
    assert(fake_now >= owners_finish_at && controller.gated && events.gated && audio_paused);
    assert(call_index("capture_gate") < call_index("suspend"));
    assert(call_index("event_gate") < call_index("suspend"));
    assert(call_index("suspend") < call_index("barrier"));
    assert(hooks.resume(hooks.context) == ESP_OK);
    assert(audio_running && !controller.gated && !events.gated);
    assert(call_index("resume") < call_index("event_open"));

    reset(); owners_finish_at = 1'000'000;
    assert(hooks.prepare(hooks.context, 30, never_cancel, nullptr) == ESP_ERR_TIMEOUT);
    assert(audio_running && !audio_paused);
    assert(std::find(export_calls.begin(), export_calls.end(), "suspend") == export_calls.end());
    assert(hooks.resume(hooks.context) == ESP_OK && !events.gated && !controller.gated);

    reset(); owners_finish_at = 1'000'000;
    assert(hooks.prepare(hooks.context, 100, cancel_after_delay, nullptr) == ESP_FAIL);
    assert(audio_running && !audio_paused && fake_now == 20'000);
    assert(hooks.resume(hooks.context) == ESP_OK);

    reset(); suspend_error = ESP_ERR_NO_MEM;
    assert(hooks.prepare(hooks.context, 100, never_cancel, nullptr) == ESP_ERR_NO_MEM);
    resume_error = ESP_ERR_NO_MEM;
    assert(hooks.resume(hooks.context) == ESP_ERR_NO_MEM && controller.gated && events.gated);
    resume_error = ESP_OK;
    assert(hooks.resume(hooks.context) == ESP_OK && audio_running && !events.gated);

    reset(); barrier_error = ESP_FAIL;
    assert(hooks.prepare(hooks.context, 100, never_cancel, nullptr) == ESP_FAIL && audio_paused);
    assert(hooks.resume(hooks.context) == ESP_OK && audio_running);

    reset(); storage_mounted = false;
    assert(hooks.prepare(hooks.context, 100, never_cancel, nullptr) == ESP_ERR_NOT_FOUND);
    assert(export_calls.empty() && !events.gated && !controller.gated);
    std::puts("export coordination passed (drain, timeout, cancellation, partial suspend, restore retry, barrier, missing SD)");
}
