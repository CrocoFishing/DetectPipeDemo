#pragma once

#include "audio_pipeline.h"
#include "event_ledger.hpp"
#include "system_controller.hpp"
#include "freertos/task.h"

namespace demo {
class EventManager {
public:
    esp_err_t initialize(SystemController& controller);
    esp_err_t start(bool camera_available);
    audio_pipeline_config_t audio_config();
    void set_export_pending(bool pending);
    bool export_quiescent();
private:
    enum Dirty : uint8_t { OpenDirty = 1, AudioDirty = 2, FaceDirty = 4, EndDirty = 8 };
    struct Pending {
        uint8_t dirty{0};
        sd_event_result_t audio{};
        FaceBatchResult face{};
    };
    static uint64_t on_open(int64_t timestamp_us, uint64_t sample, void* ctx);
    static void on_end(uint64_t id, int64_t timestamp_us, uint64_t sample, void* ctx);
    static void on_audio(const sd_event_result_t* result, void* ctx);
    static void on_batch(const FaceBatchResult& result, void* ctx);
    static void on_face(const FaceAssociation& face, void* ctx);
    static void task_entry(void* ctx);
    void run();
    void publish_open(const EventContext& c);
    void publish_audio(const EventContext& c);
    void publish_batch(const EventContext& c, const FaceBatchResult& batch);
    void publish_complete(const EventContext& c);
    void publish_message(protocol::MessageType type, const EventContext& c, const uint8_t* payload, size_t length);
    void lifecycle(const EventContext& c, const char* state, const char* status);
    void save_face(const FaceAssociation& face);
    void wake();
    size_t slot(const EventContext* c);
    SystemController* controller_{nullptr};
    EventLedger ledger_{};
    Pending pending_[EventLedger::Capacity]{};
    portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
    TaskHandle_t task_{nullptr};
    QueueHandle_t faces_{nullptr};
    bool camera_available_{false};
    bool export_pending_{false}, export_quiescent_{false};
    uint32_t queue_high_water_{0}, queue_drops_{0}, metadata_drops_{0}, rejected_callbacks_{0};
    uint32_t protocol_failures_{0};
};
} // namespace demo
