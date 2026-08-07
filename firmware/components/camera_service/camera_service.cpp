#include "camera_service.hpp"

#include "board_config.hpp"
#include "board_support.hpp"
#include "esp_check.h"
#include "esp_log.h"

namespace demo {
namespace { constexpr char TAG[] = "camera_service"; }

FrameLease::~FrameLease() { reset(); }
FrameLease::FrameLease(FrameLease&& other) noexcept : frame_(other.frame_) { other.frame_ = nullptr; }
FrameLease& FrameLease::operator=(FrameLease&& other) noexcept {
    if (this != &other) { reset(); frame_ = other.frame_; other.frame_ = nullptr; } return *this;
}
void FrameLease::reset() { if (frame_) { esp_camera_fb_return(frame_); frame_ = nullptr; } }

esp_err_t CameraService::initialize() {
    if (initialized_) return ESP_OK;
    ESP_RETURN_ON_ERROR(board::validate_hardware_resources(), TAG, "PSRAM check failed");
    camera_config_t c{};
    c.pin_pwdn = board::CAMERA_PWDN_GPIO; c.pin_reset = board::CAMERA_RESET_GPIO;
    c.pin_xclk = board::CAMERA_XCLK_GPIO; c.pin_sccb_sda = board::CAMERA_SIOD_GPIO; c.pin_sccb_scl = board::CAMERA_SIOC_GPIO;
    c.pin_d7 = board::CAMERA_D7_GPIO; c.pin_d6 = board::CAMERA_D6_GPIO; c.pin_d5 = board::CAMERA_D5_GPIO; c.pin_d4 = board::CAMERA_D4_GPIO;
    c.pin_d3 = board::CAMERA_D3_GPIO; c.pin_d2 = board::CAMERA_D2_GPIO; c.pin_d1 = board::CAMERA_D1_GPIO; c.pin_d0 = board::CAMERA_D0_GPIO;
    c.pin_vsync = board::CAMERA_VSYNC_GPIO; c.pin_href = board::CAMERA_HREF_GPIO; c.pin_pclk = board::CAMERA_PCLK_GPIO;
    c.xclk_freq_hz = board::CAMERA_XCLK_HZ; c.ledc_timer = LEDC_TIMER_0; c.ledc_channel = LEDC_CHANNEL_0;
    c.pixel_format = PIXFORMAT_RGB565; c.frame_size = FRAMESIZE_VGA; c.jpeg_quality = 12;
    c.fb_count = 2; c.fb_location = CAMERA_FB_IN_PSRAM; c.grab_mode = CAMERA_GRAB_LATEST; c.sccb_i2c_port = -1;
    esp_err_t err = esp_camera_init(&c);
    ESP_RETURN_ON_ERROR(err, TAG, "OV3660 initialization failed");
    sensor_t* sensor = esp_camera_sensor_get();
    if (!sensor || sensor->id.PID != OV3660_PID) { esp_camera_deinit(); return ESP_ERR_NOT_FOUND; }
    initialized_ = true;
    ESP_LOGI(TAG, "OV3660 initialized RGB565 VGA fb_count=2 PSRAM");
    return ESP_OK;
}

FrameLease CameraService::capture() {
    if (!initialized_) return {};
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) { ESP_LOGE(TAG, "frame capture failed"); return {}; }
    ESP_LOGI(TAG, "frame width=%u height=%u bytes=%u format=%d timestamp=%ld.%06ld",
             static_cast<unsigned>(fb->width), static_cast<unsigned>(fb->height), static_cast<unsigned>(fb->len),
             static_cast<int>(fb->format), static_cast<long>(fb->timestamp.tv_sec), static_cast<long>(fb->timestamp.tv_usec));
    return FrameLease(fb);
}
void CameraService::shutdown() { if (initialized_) { esp_camera_deinit(); initialized_ = false; } }
esp_err_t CameraService::reinitialize() { shutdown(); return initialize(); }
}  // namespace demo
