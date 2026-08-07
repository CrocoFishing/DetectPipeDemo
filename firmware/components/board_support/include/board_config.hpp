#pragma once

#include <cstddef>
#include <cstdint>
#include "driver/gpio.h"

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

inline constexpr uint32_t CAMERA_XCLK_HZ = 20'000'000;
inline constexpr uint16_t CAMERA_FRAME_WIDTH = 640;
inline constexpr uint16_t CAMERA_FRAME_HEIGHT = 480;
inline constexpr uint16_t DETECTOR_INPUT_WIDTH = 224;
inline constexpr uint16_t DETECTOR_INPUT_HEIGHT = 224;
inline constexpr float FACE_CONFIDENCE_THRESHOLD = 0.50F;
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
