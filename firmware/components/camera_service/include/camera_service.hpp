#pragma once

#include "esp_camera.h"
#include "esp_err.h"

namespace demo {
class FrameLease {
public:
    FrameLease() = default;
    explicit FrameLease(camera_fb_t* frame) : frame_(frame) {}
    ~FrameLease();
    FrameLease(const FrameLease&) = delete;
    FrameLease& operator=(const FrameLease&) = delete;
    FrameLease(FrameLease&& other) noexcept;
    FrameLease& operator=(FrameLease&& other) noexcept;
    camera_fb_t* get() const { return frame_; }
    camera_fb_t* operator->() const { return frame_; }
    explicit operator bool() const { return frame_ != nullptr; }
    void reset();
private:
    camera_fb_t* frame_{nullptr};
};

class CameraService {
public:
    esp_err_t initialize();
    // esp32-camera 2.1.7 applies its internal bounded 4 s frame timeout.
    FrameLease capture();
    esp_err_t reinitialize();
    void shutdown();
    bool initialized() const { return initialized_; }
private:
    bool initialized_{false};
};
}  // namespace demo
