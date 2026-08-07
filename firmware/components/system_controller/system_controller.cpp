#include "system_controller.hpp"

#include <algorithm>
#include <cstring>
#include <vector>
#include "board_config.hpp"
#include "board_support.hpp"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/task.h"

namespace demo {
namespace {
constexpr char TAG[] = "system_controller";
uint16_t read16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8)); }
float read_float(const uint8_t* p) { float v; std::memcpy(&v, p, sizeof(v)); return v; }
}

SystemController::SystemController() = default;
esp_err_t SystemController::initialize() {
    board::init_status_led(); ESP_RETURN_ON_ERROR(board::validate_hardware_resources(), TAG, "hardware validation failed");
    rx_queue_ = xQueueCreate(8, sizeof(RxPacket)); result_queue_ = xQueueCreate(2, sizeof(RxPacket));
    ESP_RETURN_ON_FALSE(rx_queue_ && result_queue_, ESP_ERR_NO_MEM, TAG, "protocol queues unavailable");
    ESP_RETURN_ON_ERROR(buffers_.initialize(), TAG, "image buffers unavailable");
    ESP_RETURN_ON_ERROR(camera_.initialize(), TAG, "camera initialization failed");
    ESP_RETURN_ON_ERROR(detector_.initialize(), TAG, "detector load failed");
    ESP_RETURN_ON_ERROR(ble_.initialize(rx_control, rx_result, this), TAG, "BLE initialization failed");
    ESP_RETURN_ON_ERROR(triggers_.initialize(on_button_submission, this), TAG, "trigger initialization failed");
    state_.transition(SystemState::Idle, 0, TriggerSource::ExternalButton);
    ESP_LOGI(TAG, "all modules initialized; GPIO0 remains reserved and untouched"); return ESP_OK;
}
void SystemController::start() {
    xTaskCreatePinnedToCore(pipeline_task_entry, "camera_pipeline_task", 8192, this, 7, nullptr, 1);
    xTaskCreate(ble_event_task_entry, "ble_event_task", 4096, this, 6, nullptr);
    xTaskCreate(metrics_task_entry, "metrics_task", 3072, this, 2, nullptr);
}
void SystemController::rx_control(const uint8_t* d, size_t n, void* c) { static_cast<SystemController*>(c)->enqueue_rx(d, n); }
void SystemController::rx_result(const uint8_t* d, size_t n, void* c) { static_cast<SystemController*>(c)->enqueue_rx(d, n); }
void SystemController::on_button_submission(const CaptureRequest& r, SubmitResult result, void* context) {
    auto* self = static_cast<SystemController*>(context);
    if (!self->ble_.connected() || !self->ble_.event_subscribed()) return;
    if (result == SubmitResult::Accepted) {
        self->ble_.send_event(protocol::MessageType::CaptureAccepted, r.request_id, 0);
    } else if (result == SubmitResult::Busy) {
        self->ble_.send_event(protocol::MessageType::Busy, r.request_id,
                              self->triggers_.active_image_id(), nullptr, 0, 0,
                              static_cast<uint32_t>(self->triggers_.active_state()));
    } else {
        const uint16_t error = static_cast<uint16_t>(protocol::ErrorCode::DuplicateRequest);
        const uint8_t payload[2]{static_cast<uint8_t>(error), static_cast<uint8_t>(error >> 8)};
        self->ble_.send_event(protocol::MessageType::Error, r.request_id, 0, payload, sizeof(payload));
    }
}
void SystemController::enqueue_rx(const uint8_t* d, size_t n) {
    if (!d || n > sizeof(RxPacket::data)) return;
    RxPacket p{};
    p.length = static_cast<uint16_t>(n);
    std::memcpy(p.data, d, n);
    if (xQueueSend(rx_queue_, &p, 0) != pdTRUE) metrics_.emit(0, "queue_congestion_count", 1, "count");
}
void SystemController::pipeline_task_entry(void* a) { static_cast<SystemController*>(a)->pipeline_task(); }
void SystemController::ble_event_task_entry(void* a) { static_cast<SystemController*>(a)->ble_event_task(); }
void SystemController::metrics_task_entry(void* a) { static_cast<SystemController*>(a)->metrics_task(); }
void SystemController::transition(SystemState s, const CaptureRequest& r, uint32_t image) { state_.transition(s, image, r.trigger_source); triggers_.set_active(image, s); }

void SystemController::recover(const CaptureRequest& r, uint32_t image, protocol::ErrorCode error) {
    transition(SystemState::ErrorRecovery, r, image); uint8_t payload[2]{static_cast<uint8_t>(error), static_cast<uint8_t>(static_cast<uint16_t>(error) >> 8)};
    if (ble_.connected() && ble_.event_subscribed()) ble_.send_event(protocol::MessageType::Error, r.request_id, image, payload, sizeof(payload));
    buffers_.release_crop(); buffers_.release_jpeg();
    if (error == protocol::ErrorCode::CaptureFailed || error == protocol::ErrorCode::CameraInit) camera_.reinitialize();
    transition(SystemState::Idle, r, image); triggers_.complete(); board::set_status_led(false);
}

void SystemController::pipeline_task() {
    esp_task_wdt_add(nullptr);
    while (true) {
        CaptureRequest r{}; if (!triggers_.receive(r, pdMS_TO_TICKS(1000))) { esp_task_wdt_reset(); continue; }
        const uint32_t image = next_image_id_++; board::set_status_led(true); transition(SystemState::Capturing, r, image);
        auto frame = camera_.capture(); if (!frame) { recover(r, image, protocol::ErrorCode::CaptureFailed); continue; }
        transition(SystemState::Detecting, r, image); std::vector<FaceBox> faces;
        if (detector_.detect(*frame.get(), faces) != ESP_OK) { frame.reset(); recover(r, image, protocol::ErrorCode::DetectorLoad); continue; }
        FaceBox largest{};
        if (!FaceDetectionService::select_largest(faces, largest)) {
            frame.reset(); transition(SystemState::NoFace, r, image);
            if (ble_.connected() && ble_.event_subscribed()) ble_.send_event(protocol::MessageType::NoFace, r.request_id, image);
            transition(SystemState::Idle, r, image); triggers_.complete(); board::set_status_led(false); continue;
        }
        FaceBox expanded = FaceDetectionService::expand_and_clamp(largest, frame->width, frame->height);
        if (!expanded.valid()) { frame.reset(); recover(r, image, protocol::ErrorCode::InvalidBoundingBox); continue; }
        transition(SystemState::Cropping, r, image); BufferView crop{};
        if (buffers_.copy_crop(*frame.get(), expanded, crop) != ESP_OK) { frame.reset(); recover(r, image, protocol::ErrorCode::CropAllocation); continue; }
        frame.reset(); transition(SystemState::Encoding, r, image); BufferView jpeg{};
        if (buffers_.encode_jpeg(crop, jpeg) != ESP_OK) { buffers_.release_crop(); recover(r, image, protocol::ErrorCode::JpegEncode); continue; }
        buffers_.release_crop(); transition(SystemState::Transmitting, r, image);
        esp_err_t tx = ble_.send_image(r.request_id, image, jpeg.data, jpeg.size); buffers_.release_jpeg();
        if (tx != ESP_OK) { recover(r, image, ble_.connected() ? protocol::ErrorCode::BleCongestion : protocol::ErrorCode::TransferDisconnected); continue; }
        transition(SystemState::WaitingResult, r, image);
        if (!await_result(r, image)) { recover(r, image, protocol::ErrorCode::RecognitionTimeout); continue; }
        transition(SystemState::Completed, r, image); transition(SystemState::Idle, r, image);
        triggers_.complete(); board::set_status_led(false); board::log_memory("request_complete"); esp_task_wdt_reset();
    }
}

bool SystemController::await_result(const CaptureRequest& r, uint32_t image) {
    const int64_t deadline = esp_timer_get_time() + 15'000'000;
    while (esp_timer_get_time() < deadline) {
        RxPacket p{}; TickType_t wait = pdMS_TO_TICKS(std::min<int64_t>(1000, (deadline - esp_timer_get_time()) / 1000));
        if (xQueueReceive(result_queue_, &p, wait) != pdTRUE) continue;
        protocol::PacketHeader h{}; const uint8_t* payload{};
        if (protocol::decode_packet(p.data, p.length, h, payload) != protocol::DecodeStatus::Ok || h.message_type != protocol::MessageType::RecognitionResult) continue;
        if (h.request_id != r.request_id || h.image_id != image) { ESP_LOGW(TAG, "stale recognition result request=%lu image=%lu", static_cast<unsigned long>(h.request_id), static_cast<unsigned long>(h.image_id)); continue; }
        if (h.payload_length < 12) return false;
        const uint8_t status = payload[0], person_id_len = payload[1], name_len = payload[2];
        if (12U + person_id_len + name_len > h.payload_length) return false;
        char person_id[33]{}, name[65]{}; std::memcpy(person_id, payload + 12, std::min<size_t>(person_id_len, 32));
        std::memcpy(name, payload + 12 + person_id_len, std::min<size_t>(name_len, 64));
        const float similarity = read_float(payload + 4); uint32_t processing_ms = 0; std::memcpy(&processing_ms, payload + 8, 4);
        ESP_LOGI(TAG, "recognition_result status=%u person_id=%s name=%s similarity=%.4f processing_ms=%lu",
                 status, person_id, board::LOG_PERSON_NAME ? name : "<redacted>", similarity, static_cast<unsigned long>(processing_ms)); return true;
    }
    return false;
}

void SystemController::ble_event_task() {
    while (true) {
        RxPacket p{}; if (xQueueReceive(rx_queue_, &p, portMAX_DELAY) != pdTRUE) continue;
        protocol::PacketHeader h{}; const uint8_t* payload{}; auto status = protocol::decode_packet(p.data, p.length, h, payload);
        if (status != protocol::DecodeStatus::Ok) { ESP_LOGW(TAG, "invalid BLE packet status=%d", static_cast<int>(status)); continue; }
        if (h.message_type == protocol::MessageType::RecognitionResult) { if (xQueueSend(result_queue_, &p, 0) != pdTRUE) metrics_.emit(h.image_id, "queue_congestion_count", 1, "count"); continue; }
        if (h.message_type == protocol::MessageType::Ping) { ble_.send_event(protocol::MessageType::Pong, h.request_id, h.image_id); continue; }
        if (h.message_type != protocol::MessageType::StartCapture || h.payload_length < 8 || read16(payload) != static_cast<uint16_t>(protocol::Command::StartCapture)) continue;
        SubmitResult result = triggers_.submit_ble(h.request_id);
        if (result == SubmitResult::Accepted) ble_.send_event(protocol::MessageType::CaptureAccepted, h.request_id, 0);
        else if (result == SubmitResult::Busy) ble_.send_event(protocol::MessageType::Busy, h.request_id, triggers_.active_image_id(), nullptr, 0, 0, static_cast<uint32_t>(triggers_.active_state()));
        else { uint16_t e = static_cast<uint16_t>(result == SubmitResult::Duplicate ? protocol::ErrorCode::DuplicateRequest : protocol::ErrorCode::InvalidPacket);
            uint8_t ep[2]{static_cast<uint8_t>(e), static_cast<uint8_t>(e >> 8)}; ble_.send_event(protocol::MessageType::Error, h.request_id, 0, ep, sizeof(ep)); }
    }
}
void SystemController::metrics_task() {
    while (true) { vTaskDelay(pdMS_TO_TICKS(10000)); const auto& c = ble_.counters();
        ESP_LOGI(TAG, "BLE quality mtu=%u disconnect=%lu reconnect=%lu success=%lu failure=%lu retry=%lu congestion=%lu",
            ble_.negotiated_mtu(), static_cast<unsigned long>(c.disconnect_count), static_cast<unsigned long>(c.reconnect_count),
            static_cast<unsigned long>(c.transfer_success), static_cast<unsigned long>(c.transfer_failure),
            static_cast<unsigned long>(c.retry_count), static_cast<unsigned long>(c.queue_congestion_count)); }
}
}  // namespace demo
