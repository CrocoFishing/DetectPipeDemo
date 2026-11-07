#include "board_config.h"
#include "board_config.hpp"
#include <cstdio>

extern "C" const board_audio_pins_t *board_audio_pins(void) {
    static const board_audio_pins_t pins = {
        board::PDM_DATA_GPIO, board::PDM_CLK_GPIO, board::SD_CS_GPIO,
        board::SD_SCLK_GPIO, board::SD_MISO_GPIO, board::SD_MOSI_GPIO,
        board::SD_PROFILE,
    };
    return &pins;
}
extern "C" void board_config_log(void) {
    std::printf("CONFIG,board,pdm_data,%d,pdm_clk,%d,sd_profile,%s,sd_cs,%d,"
                "sd_sclk,%d,sd_miso,%d,sd_mosi,%d,trigger,%d,camera_width,%u,"
                "camera_height,%u,camera_format,RGB565,psram,OCT,ble,NimBLE,"
                "vad_model,vadnet1_medium\n",
                board::PDM_DATA_GPIO, board::PDM_CLK_GPIO, board::SD_PROFILE,
                board::SD_CS_GPIO, board::SD_SCLK_GPIO, board::SD_MISO_GPIO,
                board::SD_MOSI_GPIO, board::EXTERNAL_TRIGGER_GPIO,
                board::CAMERA_FRAME_WIDTH, board::CAMERA_FRAME_HEIGHT);
}
