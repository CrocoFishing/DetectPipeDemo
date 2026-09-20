#pragma once

#include <cstddef>
#include <cstdint>
#include "ble_transport.hpp"
#include "camera_service.hpp"
#include "face_detection_service.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "image_processing.hpp"
#include "metrics_service.hpp"
#include "state_machine.hpp"
#include "trigger_service.hpp"

namespace demo {
class SystemController {
public:
    SystemController();
    esp_err_t initialize();
    void start();
private:
    struct RxPacket { uint16_t length; uint8_t data[256]; };
    static void rx_control(const uint8_t* data, size_t length, void* context);
    static void rx_result(const uint8_t* data, size_t length, void* context);
    static void on_button_submission(const CaptureRequest& request, SubmitResult result, void* context);
    static void pipeline_task_entry(void* arg);
    static void ble_event_task_entry(void* arg);
    static void metrics_task_entry(void* arg);
    void pipeline_task();
    void ble_event_task();
    void metrics_task();
    void enqueue_rx(const uint8_t* data, size_t length);
    void transition(SystemState state, const CaptureRequest& request, uint32_t image_id);
    void recover(const CaptureRequest& request, uint32_t image_id, protocol::ErrorCode error);
    bool await_result(const CaptureRequest& request, uint32_t image_id,
                      uint16_t face_index, uint16_t face_count, uint8_t& result_status);

    MetricsService metrics_{6}; BleTransport ble_{metrics_}; TriggerService triggers_;
    CameraService camera_; FaceDetectionService detector_; ReusableImageBuffers buffers_; StateMachine state_;
    QueueHandle_t rx_queue_{nullptr}; QueueHandle_t result_queue_{nullptr};
    uint32_t next_image_id_{1};
};
}  // namespace demo
