#include "audio_pipeline.h"
#include "board_config.hpp"
#include "event_manager.hpp"
#include "esp_log.h"
#include "system_controller.hpp"
#include "sd_export_integration.hpp"

extern "C" void app_main() {
    static demo::SystemController controller;
    static demo::EventManager events;
    static demo::SdExportIntegration sd_export;
    constexpr const char* tag = "event1";
    ESP_LOGI(tag, "CONFIG,app=EventPipeDemo,protocol=2,board=XIAO_ESP32S3_Sense,camera=1024x768,format=RGB565,"
             "recaptures=%u,rotate_final_180=%u,detector=%s,pdm_data=%d,pdm_clk=%d,sd_sclk=%d,sd_miso=%d,"
             "sd_mosi=%d,sd_cs=%d,trigger_gpio=%d,audio_source=RAW,sample_rate=16000,channels=1,bits=16,"
             "usb_uac=disabled,continuous_recording=disabled,psram=octal80MHz",
             board::FACE_DETECTION_MAX_RECAPTURES, board::FACE_DETECTION_ROTATE_FINAL_FRAME_180,
             board::DETECTOR_MODEL_NAME, board::PDM_DATA_GPIO, board::PDM_CLK_GPIO,
             board::SD_SCLK_GPIO, board::SD_MISO_GPIO, board::SD_MOSI_GPIO, board::SD_CS_GPIO,
             board::EXTERNAL_TRIGGER_GPIO);
    ESP_LOGI(tag, "CONFIG,camera_pwdn=%d,camera_reset=%d,camera_xclk=%d,camera_siod=%d,camera_sioc=%d,"
             "camera_d0=%d,camera_d1=%d,camera_d2=%d,camera_d3=%d,camera_d4=%d,camera_d5=%d,camera_d6=%d,camera_d7=%d,"
             "camera_vsync=%d,camera_href=%d,camera_pclk=%d,sd_profile=%s,ble=nimble,base_features=0x3f,"
             "sr_model=vadnet1_medium,vad_mode=0,preroll_ms=1000,min_noise_ms=1000",
             board::CAMERA_PWDN_GPIO, board::CAMERA_RESET_GPIO, board::CAMERA_XCLK_GPIO,
             board::CAMERA_SIOD_GPIO, board::CAMERA_SIOC_GPIO, board::CAMERA_D0_GPIO, board::CAMERA_D1_GPIO,
             board::CAMERA_D2_GPIO, board::CAMERA_D3_GPIO, board::CAMERA_D4_GPIO, board::CAMERA_D5_GPIO,
             board::CAMERA_D6_GPIO, board::CAMERA_D7_GPIO, board::CAMERA_VSYNC_GPIO, board::CAMERA_HREF_GPIO,
             board::CAMERA_PCLK_GPIO, board::SD_PROFILE);
    if (events.initialize(controller) != ESP_OK) {
        ESP_LOGE(tag, "persistent event identity unavailable; initialization stopped");
        return;
    }
    const bool camera_ok = controller.initialize() == ESP_OK && controller.start() == ESP_OK;
    if (!camera_ok) ESP_LOGE(tag, "camera/BLE initialization failed; audio Events remain enabled with face FAILED");
    if (events.start(camera_ok) != ESP_OK) {
        ESP_LOGE(tag, "EventManager task unavailable");
        return;
    }
    const esp_err_t export_error = camera_ok ? sd_export.initialize(controller, events) : ESP_FAIL;
    if (export_error != ESP_OK) ESP_LOGW(tag, "SD export worker unavailable: %s", esp_err_to_name(export_error));
    const auto config = events.audio_config();
    esp_err_t audio_error = audio_pipeline_init(&config);
    if (audio_error != ESP_OK) {
        ESP_LOGE(tag, "always-on audio initialization failed: %s", esp_err_to_name(audio_error));
        return;
    }
    audio_error = audio_pipeline_start();
    if (audio_error != ESP_OK) {
        ESP_LOGE(tag, "always-on audio start failed: %s", esp_err_to_name(audio_error));
        return;
    }
    sd_export.enable();
    ESP_LOGI(tag, "Event 1 running: one RAW WAV per Event, face_interval_ms=30000,client_no_face_recaptures=3; Wi-Fi SD management=%s",
             export_error == ESP_OK ? "available" : "unavailable");
}
