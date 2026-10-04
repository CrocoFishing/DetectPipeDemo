#pragma once

#include <cstddef>
#include <cstdint>
#include "ble_transport.hpp"
#include "camera_service.hpp"
#include "event_types.hpp"
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
    esp_err_t start();
    // Returns a nonzero generated request_id only when accepted by the shared one-slot queue.
    SubmitResult submit_event_capture(uint64_t event_id, uint32_t& request_id);
    // Install before start. Callbacks run on the camera task and must only enqueue metadata.
    void set_event_observers(void (*batch)(const FaceBatchResult&, void*),
                             void (*face)(const FaceAssociation&, void*), void* context);
    esp_err_t send_event_message(protocol::MessageType type, uint32_t request_id,
                                 const uint8_t* payload, size_t length);
    bool event_client_ready() const;
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
    void recover(const CaptureRequest& request, uint32_t image_id, protocol::ErrorCode error,
                 uint16_t expected_faces = 0, uint16_t recognized = 0,
                 uint16_t unknown = 0, uint16_t failed = 0);
    void finish_face_batch(const CaptureRequest& request, FaceBatchStatus status,
                           uint16_t expected_faces, uint16_t recognized = 0,
                           uint16_t unknown = 0, uint16_t failed = 0);
    bool await_result(const CaptureRequest& request, uint32_t image_id,
                      uint16_t face_index, uint16_t face_count, uint8_t& result_status,
                      protocol::ErrorCode& error);

    MetricsService metrics_{6}; BleTransport ble_{metrics_}; TriggerService triggers_;
    CameraService camera_; FaceDetectionService detector_; ReusableImageBuffers buffers_; StateMachine state_;
    QueueHandle_t rx_queue_{nullptr}; QueueHandle_t result_queue_{nullptr};
    uint32_t next_image_id_{1};
    void (*batch_observer_)(const FaceBatchResult&, void*){nullptr};
    void (*face_observer_)(const FaceAssociation&, void*){nullptr};
    void* event_observer_context_{nullptr};
    bool started_{false};
};
}  // namespace demo
