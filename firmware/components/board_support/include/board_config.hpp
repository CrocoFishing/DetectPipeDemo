#pragma once

#include <cstddef>
#include <cstdint>
#include "driver/gpio.h"
#include "sdkconfig.h"

namespace board {

// Seeed Studio XIAO ESP32-S3 Sense / OV3660. No pin number may be duplicated
// in service implementations; this file is the hardware single source of truth.
inline constexpr gpio_num_t CAMERA_PWDN_GPIO = GPIO_NUM_NC;
inline constexpr gpio_num_t CAMERA_RESET_GPIO = GPIO_NUM_NC;
inline constexpr gpio_num_t CAMERA_XCLK_GPIO = GPIO_NUM_10;
inline constexpr gpio_num_t CAMERA_SIOD_GPIO = GPIO_NUM_40;
inline constexpr gpio_num_t CAMERA_SIOC_GPIO = GPIO_NUM_39;
inline constexpr gpio_num_t CAMERA_D7_GPIO = GPIO_NUM_48;
inline constexpr gpio_num_t CAMERA_D6_GPIO = GPIO_NUM_11;
inline constexpr gpio_num_t CAMERA_D5_GPIO = GPIO_NUM_12;
inline constexpr gpio_num_t CAMERA_D4_GPIO = GPIO_NUM_14;
inline constexpr gpio_num_t CAMERA_D3_GPIO = GPIO_NUM_16;
inline constexpr gpio_num_t CAMERA_D2_GPIO = GPIO_NUM_18;
inline constexpr gpio_num_t CAMERA_D1_GPIO = GPIO_NUM_17;
inline constexpr gpio_num_t CAMERA_D0_GPIO = GPIO_NUM_15;
inline constexpr gpio_num_t CAMERA_VSYNC_GPIO = GPIO_NUM_38;
inline constexpr gpio_num_t CAMERA_HREF_GPIO = GPIO_NUM_47;
inline constexpr gpio_num_t CAMERA_PCLK_GPIO = GPIO_NUM_13;

inline constexpr gpio_num_t EXTERNAL_TRIGGER_GPIO = GPIO_NUM_2;  // XIAO D1
inline constexpr int EXTERNAL_TRIGGER_ACTIVE_LEVEL = 0;
inline constexpr uint32_t EXTERNAL_TRIGGER_DEBOUNCE_MS = 40;
inline constexpr uint32_t EXTERNAL_TRIGGER_RELEASE_POLL_MS = 10;
inline constexpr gpio_num_t BOOT_GPIO = GPIO_NUM_0;  // Reserved; never configured by the app.
inline constexpr gpio_num_t STATUS_LED_GPIO = GPIO_NUM_21;
inline constexpr int STATUS_LED_ACTIVE_LEVEL = 0;

inline constexpr gpio_num_t PDM_DATA_GPIO = GPIO_NUM_41;
inline constexpr gpio_num_t PDM_CLK_GPIO = GPIO_NUM_42;
#if CONFIG_BOARD_SD_PROFILE_LEGACY
inline constexpr gpio_num_t SD_CS_GPIO = GPIO_NUM_21;
inline constexpr char SD_PROFILE[] = "legacy-gpio21";
#elif CONFIG_BOARD_SD_PROFILE_CUSTOM
inline constexpr gpio_num_t SD_CS_GPIO = static_cast<gpio_num_t>(CONFIG_BOARD_SD_CUSTOM_CS_GPIO);
inline constexpr char SD_PROFILE[] = "custom";
#else
inline constexpr gpio_num_t SD_CS_GPIO = GPIO_NUM_3;
inline constexpr char SD_PROFILE[] = "current-gpio3";
#endif
#if CONFIG_BOARD_SD_PROFILE_CUSTOM
inline constexpr gpio_num_t SD_SCLK_GPIO = static_cast<gpio_num_t>(CONFIG_BOARD_SD_CUSTOM_SCLK_GPIO);
inline constexpr gpio_num_t SD_MISO_GPIO = static_cast<gpio_num_t>(CONFIG_BOARD_SD_CUSTOM_MISO_GPIO);
inline constexpr gpio_num_t SD_MOSI_GPIO = static_cast<gpio_num_t>(CONFIG_BOARD_SD_CUSTOM_MOSI_GPIO);
#else
inline constexpr gpio_num_t SD_SCLK_GPIO = GPIO_NUM_7;
inline constexpr gpio_num_t SD_MISO_GPIO = GPIO_NUM_8;
inline constexpr gpio_num_t SD_MOSI_GPIO = GPIO_NUM_9;
#endif
inline constexpr bool STATUS_LED_AVAILABLE = SD_CS_GPIO != STATUS_LED_GPIO &&
    SD_SCLK_GPIO != STATUS_LED_GPIO && SD_MISO_GPIO != STATUS_LED_GPIO &&
    SD_MOSI_GPIO != STATUS_LED_GPIO;

inline constexpr uint32_t CAMERA_XCLK_HZ = 20'000'000;
inline constexpr uint16_t CAMERA_FRAME_WIDTH = 1024;
inline constexpr uint16_t CAMERA_FRAME_HEIGHT = 768;
inline constexpr uint16_t DETECTOR_INPUT_WIDTH = 224;
inline constexpr uint16_t DETECTOR_INPUT_HEIGHT = 224;
inline constexpr float FACE_CONFIDENCE_THRESHOLD = 0.50F;
// Number of additional captures after the initial no-face result.
inline constexpr uint8_t FACE_DETECTION_MAX_RECAPTURES = 2;
// After all normal-orientation captures miss, retry the final frame in-place at 180 degrees.
inline constexpr bool FACE_DETECTION_ROTATE_FINAL_FRAME_180 = true;
inline constexpr float FACE_TOTAL_MARGIN = 0.4F;
inline constexpr float FACE_MARGIN_PER_SIDE = FACE_TOTAL_MARGIN / 2.0F;
inline constexpr uint8_t JPEG_QUALITY = 85;
inline constexpr size_t MAX_JPEG_BYTES = 160U * 1024U;
inline constexpr size_t MAX_CROP_RGB565_BYTES =
    static_cast<size_t>(CAMERA_FRAME_WIDTH) * CAMERA_FRAME_HEIGHT * 2U;

inline constexpr char DEVICE_NAME[] = "ESP32S3-FACE-DEMO";
inline constexpr char FIRMWARE_VERSION[] = "0.1.0-demo";
inline constexpr char BUILD_ID[] = __DATE__ "-" __TIME__;
inline constexpr char DETECTOR_MODEL_NAME[] = "espdet_pico_224_224_face_s8_s3";

// Demo security policy. Production must set all three to true and provision
// authenticated peers; see docs/PRIVACY.md.
inline constexpr bool BLE_REQUIRE_PAIRING = false;
inline constexpr bool BLE_REQUIRE_ENCRYPTION = false;
inline constexpr bool BLE_REQUIRE_AUTHENTICATED_RESULT = false;
inline constexpr bool LOG_PERSON_NAME = true;

}  // namespace board
