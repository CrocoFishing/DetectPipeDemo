#pragma once

#include "esp_err.h"

namespace board {
esp_err_t validate_hardware_resources();
void log_memory(const char* phase);
void init_status_led();
void set_status_led(bool on);
}  // namespace board

