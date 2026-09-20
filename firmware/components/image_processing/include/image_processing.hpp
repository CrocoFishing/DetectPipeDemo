#pragma once

#include <cstddef>
#include <cstdint>
#include "esp_camera.h"
#include "esp_err.h"
#include "face_detection_service.hpp"

namespace demo {
struct BufferView { uint8_t* data{nullptr}; size_t size{0}; uint16_t width{0}; uint16_t height{0}; };

// Rotates a packed RGB565 camera frame in-place without allocating another frame buffer.
esp_err_t rotate_rgb565_180(camera_fb_t& frame);

class ReusableImageBuffers {
public:
    ~ReusableImageBuffers();
    esp_err_t initialize();
    esp_err_t copy_crop(const camera_fb_t& source, const FaceBox& box, BufferView& crop);
    esp_err_t encode_jpeg(const BufferView& crop, BufferView& jpeg);
    void release_crop();
    void release_jpeg();
private:
    static size_t jpeg_callback(void* arg, size_t index, const void* data, size_t length);
    uint8_t* crop_{nullptr}; uint8_t* jpeg_{nullptr};
    size_t jpeg_size_{0}; bool crop_owned_{false}; bool jpeg_owned_{false}; bool jpeg_overflow_{false};
};
}  // namespace demo
