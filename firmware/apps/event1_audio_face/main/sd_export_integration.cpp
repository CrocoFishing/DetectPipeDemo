#include "sd_export_integration.hpp"

#include "audio_pipeline.h"
#include "esp_timer.h"
#include "sd_recorder.h"

namespace demo {
esp_err_t SdExportIntegration::initialize(SystemController& controller, EventManager& events) {
    controller_ = &controller;
    events_ = &events;
    WifiSdExportHooks hooks{};
    hooks.prepare = prepare;
    hooks.resume = resume;
    hooks.notify = notify;
    hooks.context = this;
    hooks.storage_drive = storage_drive;
    const esp_err_t error = service_.initialize(hooks);
    initialized_ = error == ESP_OK;
    return error;
}
void SdExportIntegration::enable() {
    if (initialized_) controller_->set_sd_export_handlers(command, this, info);
}
bool SdExportIntegration::command(const protocol::PacketHeader& header, const uint8_t* payload, void* ctx) {
    return static_cast<SdExportIntegration*>(ctx)->service_.submit(header, payload);
}
size_t SdExportIntegration::info(uint8_t* destination, size_t capacity, void* ctx) {
    return static_cast<SdExportIntegration*>(ctx)->service_.copy_info(destination, capacity);
}
esp_err_t SdExportIntegration::storage_drive(void*, char* destination, size_t capacity) {
    return sd_recorder_fatfs_drive(destination, capacity);
}
esp_err_t SdExportIntegration::notify(void* ctx, uint32_t request,
                                     const protocol::SdExportStatusPayload& status) {
    auto& self = *static_cast<SdExportIntegration*>(ctx);
    uint8_t payload[protocol::SD_EXPORT_STATUS_PAYLOAD_SIZE]{};
    if (!protocol::encode_payload(status, payload, sizeof(payload))) return ESP_ERR_INVALID_ARG;
    return self.controller_->send_sd_export_status(request, payload, sizeof(payload));
}
esp_err_t SdExportIntegration::prepare(void* ctx, uint32_t timeout_ms,
                                      bool (*cancelled)(void*), void* cancel_ctx) {
    auto& self = *static_cast<SdExportIntegration*>(ctx);
    if (!sd_recorder_is_mounted() || !sd_recorder_is_running()) return ESP_ERR_NOT_FOUND;
    if (!audio_pipeline_is_running()) return ESP_ERR_INVALID_STATE;
    self.gated_ = true;
    self.controller_->set_export_pending(true);
    self.events_->set_export_pending(true);
    const int64_t deadline = esp_timer_get_time() + static_cast<int64_t>(timeout_ms) * 1000;
    while (!self.events_->export_quiescent() || self.controller_->capture_busy()) {
        if (cancelled && cancelled(cancel_ctx)) return ESP_FAIL;
        if (esp_timer_get_time() >= deadline) return ESP_ERR_TIMEOUT;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (cancelled && cancelled(cancel_ctx)) return ESP_FAIL;
    // Unlike stop(), this keeps the writer alive until metadata is drained.
    esp_err_t error = audio_pipeline_suspend_for_export();
    if (error != ESP_OK) return error;
    return sd_recorder_flush_barrier(3000);
}
esp_err_t SdExportIntegration::resume(void* ctx) {
    auto& self = *static_cast<SdExportIntegration*>(ctx);
    const esp_err_t error = audio_pipeline_resume_after_export();
    if (error != ESP_OK) return error;
    if (self.gated_) {
        self.events_->set_export_pending(false);
        self.controller_->set_export_pending(false);
        self.gated_ = false;
    }
    return ESP_OK;
}
}
