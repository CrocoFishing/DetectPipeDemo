#include "trigger_service.hpp"

#include "board_config.hpp"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"

namespace demo {
namespace { constexpr char TAG[] = "trigger"; }

esp_err_t TriggerService::initialize(TriggerSubmitObserver observer, void* observer_context) {
    observer_ = observer;
    observer_context_ = observer_context;
    queue_ = xQueueCreate(1, sizeof(CaptureRequest));
    ESP_RETURN_ON_FALSE(queue_, ESP_ERR_NO_MEM, TAG, "CaptureRequest queue allocation failed");
    BaseType_t ok = xTaskCreate(trigger_task_entry, "trigger_task", 3072, this, 6, &task_);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "trigger task allocation failed");
    gpio_config_t cfg{}; cfg.pin_bit_mask = 1ULL << static_cast<unsigned>(board::EXTERNAL_TRIGGER_GPIO);
    cfg.mode = GPIO_MODE_INPUT; cfg.pull_up_en = GPIO_PULLUP_ENABLE; cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_NEGEDGE; ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "GPIO config failed");
    esp_err_t err = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(board::EXTERNAL_TRIGGER_GPIO, gpio_isr, this), TAG, "ISR add failed");
    ESP_LOGI(TAG, "external trigger enabled gpio=%d debounce_ms=%lu after system initialization",
             static_cast<int>(board::EXTERNAL_TRIGGER_GPIO), static_cast<unsigned long>(board::EXTERNAL_TRIGGER_DEBOUNCE_MS));
    return ESP_OK;
}

void IRAM_ATTR TriggerService::gpio_isr(void* arg) {
    auto* self = static_cast<TriggerService*>(arg); BaseType_t wake = pdFALSE;
    vTaskNotifyGiveFromISR(self->task_, &wake); if (wake) portYIELD_FROM_ISR();
}
void TriggerService::trigger_task_entry(void* arg) { static_cast<TriggerService*>(arg)->trigger_task(); }
void TriggerService::trigger_task() {
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(board::EXTERNAL_TRIGGER_DEBOUNCE_MS));
        if (gpio_get_level(board::EXTERNAL_TRIGGER_GPIO) != board::EXTERNAL_TRIGGER_ACTIVE_LEVEL) continue;
        CaptureRequest r{0x80000000U | (generated_counter_++ & 0x7FFFFFFFU), TriggerSource::ExternalButton, esp_timer_get_time()};
        SubmitResult result = submit(r);
        ESP_LOGI(TAG, "button request_id=%lu result=%d source=EXTERNAL_BUTTON", static_cast<unsigned long>(r.request_id), static_cast<int>(result));
        if (observer_) observer_(r, result, observer_context_);
        while (gpio_get_level(board::EXTERNAL_TRIGGER_GPIO) == board::EXTERNAL_TRIGGER_ACTIVE_LEVEL)
            vTaskDelay(pdMS_TO_TICKS(board::EXTERNAL_TRIGGER_RELEASE_POLL_MS));
        ulTaskNotifyTake(pdTRUE, 0);  // discard bounce notifications accumulated during the same press
    }
}
bool TriggerService::duplicate(uint32_t id) {
    for (uint32_t old : recent_ids_) if (old == id && id != 0) return true;
    recent_ids_[recent_pos_++ % 8] = id; return false;
}
SubmitResult TriggerService::submit(const CaptureRequest& r) {
    portENTER_CRITICAL(&mux_);
    if (busy_) { portEXIT_CRITICAL(&mux_); return SubmitResult::Busy; }
    if (duplicate(r.request_id)) { portEXIT_CRITICAL(&mux_); return SubmitResult::Duplicate; }
    busy_ = true; portEXIT_CRITICAL(&mux_);
    if (xQueueSend(queue_, &r, 0) != pdTRUE) { complete(); return SubmitResult::Busy; }
    return SubmitResult::Accepted;
}
SubmitResult TriggerService::submit_ble(uint32_t id) {
    if (id == 0 || (id & 0x80000000U)) return SubmitResult::InvalidClientId;
    return submit({id, TriggerSource::BleClient, esp_timer_get_time()});
}
bool TriggerService::receive(CaptureRequest& r, TickType_t wait) { return xQueueReceive(queue_, &r, wait) == pdTRUE; }
void TriggerService::complete() { portENTER_CRITICAL(&mux_); busy_ = false; active_image_id_ = 0; active_state_ = SystemState::Idle; portEXIT_CRITICAL(&mux_); }
void TriggerService::set_active(uint32_t id, SystemState s) { portENTER_CRITICAL(&mux_); active_image_id_ = id; active_state_ = s; portEXIT_CRITICAL(&mux_); }
}  // namespace demo
