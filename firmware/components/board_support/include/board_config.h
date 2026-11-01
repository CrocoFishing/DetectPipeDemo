#pragma once
#include "driver/gpio.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    gpio_num_t pdm_data;
    gpio_num_t pdm_clk;
    gpio_num_t sd_cs;
    gpio_num_t sd_sclk;
    gpio_num_t sd_miso;
    gpio_num_t sd_mosi;
    const char *sd_profile;
} board_audio_pins_t;
const board_audio_pins_t *board_audio_pins(void);
void board_config_log(void);
#ifdef __cplusplus
}
#endif
