#pragma once

#include <cstddef>
#include <cstdint>
#include "application_protocol.hpp"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "metrics_service.hpp"

struct ble_gap_event;
struct ble_gatt_access_ctxt;

namespace demo {
inline constexpr char BLE_SERVICE_UUID[] = "7f510000-b7b2-4f6a-9f3a-6c8a2d7e1000";
inline constexpr char BLE_CONTROL_UUID[] = "7f510001-b7b2-4f6a-9f3a-6c8a2d7e1000";
inline constexpr char BLE_EVENT_UUID[] = "7f510002-b7b2-4f6a-9f3a-6c8a2d7e1000";
inline constexpr char BLE_IMAGE_UUID[] = "7f510003-b7b2-4f6a-9f3a-6c8a2d7e1000";
inline constexpr char BLE_RESULT_UUID[] = "7f510004-b7b2-4f6a-9f3a-6c8a2d7e1000";
inline constexpr char BLE_DEVICE_INFO_UUID[] = "7f510005-b7b2-4f6a-9f3a-6c8a2d7e1000";

using BlePacketHandler = void (*)(const uint8_t* data, size_t length, void* context);

struct BleQualityCounters {
    uint32_t disconnect_count{0}; uint32_t reconnect_count{0}; uint32_t retry_count{0};
    uint32_t queue_congestion_count{0}; uint32_t transfer_success{0}; uint32_t transfer_failure{0};
};

class BleTransport {
public:
    explicit BleTransport(MetricsService& metrics) : metrics_(metrics) {}
    esp_err_t initialize(BlePacketHandler control_handler, BlePacketHandler result_handler, void* context);
    esp_err_t send_event(protocol::MessageType type, uint32_t request_id, uint32_t image_id,
                         const uint8_t* payload = nullptr, size_t payload_length = 0,
                         uint16_t flags = 0, uint32_t reserved = 0);
    esp_err_t send_image(uint32_t request_id, uint32_t image_id, const uint8_t* jpeg, size_t length,
                         uint16_t face_index = 0, uint16_t face_count = 0);
    bool connected() const { return connected_; }
    void set_event_association_enabled(bool enabled) { event_association_enabled_ = enabled; }
    bool event_subscribed() const { return event_subscribed_; }
    bool image_subscribed() const { return image_subscribed_; }
    uint16_t negotiated_mtu() const { return negotiated_mtu_; }
    size_t chunk_payload_size() const;
    const BleQualityCounters& counters() const { return counters_; }

    static int gap_event(ble_gap_event* event, void* arg);
    static int gatt_access(uint16_t conn_handle, uint16_t attr_handle, ble_gatt_access_ctxt* ctxt, void* arg);
    static void on_sync();
    static void host_task(void* arg);
private:
    esp_err_t advertise();
    esp_err_t notify(uint16_t value_handle, const uint8_t* data, size_t length);
    esp_err_t make_and_notify(uint16_t handle, protocol::PacketHeader header, const uint8_t* payload);
    void sample_rssi(uint32_t image_id, int64_t now_us);
    MetricsService& metrics_;
    BlePacketHandler control_handler_{nullptr}; BlePacketHandler result_handler_{nullptr}; void* handler_context_{nullptr};
    volatile bool connected_{false}; volatile bool event_subscribed_{false}; volatile bool image_subscribed_{false};
    volatile bool transferring_{false}; uint16_t connection_handle_{0xffff}; uint16_t negotiated_mtu_{23};
    bool event_association_enabled_{false}; // configured before host startup
    int64_t last_rssi_us_{0}; BleQualityCounters counters_{}; uint8_t last_event_[517]{}; size_t last_event_length_{0};
    SemaphoreHandle_t event_mutex_{nullptr};
    portMUX_TYPE last_event_mux_ = portMUX_INITIALIZER_UNLOCKED;
};
}  // namespace demo
