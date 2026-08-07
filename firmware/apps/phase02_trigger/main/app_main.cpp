#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "state_machine.hpp"
#include "trigger_service.hpp"

namespace {
demo::TriggerService triggers;
demo::StateMachine state;
void pipeline(void*) {
    while (true) { demo::CaptureRequest r{}; if (!triggers.receive(r, portMAX_DELAY)) continue;
        const uint32_t image = r.request_id & 0xffff; state.transition(demo::SystemState::Capturing, image, r.trigger_source);
        triggers.set_active(image, demo::SystemState::Capturing); vTaskDelay(pdMS_TO_TICKS(500));
        state.transition(demo::SystemState::Completed, image, r.trigger_source); state.transition(demo::SystemState::Idle, image, r.trigger_source); triggers.complete(); }
}
void mock_client(void*) {
    vTaskDelay(pdMS_TO_TICKS(1500));
    auto a = triggers.submit_ble(42); auto busy = triggers.submit_ble(43); auto duplicate = triggers.submit_ble(42);
    ESP_LOGI("phase02", "mock accepted=%d busy=%d duplicate_while_busy=%d", static_cast<int>(a), static_cast<int>(busy), static_cast<int>(duplicate));
    vTaskDelay(pdMS_TO_TICKS(1000)); duplicate = triggers.submit_ble(42);
    ESP_LOGI("phase02", "duplicate_after_complete=%d expected=%d", static_cast<int>(duplicate), static_cast<int>(demo::SubmitResult::Duplicate)); vTaskDelete(nullptr);
}
}
extern "C" void app_main() {
    if (triggers.initialize() != ESP_OK) { ESP_LOGE("phase02", "[TEST] phase=2 result=FAIL"); return; }
    state.transition(demo::SystemState::Idle, 0, demo::TriggerSource::ExternalButton);
    xTaskCreate(pipeline, "mock_pipeline_task", 3072, nullptr, 5, nullptr); xTaskCreate(mock_client, "mock_ble_client", 3072, nullptr, 4, nullptr);
}

