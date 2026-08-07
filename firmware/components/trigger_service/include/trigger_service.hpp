#pragma once

#include <cstdint>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "pipeline_types.hpp"

namespace demo {
enum class SubmitResult { Accepted, Busy, Duplicate, InvalidClientId };
using TriggerSubmitObserver = void (*)(const CaptureRequest& request, SubmitResult result, void* context);

class TriggerService {
public:
    esp_err_t initialize(TriggerSubmitObserver observer = nullptr, void* observer_context = nullptr);
    bool receive(CaptureRequest& request, TickType_t wait);
    SubmitResult submit_ble(uint32_t request_id);
    void complete();
    void set_active(uint32_t image_id, SystemState state);
    bool busy() const { return busy_; }
    uint32_t active_image_id() const { return active_image_id_; }
    SystemState active_state() const { return active_state_; }
private:
    static void IRAM_ATTR gpio_isr(void* arg);
    static void trigger_task_entry(void* arg);
    void trigger_task();
    SubmitResult submit(const CaptureRequest& request);
    bool duplicate(uint32_t id);
    QueueHandle_t queue_{nullptr};
    TaskHandle_t task_{nullptr};
    portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
    volatile bool busy_{false};
    volatile uint32_t active_image_id_{0};
    volatile SystemState active_state_{SystemState::Idle};
    uint32_t generated_counter_{1};
    uint32_t recent_ids_[8]{};
    size_t recent_pos_{0};
    TriggerSubmitObserver observer_{nullptr};
    void* observer_context_{nullptr};
};
}  // namespace demo
