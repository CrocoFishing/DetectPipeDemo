#include "board_support.hpp"
#include "camera_service.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern "C" void app_main() {
    demo::CameraService camera;
    if (camera.initialize() != ESP_OK) { ESP_LOGE("phase01", "[TEST] phase=1 result=FAIL reason=camera_init"); return; }
    board::log_memory("before_capture");
    auto frame = camera.capture();
    if (!frame) { ESP_LOGE("phase01", "[TEST] phase=1 result=FAIL reason=capture"); return; }
    ESP_LOGI("phase01", "[METRIC] phase=1 image_id=1 metric=frame_bytes value=%u unit=bytes", static_cast<unsigned>(frame->len));
    frame.reset(); board::log_memory("after_frame_return");
    ESP_LOGI("phase01", "[TEST] phase=1 result=PASS_LOCAL_CHECK hardware_claim=false");
    while (true) vTaskDelay(pdMS_TO_TICKS(1000));
}

