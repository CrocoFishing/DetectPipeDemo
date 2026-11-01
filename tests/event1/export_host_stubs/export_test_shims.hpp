#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "application_protocol.hpp"
using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_NO_MEM = 0x101,
    ESP_ERR_INVALID_ARG = 0x102, ESP_ERR_INVALID_STATE = 0x103,
    ESP_ERR_NOT_FOUND = 0x105, ESP_ERR_TIMEOUT = 0x107;
inline int64_t fake_now;
inline int64_t owners_finish_at;
inline bool storage_mounted, audio_running, audio_paused;
inline esp_err_t suspend_error, resume_error, barrier_error;
inline std::vector<std::string> export_calls;
inline int64_t esp_timer_get_time() { return fake_now; }
inline uint32_t pdMS_TO_TICKS(uint32_t ms) { return ms; }
inline void vTaskDelay(uint32_t ms) { fake_now += static_cast<int64_t>(ms) * 1000; }
inline bool sd_recorder_is_mounted() { return storage_mounted; }
inline bool sd_recorder_is_running() { return storage_mounted; }
inline esp_err_t sd_recorder_fatfs_drive(char*, size_t) { return ESP_OK; }
inline bool audio_pipeline_is_running() { return audio_running; }
inline esp_err_t audio_pipeline_suspend_for_export() {
    export_calls.emplace_back("suspend");
    audio_paused = true;
    audio_running = false;
    return suspend_error;
}
inline esp_err_t audio_pipeline_resume_after_export() {
    export_calls.emplace_back("resume");
    if (resume_error == ESP_OK && audio_paused) { audio_paused = false; audio_running = true; }
    return resume_error;
}
inline esp_err_t sd_recorder_flush_barrier(uint32_t) {
    export_calls.emplace_back("barrier");
    return barrier_error;
}
namespace demo {
class SystemController {
public:
    bool gated{false};
    using Handler = bool (*)(const protocol::PacketHeader&, const uint8_t*, void*);
    void set_sd_export_handlers(Handler, void*, size_t (*)(uint8_t*, size_t, void*)) {}
    void set_export_pending(bool value) { gated = value; export_calls.emplace_back(value ? "capture_gate" : "capture_open"); }
    bool capture_busy() const { return fake_now < owners_finish_at; }
    esp_err_t send_event_message(protocol::MessageType, uint32_t, const uint8_t*, size_t) { return ESP_OK; }
    esp_err_t send_sd_export_status(uint32_t, const uint8_t*, size_t) { return ESP_OK; }
};
class EventManager {
public:
    bool gated{false};
    void set_export_pending(bool value) { gated = value; export_calls.emplace_back(value ? "event_gate" : "event_open"); }
    bool export_quiescent() const { return gated && fake_now >= owners_finish_at; }
};
struct WifiSdExportHooks {
    esp_err_t (*prepare)(void*, uint32_t, bool (*)(void*), void*){nullptr};
    esp_err_t (*resume)(void*){nullptr};
    esp_err_t (*notify)(void*, uint32_t, const protocol::SdExportStatusPayload&){nullptr};
    void* context{nullptr};
    esp_err_t (*storage_drive)(void*, char*, size_t){nullptr};
};
inline WifiSdExportHooks captured_hooks;
class WifiSdExportService {
public:
    esp_err_t initialize(const WifiSdExportHooks& hooks) { captured_hooks = hooks; return ESP_OK; }
    bool submit(const protocol::PacketHeader&, const uint8_t*) { return true; }
    size_t copy_info(uint8_t*, size_t) const { return 0; }
};
}
