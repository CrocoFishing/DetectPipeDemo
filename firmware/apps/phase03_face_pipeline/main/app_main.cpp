#include <algorithm>
#include <vector>
#include "application_protocol.hpp"
#include "board_config.hpp"
#include "board_support.hpp"
#include "camera_service.hpp"
#include "esp_log.h"
#include "face_detection_service.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "image_processing.hpp"
#include "mbedtls/base64.h"

namespace {
void emit_jpeg_base64(const uint8_t* jpeg, size_t length, size_t face_index, size_t face_count) {
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
        ESP_LOGI("phase03", "[JPEG_B64] face=%u/%u index=%u data=%s",
                 static_cast<unsigned>(face_index + 1U), static_cast<unsigned>(face_count), index,
                 reinterpret_cast<const char*>(encoded));
    }
    ESP_LOGI("phase03", "[JPEG_B64_END] face=%u/%u chunks=%u bytes=%u",
             static_cast<unsigned>(face_index + 1U), static_cast<unsigned>(face_count), index,
             static_cast<unsigned>(length));
}
}  // namespace

extern "C" void app_main() {
    demo::CameraService camera; demo::FaceDetectionService detector; demo::ReusableImageBuffers buffers;
    if (buffers.initialize()!=ESP_OK || camera.initialize()!=ESP_OK || detector.initialize()!=ESP_OK) { ESP_LOGE("phase03", "[TEST] phase=3 result=FAIL reason=init"); return; }
    const uint32_t capture_attempts = 1U + static_cast<uint32_t>(board::FACE_DETECTION_MAX_RECAPTURES);
    demo::FrameLease frame;
    std::vector<demo::FaceBox> faces;
    for (uint32_t attempt = 1; attempt <= capture_attempts; ++attempt) {
        frame = camera.capture();
        if (!frame) { ESP_LOGE("phase03", "[TEST] phase=3 result=FAIL reason=capture"); return; }
        if (detector.detect(*frame.get(), faces)!=ESP_OK) { ESP_LOGE("phase03", "[TEST] phase=3 result=FAIL reason=detect"); return; }
        ESP_LOGI("phase03", "detection_attempt capture=%lu/%lu rotated=false faces=%u",
                 static_cast<unsigned long>(attempt), static_cast<unsigned long>(capture_attempts),
                 static_cast<unsigned>(faces.size()));
        if (!faces.empty()) break;
        if (attempt < capture_attempts) {
            ESP_LOGI("phase03", "no_face recapture_next=%lu/%lu",
                     static_cast<unsigned long>(attempt + 1U), static_cast<unsigned long>(capture_attempts));
            frame.reset();
            continue;
        }
        if (board::FACE_DETECTION_ROTATE_FINAL_FRAME_180) {
            if (demo::rotate_rgb565_180(*frame.get()) != ESP_OK) { ESP_LOGE("phase03", "[TEST] phase=3 result=FAIL reason=rotate"); return; }
            if (detector.detect(*frame.get(), faces)!=ESP_OK) { ESP_LOGE("phase03", "[TEST] phase=3 result=FAIL reason=detect_rotated"); return; }
            ESP_LOGI("phase03", "detection_attempt capture=%lu/%lu rotated=true faces=%u",
                     static_cast<unsigned long>(attempt), static_cast<unsigned long>(capture_attempts),
                     static_cast<unsigned>(faces.size()));
        }
    }
    if (faces.empty()) { frame.reset(); ESP_LOGI("phase03", "[TEST] phase=3 result=NO_FACE safe_idle=true"); }
    else {
        demo::FaceDetectionService::sort_largest_first(faces);
        for (size_t face_index = 0; face_index < faces.size(); ++face_index) {
            const auto& face = faces[face_index];
            auto expanded=demo::FaceDetectionService::expand_and_clamp(face, frame->width, frame->height);
            demo::BufferView crop{}, jpeg{};
            ESP_LOGI("phase03", "face_selected rank=%u/%u box=(%d,%d,%d,%d) area=%d confidence=%.4f expanded=(%d,%d,%d,%d)",
                     static_cast<unsigned>(face_index + 1U), static_cast<unsigned>(faces.size()),
                     face.x1,face.y1,face.x2,face.y2,face.area(),face.confidence,
                     expanded.x1,expanded.y1,expanded.x2,expanded.y2);
            if (!expanded.valid() || buffers.copy_crop(*frame.get(),expanded,crop)!=ESP_OK) return;
            if (face_index + 1U == faces.size()) frame.reset();
            if (buffers.encode_jpeg(crop,jpeg)!=ESP_OK) return;
            buffers.release_crop();
            uint32_t crc=protocol::crc32(jpeg.data,jpeg.size);
            ESP_LOGI("phase03", "[METRIC] phase=3 image_id=%u metric=jpeg_bytes value=%u unit=bytes",
                     static_cast<unsigned>(face_index + 1U), static_cast<unsigned>(jpeg.size));
            ESP_LOGI("phase03", "[TEST] phase=3 result=JPEG_READY face=%u/%u crc32=%08lx width=%u height=%u",
                     static_cast<unsigned>(face_index + 1U), static_cast<unsigned>(faces.size()),
                     static_cast<unsigned long>(crc),jpeg.width,jpeg.height);
            emit_jpeg_base64(jpeg.data, jpeg.size, face_index, faces.size());
            buffers.release_jpeg();
        }
    }
    board::log_memory("phase03_complete"); while(true) vTaskDelay(pdMS_TO_TICKS(1000));
}
