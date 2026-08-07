#include "esp_log.h"
#include "system_controller.hpp"
extern "C" void app_main(){static demo::SystemController controller;if(controller.initialize()!=ESP_OK){ESP_LOGE("phase06","fatal initialization failure");return;}controller.start();}
