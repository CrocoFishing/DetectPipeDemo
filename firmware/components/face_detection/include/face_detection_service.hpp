#pragma once

#include <cstdint>
#include <vector>
#include "esp_camera.h"
#include "esp_err.h"

class HumanFaceDetect;

namespace demo {
struct FaceBox {
    int x1{0}; int y1{0}; int x2{0}; int y2{0};  // half-open [x1,x2) x [y1,y2)
    float confidence{0};
    int width() const { return x2 - x1; }
    int height() const { return y2 - y1; }
    int area() const { return width() * height(); }
    bool valid() const { return x1 >= 0 && y1 >= 0 && x2 > x1 && y2 > y1; }
};

class FaceDetectionService {
public:
    ~FaceDetectionService();
    esp_err_t initialize();
    esp_err_t detect(const camera_fb_t& frame, std::vector<FaceBox>& results);
    static bool select_largest(const std::vector<FaceBox>& results, FaceBox& largest);
    static FaceBox expand_and_clamp(const FaceBox& box, int frame_width, int frame_height);
    bool loaded() const { return detector_ != nullptr; }
private:
    HumanFaceDetect* detector_{nullptr};
};
}  // namespace demo

