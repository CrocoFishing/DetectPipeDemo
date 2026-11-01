#pragma once

#include "event_manager.hpp"
#include "wifi_sd_export_service.hpp"

namespace demo {
// Allocate the command worker before audio initialization; expose it to BLE
// only after startup succeeds. All slow coordination runs on that worker.
class SdExportIntegration {
public:
    esp_err_t initialize(SystemController& controller, EventManager& events);
    void enable();
private:
    static esp_err_t prepare(void*, uint32_t, bool (*)(void*), void*);
    static esp_err_t resume(void*);
    static esp_err_t notify(void*, uint32_t, const protocol::SdExportStatusPayload&);
    static bool command(const protocol::PacketHeader&, const uint8_t*, void*);
    static size_t info(uint8_t*, size_t, void*);
    static esp_err_t storage_drive(void*, char*, size_t);
    SystemController* controller_{nullptr};
    EventManager* events_{nullptr};
    WifiSdExportService service_;
    bool initialized_{false};
    bool gated_{false};
};
}
