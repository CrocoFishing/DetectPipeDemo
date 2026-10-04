#include "system_controller.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
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
SubmitResult SystemController::submit_event_capture(uint64_t event_id, uint32_t& request_id) {
    if (!started_) { request_id = 0; return SubmitResult::Busy; }
    return triggers_.submit_event(event_id, request_id);
}
void SystemController::set_event_observers(void (*batch)(const FaceBatchResult&, void*),
                                          void (*face)(const FaceAssociation&, void*), void* context) {
    // Install before start(); callbacks only enqueue bounded metadata and never perform I/O.
    batch_observer_ = batch;
    face_observer_ = face;
    event_observer_context_ = context;
    ble_.set_event_association_enabled(batch != nullptr);
}
esp_err_t SystemController::send_event_message(protocol::MessageType type, uint32_t request_id,
                                              const uint8_t* payload, size_t length) {
    return ble_.send_event(type, request_id, 0, payload, length);
}
bool SystemController::event_client_ready() const {
    if (!ble_.connected() || !ble_.event_subscribed() || !ble_.image_subscribed()) return false;
    constexpr size_t required_mtu = protocol::HEADER_SIZE + protocol::FACE_BATCH_END_PAYLOAD_SIZE + 3;
    if (ble_.negotiated_mtu() < required_mtu) {
        ESP_LOGW(TAG, "EVENT_CLIENT_NOT_READY,reason=protocol_mtu,mtu=%u,required=%u",
                 ble_.negotiated_mtu(), static_cast<unsigned>(required_mtu));
        return false;
    }
    return true;
}
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
esp_err_t SystemController::start() {
    if (started_ || !rx_queue_ || !result_queue_) return ESP_ERR_INVALID_STATE;
    TaskHandle_t ble_task = nullptr, metrics_task = nullptr;
    if (xTaskCreate(ble_event_task_entry, "ble_event_task", 4096, this, 6, &ble_task) != pdPASS) {
        ESP_LOGE(TAG, "BLE event task allocation failed"); return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(metrics_task_entry, "metrics_task", 3072, this, 2, &metrics_task) != pdPASS) {
        vTaskDelete(ble_task);
        ESP_LOGE(TAG, "metrics task allocation failed"); return ESP_ERR_NO_MEM;
    }
    if (xTaskCreatePinnedToCore(pipeline_task_entry, "camera_pipeline_task", 8192, this, 7, nullptr, 1) != pdPASS) {
        vTaskDelete(ble_task); vTaskDelete(metrics_task);
        ESP_LOGE(TAG, "camera task allocation failed"); return ESP_ERR_NO_MEM;
    }
    started_ = true;
    return ESP_OK;
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

void SystemController::finish_face_batch(const CaptureRequest& r, FaceBatchStatus status,
                                        uint16_t expected, uint16_t recognized,
                                        uint16_t unknown, uint16_t failed) {
    if (!r.has_event) return;
    FaceBatchResult result{};
    result.event_id = r.event_id; result.request_id = r.request_id;
    result.expected_faces = expected;
    result.completed_faces = recognized + unknown + failed;
    result.recognized_count = recognized; result.unknown_count = unknown; result.failed_count = failed;
    result.status = status;
    ESP_LOGI(TAG, "FACE_BATCH_END,event_id=%016llX,request_id=%lu,expected=%u,completed=%u,status=%u",
             static_cast<unsigned long long>(r.event_id), static_cast<unsigned long>(r.request_id),
             static_cast<unsigned>(expected), static_cast<unsigned>(result.completed_faces),
             static_cast<unsigned>(status));
    if (batch_observer_) batch_observer_(result, event_observer_context_);
}
void SystemController::recover(const CaptureRequest& r, uint32_t image, protocol::ErrorCode error,
                               uint16_t expected, uint16_t recognized, uint16_t unknown, uint16_t failed) {
    transition(SystemState::ErrorRecovery, r, image); uint8_t payload[2]{static_cast<uint8_t>(error), static_cast<uint8_t>(static_cast<uint16_t>(error) >> 8)};
    if (ble_.connected() && ble_.event_subscribed()) ble_.send_event(protocol::MessageType::Error, r.request_id, image, payload, sizeof(payload));
    buffers_.release_crop(); buffers_.release_jpeg();
    if (error == protocol::ErrorCode::CaptureFailed || error == protocol::ErrorCode::CameraInit) camera_.reinitialize();
    FaceBatchStatus batch_status = FaceBatchStatus::Failed;
    if (recognized || unknown || failed) batch_status = FaceBatchStatus::Partial;
    else if (error == protocol::ErrorCode::TransferDisconnected || error == protocol::ErrorCode::BleNotConnected)
        batch_status = FaceBatchStatus::Disconnected;
    else if (error == protocol::ErrorCode::RecognitionTimeout || error == protocol::ErrorCode::ImageTimeout)
        batch_status = FaceBatchStatus::Timeout;
    finish_face_batch(r, batch_status, expected, recognized, unknown, failed);
    transition(SystemState::Idle, r, image); triggers_.complete(); board::set_status_led(false);
}

void SystemController::pipeline_task() {
    esp_task_wdt_add(nullptr);
    while (true) {
        CaptureRequest r{}; if (!triggers_.receive(r, pdMS_TO_TICKS(1000))) { esp_task_wdt_reset(); continue; }
        if (r.has_event) {
            // Publish the association from the pipeline task before any image packet.
            // The EventManager may publish the same OPEN; clients accept it idempotently.
            uint8_t event_open[protocol::EVENT_OPEN_PAYLOAD_SIZE]{};
            const protocol::EventOpenPayload open{r.event_id, protocol::TriggerSource::Event1};
            protocol::encode_payload(open, event_open, sizeof(event_open));
            const esp_err_t association_tx = send_event_message(protocol::MessageType::EventOpen,
                                                               r.request_id, event_open, sizeof(event_open));
            if (association_tx != ESP_OK) {
                recover(r, 0, ble_.connected() ? protocol::ErrorCode::BleCongestion
                                               : protocol::ErrorCode::TransferDisconnected);
                esp_task_wdt_reset();
                continue;
            }
        }
        const uint32_t image = next_image_id_++;
        const uint32_t capture_attempts = 1U + static_cast<uint32_t>(board::FACE_DETECTION_MAX_RECAPTURES);
        board::set_status_led(true);

        FrameLease frame;
        std::vector<FaceBox> faces;
        protocol::ErrorCode attempt_error = protocol::ErrorCode::None;
        for (uint32_t attempt = 1; attempt <= capture_attempts; ++attempt) {
            transition(SystemState::Capturing, r, image);
            frame = camera_.capture();
            if (!frame) {
                attempt_error = protocol::ErrorCode::CaptureFailed;
                break;
            }
            esp_task_wdt_reset();

            transition(SystemState::Detecting, r, image);
            if (detector_.detect(*frame.get(), faces) != ESP_OK) {
                attempt_error = protocol::ErrorCode::DetectorLoad;
                break;
            }
            esp_task_wdt_reset();
            ESP_LOGI(TAG, "detection_attempt capture=%lu/%lu rotated=false faces=%u",
                     static_cast<unsigned long>(attempt), static_cast<unsigned long>(capture_attempts),
                     static_cast<unsigned>(faces.size()));
            if (!faces.empty()) break;

            if (attempt < capture_attempts) {
                ESP_LOGI(TAG, "no_face recapture_next=%lu/%lu",
                         static_cast<unsigned long>(attempt + 1U), static_cast<unsigned long>(capture_attempts));
                frame.reset();
                continue;
            }

            if (board::FACE_DETECTION_ROTATE_FINAL_FRAME_180) {
                if (rotate_rgb565_180(*frame.get()) != ESP_OK) {
                    attempt_error = protocol::ErrorCode::Internal;
                    break;
                }
                esp_task_wdt_reset();
                if (detector_.detect(*frame.get(), faces) != ESP_OK) {
                    attempt_error = protocol::ErrorCode::DetectorLoad;
                    break;
                }
                esp_task_wdt_reset();
                ESP_LOGI(TAG, "detection_attempt capture=%lu/%lu rotated=true faces=%u",
                         static_cast<unsigned long>(attempt), static_cast<unsigned long>(capture_attempts),
                         static_cast<unsigned>(faces.size()));
            }
        }

        if (attempt_error != protocol::ErrorCode::None) {
            frame.reset();
            recover(r, image, attempt_error);
            continue;
        }

        if (faces.empty()) {
            frame.reset(); transition(SystemState::NoFace, r, image);
            if (ble_.connected() && ble_.event_subscribed()) ble_.send_event(protocol::MessageType::NoFace, r.request_id, image);
            finish_face_batch(r, FaceBatchStatus::NoFace, 0);
            transition(SystemState::Idle, r, image); triggers_.complete(); board::set_status_led(false); continue;
        }
        if (faces.size() > std::numeric_limits<uint16_t>::max()) {
            frame.reset(); recover(r, image, protocol::ErrorCode::Internal); continue;
        }

        FaceDetectionService::sort_largest_first(faces);
        const uint16_t face_count = static_cast<uint16_t>(faces.size());
        uint16_t ok_count = 0, unknown_count = 0, failed_count = 0;
        uint32_t current_image = image;
        bool batch_failed = false;

        for (uint16_t face_index = 0; face_index < face_count; ++face_index) {
            if (face_index) current_image = next_image_id_++;
            const FaceBox& face = faces[face_index];
            const FaceBox expanded = FaceDetectionService::expand_and_clamp(
                face, frame->width, frame->height);
            ESP_LOGI(TAG,
                     "face_selected rank=%u/%u image=%lu box=(%d,%d,%d,%d) area=%d "
                     "confidence=%.4f expanded=(%d,%d,%d,%d)",
                     static_cast<unsigned>(face_index + 1U), static_cast<unsigned>(face_count),
                     static_cast<unsigned long>(current_image), face.x1, face.y1, face.x2, face.y2,
                     face.area(), face.confidence, expanded.x1, expanded.y1, expanded.x2, expanded.y2);
            if (!expanded.valid()) {
                frame.reset(); recover(r, current_image, protocol::ErrorCode::InvalidBoundingBox,
                                       face_count, ok_count, unknown_count, failed_count);
                batch_failed = true; break;
            }

            transition(SystemState::Cropping, r, current_image);
            BufferView crop{};
            if (buffers_.copy_crop(*frame.get(), expanded, crop) != ESP_OK) {
                frame.reset(); recover(r, current_image, protocol::ErrorCode::CropAllocation,
                                       face_count, ok_count, unknown_count, failed_count);
                batch_failed = true; break;
            }
            if (face_index + 1U == face_count) frame.reset();

            transition(SystemState::Encoding, r, current_image);
            BufferView jpeg{};
            if (buffers_.encode_jpeg(crop, jpeg) != ESP_OK) {
                frame.reset(); recover(r, current_image, protocol::ErrorCode::JpegEncode,
                                       face_count, ok_count, unknown_count, failed_count);
                batch_failed = true; break;
            }
            buffers_.release_crop();

            transition(SystemState::Transmitting, r, current_image);
            const esp_err_t tx = ble_.send_image(r.request_id, current_image, jpeg.data, jpeg.size,
                                                 face_index, face_count);
            buffers_.release_jpeg();
            if (tx != ESP_OK) {
                frame.reset();
                recover(r, current_image, !ble_.connected() ? protocol::ErrorCode::TransferDisconnected
                                            : tx == ESP_ERR_TIMEOUT ? protocol::ErrorCode::ImageTimeout
                                                                     : protocol::ErrorCode::BleCongestion,
                        face_count, ok_count, unknown_count, failed_count);
                batch_failed = true; break;
            }

            transition(SystemState::WaitingResult, r, current_image);
            uint8_t result_status = static_cast<uint8_t>(protocol::StatusCode::Failed);
            protocol::ErrorCode result_error = protocol::ErrorCode::RecognitionTimeout;
            if (!await_result(r, current_image, face_index, face_count, result_status, result_error)) {
                frame.reset(); recover(r, current_image, result_error,
                                       face_count, ok_count, unknown_count, failed_count);
                batch_failed = true; break;
            }
            if (result_status == static_cast<uint8_t>(protocol::StatusCode::Ok)) ++ok_count;
            else if (result_status == static_cast<uint8_t>(protocol::StatusCode::Unknown)) ++unknown_count;
            else ++failed_count;
            esp_task_wdt_reset();
        }

        if (batch_failed) continue;
        frame.reset();
        ESP_LOGI(TAG, "face_batch_complete request=%lu faces=%u ok=%lu unknown=%lu failed=%lu",
                 static_cast<unsigned long>(r.request_id), static_cast<unsigned>(face_count),
                 static_cast<unsigned long>(ok_count), static_cast<unsigned long>(unknown_count),
                 static_cast<unsigned long>(failed_count));
        const FaceBatchStatus batch_status = failed_count == 0 ? FaceBatchStatus::Completed
            : (ok_count || unknown_count) ? FaceBatchStatus::Partial : FaceBatchStatus::Failed;
        finish_face_batch(r, batch_status, face_count, ok_count, unknown_count, failed_count);
        transition(SystemState::Completed, r, current_image); transition(SystemState::Idle, r, current_image);
        triggers_.complete(); board::set_status_led(false); board::log_memory("request_complete"); esp_task_wdt_reset();
    }
}

bool SystemController::await_result(const CaptureRequest& r, uint32_t image,
                                    uint16_t face_index, uint16_t face_count, uint8_t& result_status,
                                    protocol::ErrorCode& error) {
    const int64_t deadline = esp_timer_get_time() + 15'000'000;
    while (esp_timer_get_time() < deadline) {
        if (!ble_.connected()) { error = protocol::ErrorCode::TransferDisconnected; return false; }
        const int64_t remaining_us = deadline - esp_timer_get_time();
        if (remaining_us <= 0) break;
        RxPacket p{};
        const TickType_t wait = pdMS_TO_TICKS(std::max<int64_t>(1, std::min<int64_t>(1000, remaining_us / 1000)));
        if (xQueueReceive(result_queue_, &p, wait) != pdTRUE) { esp_task_wdt_reset(); continue; }
        protocol::PacketHeader h{}; const uint8_t* payload{};
        if (protocol::decode_packet(p.data, p.length, h, payload) != protocol::DecodeStatus::Ok || h.message_type != protocol::MessageType::RecognitionResult) {
            esp_task_wdt_reset(); continue;
        }
        if (h.request_id != r.request_id || h.image_id != image) {
            ESP_LOGW(TAG, "stale recognition result request=%lu image=%lu", static_cast<unsigned long>(h.request_id), static_cast<unsigned long>(h.image_id));
            esp_task_wdt_reset(); continue;
        }
        if (h.payload_length < 12) { error = protocol::ErrorCode::InvalidPacket; return false; }
        const uint8_t status = payload[0], person_id_len = payload[1], name_len = payload[2];
        if (status > static_cast<uint8_t>(protocol::StatusCode::Failed) || person_id_len > 32 || name_len > 64 ||
            12U + person_id_len + name_len != h.payload_length) {
            error = protocol::ErrorCode::InvalidPacket; return false;
        }
        char person_id[33]{}, name[65]{}; std::memcpy(person_id, payload + 12, std::min<size_t>(person_id_len, 32));
        std::memcpy(name, payload + 12 + person_id_len, std::min<size_t>(name_len, 64));
        const float similarity = read_float(payload + 4); uint32_t processing_ms = 0; std::memcpy(&processing_ms, payload + 8, 4);
        if (!std::isfinite(similarity)) { error = protocol::ErrorCode::InvalidPacket; return false; }
        ESP_LOGI(TAG, "recognition_result face=%u/%u image=%lu status=%u person_id=%s name=%s "
                      "similarity=%.4f processing_ms=%lu",
                 static_cast<unsigned>(face_index + 1U), static_cast<unsigned>(face_count),
                 static_cast<unsigned long>(image), status, person_id,
                 board::LOG_PERSON_NAME ? name : "<redacted>", similarity,
                 static_cast<unsigned long>(processing_ms));
        if (r.has_event && face_observer_) {
            FaceAssociation association{};
            association.event_id = r.event_id; association.request_id = r.request_id; association.image_id = image;
            association.face_index = face_index; association.face_count = face_count;
            association.status = status; std::memcpy(association.person_id, person_id, sizeof(person_id));
            association.similarity = similarity; association.processing_time_ms = processing_ms;
            face_observer_(association, event_observer_context_);
        }
        result_status = status; esp_task_wdt_reset(); return true;
    }
    error = protocol::ErrorCode::RecognitionTimeout;
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
