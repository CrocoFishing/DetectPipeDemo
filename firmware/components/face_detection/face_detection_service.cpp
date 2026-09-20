#include "face_detection_service.hpp"

#include <algorithm>
#include <cmath>
#include <new>
#include "board_config.hpp"
#include "dl_image_define.hpp"
#include "esp_log.h"
#include "human_face_detect.hpp"

namespace demo {
namespace { constexpr char TAG[] = "face_detection"; }

FaceDetectionService::~FaceDetectionService() { delete detector_; }
esp_err_t FaceDetectionService::initialize() {
    if (detector_) return ESP_OK;
    detector_ = new (std::nothrow) HumanFaceDetect(HumanFaceDetect::ESPDET_PICO_224_224_FACE, false);
    if (!detector_ || !detector_->get_raw_model()) { delete detector_; detector_ = nullptr; ESP_LOGE(TAG, "detector load failed"); return ESP_ERR_NOT_FOUND; }
    detector_->set_score_thr(board::FACE_CONFIDENCE_THRESHOLD);
    ESP_LOGI(TAG, "model_loaded name=%s input=%ux%u threshold=%.2f", board::DETECTOR_MODEL_NAME,
             board::DETECTOR_INPUT_WIDTH, board::DETECTOR_INPUT_HEIGHT, board::FACE_CONFIDENCE_THRESHOLD);
    return ESP_OK;
}

esp_err_t FaceDetectionService::detect(const camera_fb_t& f, std::vector<FaceBox>& out) {
    out.clear(); if (!detector_) return ESP_ERR_INVALID_STATE;
    if (!f.buf || !f.width || !f.height || f.format != PIXFORMAT_RGB565) return ESP_ERR_INVALID_ARG;
    // ESP-DL's ImagePreprocessor performs the required letterboxed resize to the model's
    // fixed 224x224 tensor and maps ESPDet results back to this source image size.
    dl::image::img_t img{f.buf, static_cast<uint16_t>(f.width), static_cast<uint16_t>(f.height), dl::image::DL_IMAGE_PIX_TYPE_RGB565BE};
    auto& detected = detector_->run(img);
    for (const auto& r : detected) {
        if (r.score < board::FACE_CONFIDENCE_THRESHOLD || r.box.size() < 4) continue;
        FaceBox b{r.box[0], r.box[1], r.box[2] + 1, r.box[3] + 1, r.score};
        b.x1 = std::clamp(b.x1, 0, static_cast<int>(f.width)); b.x2 = std::clamp(b.x2, 0, static_cast<int>(f.width));
        b.y1 = std::clamp(b.y1, 0, static_cast<int>(f.height)); b.y2 = std::clamp(b.y2, 0, static_cast<int>(f.height));
        if (b.valid()) out.push_back(b); else ESP_LOGW(TAG, "discard invalid detector box");
    }
    ESP_LOGI(TAG, "face_detection_count=%u", static_cast<unsigned>(out.size())); return ESP_OK;
}
void FaceDetectionService::sort_largest_first(std::vector<FaceBox>& results) {
    std::stable_sort(results.begin(), results.end(),
                     [](const FaceBox& a, const FaceBox& b) { return a.area() > b.area(); });
}
FaceBox FaceDetectionService::expand_and_clamp(const FaceBox& b, int fw, int fh) {
    const float dx = b.width() * board::FACE_MARGIN_PER_SIDE; const float dy = b.height() * board::FACE_MARGIN_PER_SIDE;
    FaceBox o{static_cast<int>(std::floor(b.x1 - dx)), static_cast<int>(std::floor(b.y1 - dy)),
              static_cast<int>(std::ceil(b.x2 + dx)), static_cast<int>(std::ceil(b.y2 + dy)), b.confidence};
    o.x1 = std::clamp(o.x1, 0, fw); o.y1 = std::clamp(o.y1, 0, fh); o.x2 = std::clamp(o.x2, 0, fw); o.y2 = std::clamp(o.y2, 0, fh);
    return o;
}
}  // namespace demo
