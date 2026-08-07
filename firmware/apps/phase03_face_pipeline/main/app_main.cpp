#include <algorithm>
#include <vector>
#include "application_protocol.hpp"
#include "board_support.hpp"
#include "camera_service.hpp"
#include "esp_log.h"
#include "face_detection_service.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "image_processing.hpp"
#include "mbedtls/base64.h"

namespace {
void emit_jpeg_base64(const uint8_t* jpeg, size_t length) {
    constexpr size_t INPUT_CHUNK = 144;  // divisible by 3; produces 192 Base64 characters
    unsigned char encoded[193]{};
    unsigned index = 0;
    for (size_t offset = 0; offset < length; offset += INPUT_CHUNK, ++index) {
        const size_t input_length = std::min(INPUT_CHUNK, length - offset);
        size_t output_length = 0;
        if (mbedtls_base64_encode(encoded, sizeof(encoded), &output_length,
                                  jpeg + offset, input_length) != 0) {
            ESP_LOGE("phase03", "[TEST] phase=3 result=FAIL reason=base64_encode");
            return;
        }
        encoded[output_length] = '\0';
        ESP_LOGI("phase03", "[JPEG_B64] index=%u data=%s", index,
                 reinterpret_cast<const char*>(encoded));
    }
    ESP_LOGI("phase03", "[JPEG_B64_END] chunks=%u bytes=%u", index,
             static_cast<unsigned>(length));
}
}  // namespace

extern "C" void app_main() {
    demo::CameraService camera; demo::FaceDetectionService detector; demo::ReusableImageBuffers buffers;
    if (buffers.initialize()!=ESP_OK || camera.initialize()!=ESP_OK || detector.initialize()!=ESP_OK) { ESP_LOGE("phase03", "[TEST] phase=3 result=FAIL reason=init"); return; }
    auto frame = camera.capture();
    if (!frame) return;
    std::vector<demo::FaceBox> faces;
    if (detector.detect(*frame.get(), faces)!=ESP_OK) return;
    demo::FaceBox largest{};
    if (!demo::FaceDetectionService::select_largest(faces, largest)) { frame.reset(); ESP_LOGI("phase03", "[TEST] phase=3 result=NO_FACE safe_idle=true"); }
    else { auto expanded=demo::FaceDetectionService::expand_and_clamp(largest, frame->width, frame->height); demo::BufferView crop{}, jpeg{};
        ESP_LOGI("phase03", "largest=(%d,%d,%d,%d) expanded=(%d,%d,%d,%d) faces=%u", largest.x1,largest.y1,largest.x2,largest.y2,expanded.x1,expanded.y1,expanded.x2,expanded.y2,static_cast<unsigned>(faces.size()));
        if (buffers.copy_crop(*frame.get(),expanded,crop)!=ESP_OK) return;
        frame.reset();
        if (buffers.encode_jpeg(crop,jpeg)!=ESP_OK) return;
        uint32_t crc=protocol::crc32(jpeg.data,jpeg.size);
        ESP_LOGI("phase03", "[METRIC] phase=3 image_id=1 metric=jpeg_bytes value=%u unit=bytes",static_cast<unsigned>(jpeg.size));
        ESP_LOGI("phase03", "[TEST] phase=3 result=JPEG_READY crc32=%08lx width=%u height=%u",static_cast<unsigned long>(crc),jpeg.width,jpeg.height);
        emit_jpeg_base64(jpeg.data, jpeg.size);
        buffers.release_crop(); buffers.release_jpeg(); }
    board::log_memory("phase03_complete"); while(true) vTaskDelay(pdMS_TO_TICKS(1000));
}
