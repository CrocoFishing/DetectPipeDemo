#include "image_processing.hpp"

#include <algorithm>
#include <cstring>
#include "board_config.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "img_converters.h"

namespace demo {
namespace { constexpr char TAG[] = "image_processing"; }
ReusableImageBuffers::~ReusableImageBuffers() { heap_caps_free(crop_); heap_caps_free(jpeg_); }
esp_err_t ReusableImageBuffers::initialize() {
    if (crop_ && jpeg_) return ESP_OK;
    crop_ = static_cast<uint8_t*>(heap_caps_malloc(board::MAX_CROP_RGB565_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    jpeg_ = static_cast<uint8_t*>(heap_caps_malloc(board::MAX_JPEG_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!crop_ || !jpeg_) { ESP_LOGE(TAG, "PSRAM reusable buffer allocation failed"); return ESP_ERR_NO_MEM; }
    ESP_LOGI(TAG, "reusable PSRAM buffers crop=%u jpeg=%u", static_cast<unsigned>(board::MAX_CROP_RGB565_BYTES), static_cast<unsigned>(board::MAX_JPEG_BYTES));
    return ESP_OK;
}
esp_err_t ReusableImageBuffers::copy_crop(const camera_fb_t& src, const FaceBox& b, BufferView& out) {
    if (!crop_ || crop_owned_ || !b.valid() || b.x2 > static_cast<int>(src.width) || b.y2 > static_cast<int>(src.height) || src.format != PIXFORMAT_RGB565) return ESP_ERR_INVALID_STATE;
    const size_t row = static_cast<size_t>(b.width()) * 2U; const size_t total = row * b.height();
    if (total > board::MAX_CROP_RGB565_BYTES) return ESP_ERR_INVALID_SIZE;
    for (int y = 0; y < b.height(); ++y) std::memcpy(crop_ + static_cast<size_t>(y) * row,
        src.buf + (static_cast<size_t>(b.y1 + y) * src.width + b.x1) * 2U, row);
    crop_owned_ = true; out = {crop_, total, static_cast<uint16_t>(b.width()), static_cast<uint16_t>(b.height())}; return ESP_OK;
}
size_t ReusableImageBuffers::jpeg_callback(void* arg, size_t index, const void* data, size_t len) {
    auto* self = static_cast<ReusableImageBuffers*>(arg);
    if (index + len > board::MAX_JPEG_BYTES) { self->jpeg_overflow_ = true; return 0; }
    std::memcpy(self->jpeg_ + index, data, len); self->jpeg_size_ = std::max(self->jpeg_size_, index + len); return len;
}
esp_err_t ReusableImageBuffers::encode_jpeg(const BufferView& crop, BufferView& out) {
    if (!jpeg_ || jpeg_owned_ || !crop_owned_ || !crop.data || !crop.width || !crop.height) return ESP_ERR_INVALID_STATE;
    jpeg_size_ = 0; jpeg_overflow_ = false; jpgSetRgb565BE(true);
    bool ok = fmt2jpg_cb(crop.data, crop.size, crop.width, crop.height, PIXFORMAT_RGB565, board::JPEG_QUALITY, jpeg_callback, this);
    if (!ok || jpeg_overflow_ || jpeg_size_ == 0) { ESP_LOGE(TAG, "JPEG encode failed quality=%u", board::JPEG_QUALITY); return ESP_FAIL; }
    jpeg_owned_ = true; out = {jpeg_, jpeg_size_, crop.width, crop.height};
    ESP_LOGI(TAG, "JPEG encoded bytes=%u quality=%u", static_cast<unsigned>(jpeg_size_), board::JPEG_QUALITY); return ESP_OK;
}
void ReusableImageBuffers::release_crop() { crop_owned_ = false; }
void ReusableImageBuffers::release_jpeg() { jpeg_owned_ = false; jpeg_size_ = 0; }
}  // namespace demo
