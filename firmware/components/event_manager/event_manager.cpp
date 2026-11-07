#include "event_manager.hpp"

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <limits>
#include "application_protocol.hpp"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace demo {
namespace { constexpr char TAG[] = "event_manager"; }

esp_err_t EventManager::initialize(SystemController& controller) {
    // Do not erase NVS: the persisted boot counter is part of Event identity.
    ESP_RETURN_ON_ERROR(nvs_flash_init(), TAG, "NVS initialization; counter must be preserved");
    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open("event1", NVS_READWRITE, &handle), TAG, "open counter namespace");
    uint32_t previous = 0;
    esp_err_t error = nvs_get_u32(handle, "boot_session", &previous);
    if (error == ESP_ERR_NVS_NOT_FOUND) error = ESP_OK;
    if (error == ESP_OK && previous == std::numeric_limits<uint32_t>::max()) error = ESP_ERR_INVALID_STATE;
    if (error == ESP_OK) error = nvs_set_u32(handle, "boot_session", previous + 1);
    if (error == ESP_OK) error = nvs_commit(handle);
    nvs_close(handle);
    ESP_RETURN_ON_ERROR(error, TAG, "persistent boot counter unavailable; refusing colliding IDs");
    ledger_ = EventLedger(previous + 1);
    faces_ = xQueueCreate(16, sizeof(FaceAssociation));
    ESP_RETURN_ON_FALSE(faces_, ESP_ERR_NO_MEM, TAG, "association queue allocation");
    controller_ = &controller;
    controller.set_event_observers(on_batch, on_face, this);
    ESP_LOGI(TAG, "CONFIG,boot_session=%08" PRIX32 ",event_capacity=%u,event_face_queue=16",
             previous + 1, static_cast<unsigned>(EventLedger::Capacity));
    return ESP_OK;
}

esp_err_t EventManager::start(bool camera_available) {
    camera_available_ = camera_available;
    return xTaskCreatePinnedToCore(task_entry, "event_manager", 6144, this, 9, &task_, 1) == pdPASS
        ? ESP_OK : ESP_ERR_NO_MEM;
}

audio_pipeline_config_t EventManager::audio_config() {
    auto config = audio_pipeline_default_config();
    config.metrics_enabled = false;
    config.event_open_cb = on_open;
    config.event_end_cb = on_end;
    config.audio_terminal_cb = on_audio;
    config.callback_ctx = this;
    return config;
}

size_t EventManager::slot(const EventContext* c) { return static_cast<size_t>(c - ledger_.contexts().data()); }
void EventManager::wake() { if (task_) xTaskNotifyGive(task_); }

void EventManager::set_export_pending(bool pending) {
    portENTER_CRITICAL(&mux_);
    export_pending_ = pending;
    export_quiescent_ = false;
    portEXIT_CRITICAL(&mux_);
    wake();
}
bool EventManager::export_quiescent() {
    portENTER_CRITICAL(&mux_);
    const bool ready = export_pending_ && export_quiescent_;
    portEXIT_CRITICAL(&mux_);
    return ready;
}

uint64_t EventManager::on_open(int64_t ts, uint64_t sample, void* ctx) {
    auto& self = *static_cast<EventManager*>(ctx);
    uint64_t id = 0;
    portENTER_CRITICAL(&self.mux_);
    if (self.export_pending_) {
        portEXIT_CRITICAL(&self.mux_);
        return 0;
    }
    if (auto* c = self.ledger_.open(ts, sample)) {
        id = c->event_id;
        self.pending_[self.slot(c)] = {};
        self.pending_[self.slot(c)].dirty = OpenDirty;
    } else ++self.queue_drops_;
    portEXIT_CRITICAL(&self.mux_);
    if (!id) ESP_LOGE(TAG, "EVENT_REJECTED,reason=bounded_event_capacity_or_id_exhaustion,sample=%" PRIu64, sample);
    self.wake();
    return id;
}

void EventManager::on_end(uint64_t id, int64_t ts, uint64_t sample, void* ctx) {
    auto& self = *static_cast<EventManager*>(ctx);
    portENTER_CRITICAL(&self.mux_);
    if (auto* c = self.ledger_.find(id)) {
        const bool already_ended = c->vad_ended;
        if (!apply_vad_end(*c, id, ts, sample)) ++self.rejected_callbacks_;
        else if (!already_ended) self.pending_[self.slot(c)].dirty |= EndDirty;
    } else ++self.rejected_callbacks_;
    portEXIT_CRITICAL(&self.mux_);
    self.wake();
}

void EventManager::on_audio(const sd_event_result_t* result, void* ctx) {
    if (!result || !result->event_id) return;
    auto& self = *static_cast<EventManager*>(ctx);
    portENTER_CRITICAL(&self.mux_);
    if (auto* c = self.ledger_.find(result->event_id)) {
        auto& p = self.pending_[self.slot(c)];
        auto candidate = *c;
        if (!(p.dirty & AudioDirty) && apply_audio_terminal(candidate, result->event_id,
                result->ready ? AudioStatus::Ready : AudioStatus::Failed,
                result->audio_first_sample, result->audio_last_sample)) {
            p.audio = *result;
            p.dirty |= AudioDirty;
        } else ++self.rejected_callbacks_;
    } else ++self.rejected_callbacks_;
    portEXIT_CRITICAL(&self.mux_);
    self.wake();
}

void EventManager::on_batch(const FaceBatchResult& result, void* ctx) {
    auto& self = *static_cast<EventManager*>(ctx);
    portENTER_CRITICAL(&self.mux_);
    if (auto* c = self.ledger_.find(result.event_id)) {
        auto& p = self.pending_[self.slot(c)];
        auto candidate = *c;
        // Validate before reserving the terminal slot: stale ingress cannot hide a real result.
        if (!(p.dirty & FaceDirty) && apply_face_terminal(candidate, result)) {
            p.face = result;
            p.dirty |= FaceDirty;
        } else ++self.rejected_callbacks_;
    } else ++self.rejected_callbacks_;
    portEXIT_CRITICAL(&self.mux_);
    self.wake();
}

void EventManager::on_face(const FaceAssociation& face, void* ctx) {
    auto& self = *static_cast<EventManager*>(ctx);
    const bool queued = xQueueSend(self.faces_, &face, 0) == pdTRUE;
    portENTER_CRITICAL(&self.mux_);
    if (!queued) ++self.queue_drops_;
    const uint32_t depth = uxQueueMessagesWaiting(self.faces_);
    if (depth > self.queue_high_water_) self.queue_high_water_ = depth;
    portEXIT_CRITICAL(&self.mux_);
    if (!queued) ESP_LOGE(TAG, "ASSOCIATION_QUEUE_FULL,event_id=%016" PRIX64 ",image_id=%" PRIu32,
                         face.event_id, face.image_id);
    self.wake();
}

void EventManager::publish_open(const EventContext& c) {
    uint8_t payload[protocol::EVENT_OPEN_PAYLOAD_SIZE]{};
    protocol::encode_payload(protocol::EventOpenPayload{c.event_id, protocol::TriggerSource::Event1}, payload, sizeof(payload));
    publish_message(protocol::MessageType::EventOpen, c, payload, sizeof(payload));
}
void EventManager::publish_audio(const EventContext& c) {
    uint8_t payload[protocol::EVENT_AUDIO_READY_PAYLOAD_SIZE]{};
    protocol::encode_payload(protocol::EventAudioReadyPayload{c.event_id,
        static_cast<protocol::AudioStatus>(c.audio_status), c.audio_first_sample, c.audio_last_sample}, payload, sizeof(payload));
    publish_message(protocol::MessageType::EventAudioReady, c, payload, sizeof(payload));
    lifecycle(c, "AUDIO_READY", audio_status_name(c.audio_status));
}
void EventManager::publish_batch(const EventContext& c, const FaceBatchResult& b) {
    uint8_t payload[protocol::FACE_BATCH_END_PAYLOAD_SIZE]{};
    protocol::encode_payload(protocol::FaceBatchEndPayload{c.event_id, c.request_id,
        b.expected_faces, b.completed_faces, b.recognized_count, b.unknown_count, b.failed_count,
        static_cast<protocol::FaceStatus>(b.status)}, payload, sizeof(payload));
    publish_message(protocol::MessageType::FaceBatchEnd, c, payload, sizeof(payload));
    lifecycle(c, "FACE_BATCH_END", face_status_name(c.face_status));
    // A terminal sentinel preserves request/count/status even if no result returned.
    {
        char row[192];
        std::snprintf(row, sizeof(row), "%016" PRIX64 ",%" PRIu32 ",0,-1,%u,%s,,,0",
                      c.event_id, c.request_id, b.expected_faces, face_status_name(b.status));
        if (!sd_recorder_metadata_append(SD_METADATA_FACE_ASSOCIATIONS, row)) ++metadata_drops_;
    }
}
void EventManager::publish_complete(const EventContext& c) {
    uint8_t payload[protocol::EVENT_COMPLETE_PAYLOAD_SIZE]{};
    const bool ok = c.state == EventState::Complete;
    protocol::encode_payload(protocol::EventCompletePayload{c.event_id,
        static_cast<protocol::EventStatus>(ok ? 0 : 1), static_cast<protocol::AudioStatus>(c.audio_status),
        static_cast<protocol::FaceStatus>(c.face_status)}, payload, sizeof(payload));
    publish_message(protocol::MessageType::EventComplete, c, payload, sizeof(payload));
    lifecycle(c, "COMPLETE", ok ? "COMPLETE" : "COMPLETE_WITH_ERROR");
}
void EventManager::publish_message(protocol::MessageType type, const EventContext& c, const uint8_t* payload, size_t length) {
    const esp_err_t error = controller_->send_event_message(type, c.request_id, payload, length);
    if (error != ESP_OK) {
        ++protocol_failures_;
        ESP_LOGW(TAG, "EVENT_PROTOCOL_ERROR,event_id=%016" PRIX64 ",request_id=%" PRIu32 ",type=%s,error=%s",
                 c.event_id, c.request_id, protocol::message_type_name(type), esp_err_to_name(error));
    }
}
void EventManager::lifecycle(const EventContext& c, const char* state, const char* status) {
    char row[192];
    const int64_t ts = esp_timer_get_time();
    std::snprintf(row, sizeof(row), "%016" PRIX64 ",%s,%" PRId64 ",%s", c.event_id, state, ts, status);
    if (!sd_recorder_metadata_append(SD_METADATA_EVENT_LIFECYCLE, row)) ++metadata_drops_;
    ESP_LOGI(TAG, "EVENT,event_id=%016" PRIX64 ",request_id=%" PRIu32 ",state=%s,status=%s",
             c.event_id, c.request_id, state, status);
}
void EventManager::save_face(const FaceAssociation& f) {
    // Quote identifiers, including commas, quotes and line breaks from a peer.
    char person[70]{};
    size_t n = 0;
    person[n++] = '"';
    for (size_t i = 0; i < 32 && f.person_id[i]; ++i) {
        if (f.person_id[i] == '"') person[n++] = '"';
        person[n++] = f.person_id[i];
    }
    person[n++] = '"';
    person[n] = 0;
    char row[256];
    const char* status = f.status == 0 ? "RECOGNIZED" : f.status == 1 ? "UNKNOWN" : "FAILED";
    std::snprintf(row, sizeof(row), "%016" PRIX64 ",%" PRIu32 ",%" PRIu32 ",%u,%u,%s,%s,%.6f,%" PRIu32,
                  f.event_id, f.request_id, f.image_id, f.face_index, f.face_count, status, person,
                  static_cast<double>(f.similarity), f.processing_time_ms);
    if (!sd_recorder_metadata_append(SD_METADATA_FACE_ASSOCIATIONS, row)) ++metadata_drops_;
    ESP_LOGI(TAG, "FACE,event_id=%016" PRIX64 ",request_id=%" PRIu32 ",image_id=%" PRIu32 ",face_index=%u,face_count=%u,status=%s",
             f.event_id, f.request_id, f.image_id, f.face_index, f.face_count, status);
}

void EventManager::task_entry(void* ctx) { static_cast<EventManager*>(ctx)->run(); }
void EventManager::run() {
    int64_t metrics_at = 0;
    while (true) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
        FaceAssociation face;
        while (xQueueReceive(faces_, &face, 0) == pdTRUE) save_face(face);
        for (size_t i = 0; i < EventLedger::Capacity; ++i) {
            EventContext c;
            Pending p;
            portENTER_CRITICAL(&mux_);
            c = ledger_.contexts()[i];
            p = pending_[i];
            pending_[i].dirty = 0;
            portEXIT_CRITICAL(&mux_);
            if (!c.event_id || !p.dirty) continue;
            if (p.dirty & OpenDirty) {
                FaceBatchStatus immediate = FaceBatchStatus::Pending;
                if (!camera_available_) immediate = FaceBatchStatus::Failed;
                else if (!controller_->event_client_ready()) immediate = FaceBatchStatus::Disconnected;
                else {
                    uint32_t request_id = 0;
                    auto submitted = controller_->submit_event_capture(c.event_id, request_id);
                    if (submitted == SubmitResult::Accepted) c.request_id = request_id;
                    else immediate = submitted == SubmitResult::Busy ? FaceBatchStatus::Busy : FaceBatchStatus::Failed;
                }
                portENTER_CRITICAL(&mux_);
                ledger_.contexts()[i].request_id = c.request_id;
                portEXIT_CRITICAL(&mux_);
                publish_open(c);
                lifecycle(c, "OPEN", "OK");
                if (immediate != FaceBatchStatus::Pending) {
                    p.face = {c.event_id, c.request_id, 0, 0, 0, 0, 0, immediate};
                    p.dirty |= FaceDirty;
                }
            }
            if (p.dirty & AudioDirty) {
                // Revalidate consumption after publish may have yielded to another callback.
                if (apply_audio_terminal(c, p.audio.event_id,
                        p.audio.ready ? AudioStatus::Ready : AudioStatus::Failed,
                        p.audio.audio_first_sample, p.audio.audio_last_sample)) {
                    // Writer owns audio_events.csv and RAW WAV; it reports actual boundaries.
                    publish_audio(c);
                } else {
                    portENTER_CRITICAL(&mux_);
                    ++rejected_callbacks_;
                    portEXIT_CRITICAL(&mux_);
                }
            }
            if (p.dirty & FaceDirty) {
                if (apply_face_terminal(c, p.face)) publish_batch(c, p.face);
                else {
                    portENTER_CRITICAL(&mux_);
                    ++rejected_callbacks_;
                    portEXIT_CRITICAL(&mux_);
                    ESP_LOGE(TAG, "STALE_BATCH,event_id=%016" PRIX64 ",request_id=%" PRIu32, p.face.event_id, p.face.request_id);
                }
            }
            const bool complete = refresh_event_state(c, p.face.failed_count);
            // Merge terminal state without overwriting a concurrently received VAD_END.
            portENTER_CRITICAL(&mux_);
            auto& stored = ledger_.contexts()[i];
            stored.request_id = c.request_id;
            stored.audio_status = c.audio_status;
            stored.audio_first_sample = c.audio_first_sample;
            stored.audio_last_sample = c.audio_last_sample;
            stored.face_status = c.face_status;
            stored.expected_face_count = c.expected_face_count;
            stored.completed_face_count = c.completed_face_count;
            if (complete) stored.state = c.state;
            c = stored;
            portEXIT_CRITICAL(&mux_);
            if (complete) {
                publish_complete(c);
                portENTER_CRITICAL(&mux_);
                ledger_.release(c.event_id);
                pending_[i] = {};
                portEXIT_CRITICAL(&mux_);
            }
        }
        // Acknowledge only after the sole consumer enqueued its last metadata.
        // Polling active_count from another task could miss this final write.
        portENTER_CRITICAL(&mux_);
        bool dirty = false;
        for (const auto& pending : pending_) dirty = dirty || pending.dirty != 0;
        export_quiescent_ = export_pending_ && ledger_.active_count() == 0 &&
            !dirty && uxQueueMessagesWaiting(faces_) == 0;
        portEXIT_CRITICAL(&mux_);
        const int64_t now = esp_timer_get_time();
        if (now - metrics_at >= 5'000'000) {
            metrics_at = now;
            portENTER_CRITICAL(&mux_);
            const size_t active = ledger_.active_count(), high = ledger_.high_water();
            const uint32_t depth = queue_high_water_, drops = queue_drops_, rejected = rejected_callbacks_;
            portEXIT_CRITICAL(&mux_);
            ESP_LOGI(TAG, "RESOURCE,open_events=%u,event_high_water=%u,event_queue_high_water=%" PRIu32
                     ",event_queue_drops=%" PRIu32 ",metadata_drops=%" PRIu32 ",rejected_callbacks=%" PRIu32
                     ",event_protocol_failures=%" PRIu32
                     ",internal_free=%u,internal_min=%u,psram_free=%u,psram_min=%u",
                     static_cast<unsigned>(active), static_cast<unsigned>(high), depth, drops, metadata_drops_, rejected, protocol_failures_,
                     static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                     static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)),
                     static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
                     static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM)));
            audio_pipeline_print_status();
        }
    }
}
} // namespace demo
