#pragma once

#include <cstddef>
#include <cstdint>
#include "application_protocol.hpp"
#include "esp_err.h"

namespace demo {
struct WifiSdExportHooks {
    esp_err_t (*prepare)(void*, uint32_t timeout_ms, bool (*cancelled)(void*), void* cancel_ctx){nullptr};
    esp_err_t (*resume)(void*){nullptr};
    esp_err_t (*notify)(void*, uint32_t request_id, const protocol::SdExportStatusPayload&){nullptr};
    void* context{nullptr};
    esp_err_t (*storage_drive)(void*, char* output, size_t capacity){nullptr};
};

// Initialize before audio reserves internal RAM. No Wi-Fi or filesystem work is
// performed until START. Prepare/resume execute on the export worker; a status
// query receives a bounded snapshot reply directly on the BLE control task.
class WifiSdExportService {
public:
    esp_err_t initialize(const WifiSdExportHooks& hooks);
    bool submit(const protocol::PacketHeader& header, const uint8_t* payload);
    // JSON bytes, without trailing NUL; zero unless READY/TRANSFERRING. Reading
    // this immutable session information does not extend the inactivity timer.
    size_t copy_info(uint8_t* output, size_t capacity) const;
private:
    struct State;
    State* state_{nullptr};
};
}  // namespace demo
