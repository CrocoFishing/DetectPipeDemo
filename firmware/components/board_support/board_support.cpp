#include "board_support.hpp"

#include "board_config.hpp"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_psram.h"

namespace board {
namespace { constexpr char TAG[] = "board_support"; }

esp_err_t validate_hardware_resources() {
    if (!esp_psram_is_initialized()) {
        ESP_LOGE(TAG, "PSRAM unavailable; image pipeline cannot start");
        return ESP_ERR_NOT_SUPPORTED;
    }
    ESP_LOGI(TAG, "PSRAM initialized size=%u", static_cast<unsigned>(esp_psram_get_size()));
    log_memory("startup");
    return ESP_OK;
}

void log_memory(const char* phase) {
    ESP_LOGI(TAG,
             "heap phase=%s internal_free=%u internal_largest=%u psram_free=%u psram_largest=%u",
             phase,
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
}

void init_status_led() {
    if (!STATUS_LED_AVAILABLE) {
        ESP_LOGW(TAG, "status LED disabled: selected SD pin profile owns GPIO21");
        return;
    }
    gpio_config_t cfg{};
    cfg.pin_bit_mask = 1ULL << static_cast<unsigned>(STATUS_LED_GPIO);
    cfg.mode = GPIO_MODE_OUTPUT;
    ESP_ERROR_CHECK(gpio_config(&cfg));
    set_status_led(false);
}

void set_status_led(bool on) {
    if (!STATUS_LED_AVAILABLE) return;
    gpio_set_level(STATUS_LED_GPIO, on ? STATUS_LED_ACTIVE_LEVEL : !STATUS_LED_ACTIVE_LEVEL);
}
}  // namespace board

