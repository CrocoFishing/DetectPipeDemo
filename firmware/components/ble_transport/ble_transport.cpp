#include "ble_transport.hpp"

#include <algorithm>
#include <cstring>
#include "board_config.hpp"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "host/ble_uuid.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs_flash.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

extern "C" void ble_store_config_init(void);

namespace demo {
namespace {
constexpr char TAG[] = "ble_transport";
constexpr uint16_t NO_CONNECTION = 0xffff;
constexpr size_t ATT_OVERHEAD = 3;
constexpr size_t MAX_PACKET = 517;
constexpr int64_t NOTIFY_RETRY_TIMEOUT_US = 500'000;
constexpr uint32_t NOTIFY_RETRY_DELAY_MS = 5;
BleTransport* instance = nullptr;
uint8_t own_addr_type = 0;
uint16_t control_handle, event_handle, image_handle, result_handle, device_info_handle;

#define UUID128_LAST(v) BLE_UUID128_INIT(0x00,0x10,0x7e,0x2d,0x8a,0x6c,0x3a,0x9f,0x6a,0x4f,0xb2,0xb7,(v),0x00,0x51,0x7f)
const ble_uuid128_t service_uuid = UUID128_LAST(0x00);
const ble_uuid128_t control_uuid = UUID128_LAST(0x01);
const ble_uuid128_t event_uuid = UUID128_LAST(0x02);
const ble_uuid128_t image_uuid = UUID128_LAST(0x03);
const ble_uuid128_t result_uuid = UUID128_LAST(0x04);
const ble_uuid128_t device_info_uuid = UUID128_LAST(0x05);

ble_gatt_chr_def characteristics[6]{};
ble_gatt_svc_def services[2]{};
void configure_gatt_defs() {
    const ble_uuid_t* uuids[] = {&control_uuid.u, &event_uuid.u, &image_uuid.u, &result_uuid.u, &device_info_uuid.u};
    uint16_t* handles[] = {&control_handle, &event_handle, &image_handle, &result_handle, &device_info_handle};
    const uint32_t write_security = board::BLE_REQUIRE_ENCRYPTION ? BLE_GATT_CHR_F_WRITE_ENC : 0;
    const uint32_t read_security = board::BLE_REQUIRE_ENCRYPTION ? BLE_GATT_CHR_F_READ_ENC : 0;
    const uint32_t result_auth = board::BLE_REQUIRE_AUTHENTICATED_RESULT ? BLE_GATT_CHR_F_WRITE_AUTHEN : 0;
    uint32_t flags[] = {BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | write_security,
        BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ, BLE_GATT_CHR_F_NOTIFY, BLE_GATT_CHR_F_WRITE, BLE_GATT_CHR_F_READ};
    flags[1] |= read_security;
    flags[3] |= write_security | result_auth;
    flags[4] |= read_security;
    for (size_t i = 0; i < 5; ++i) {
        characteristics[i].uuid = uuids[i]; characteristics[i].access_cb = BleTransport::gatt_access;
        characteristics[i].flags = flags[i]; characteristics[i].val_handle = handles[i];
    }
    services[0].type = BLE_GATT_SVC_TYPE_PRIMARY; services[0].uuid = &service_uuid.u; services[0].characteristics = characteristics;
}
}

size_t BleTransport::chunk_payload_size() const {
    if (negotiated_mtu_ <= ATT_OVERHEAD + protocol::HEADER_SIZE) return 0;
    return negotiated_mtu_ - ATT_OVERHEAD - protocol::HEADER_SIZE;
}

esp_err_t BleTransport::initialize(BlePacketHandler control, BlePacketHandler result, void* context) {
    instance = this; control_handler_ = control; result_handler_ = result; handler_context_ = context;
    esp_err_t err = nvs_flash_init();
    // NVS owns the persistent Event boot counter. Automatic erasure could reuse Event IDs.
    ESP_RETURN_ON_ERROR(err, TAG, "NVS init failed; preserve persistent Event identity");
    event_mutex_ = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(event_mutex_, ESP_ERR_NO_MEM, TAG, "event notification mutex unavailable");
    ESP_RETURN_ON_ERROR(nimble_port_init(), TAG, "NimBLE init failed");
    ble_hs_cfg.sync_cb = on_sync; ble_hs_cfg.reset_cb = [](int reason) { ESP_LOGE(TAG, "NimBLE reset reason=%d", reason); };
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_bonding = board::BLE_REQUIRE_PAIRING;
    ble_hs_cfg.sm_sc = board::BLE_REQUIRE_ENCRYPTION;
    ble_hs_cfg.sm_mitm = board::BLE_REQUIRE_AUTHENTICATED_RESULT;
    ble_svc_gap_init(); ble_svc_gatt_init(); configure_gatt_defs();
    int rc = ble_gatts_count_cfg(services); if (rc == 0) rc = ble_gatts_add_svcs(services); if (rc != 0) return ESP_FAIL;
    rc = ble_svc_gap_device_name_set(board::DEVICE_NAME); if (rc != 0) return ESP_FAIL;
    ble_att_set_preferred_mtu(517); ble_store_config_init();
    nimble_port_freertos_init(host_task); ESP_LOGI(TAG, "NimBLE initialized device=%s", board::DEVICE_NAME); return ESP_OK;
}

void BleTransport::host_task(void*) { nimble_port_run(); nimble_port_freertos_deinit(); }
void BleTransport::on_sync() {
    if (!instance || ble_hs_util_ensure_addr(0) != 0 || ble_hs_id_infer_auto(0, &own_addr_type) != 0) return;
    instance->advertise();
}
esp_err_t BleTransport::advertise() {
    ble_hs_adv_fields f{}; f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.uuids128 = const_cast<ble_uuid128_t*>(&service_uuid); f.num_uuids128 = 1; f.uuids128_is_complete = 1;
    if (ble_gap_adv_set_fields(&f) != 0) return ESP_FAIL;
    ble_hs_adv_fields scan{}; scan.name = reinterpret_cast<uint8_t*>(const_cast<char*>(board::DEVICE_NAME));
    scan.name_len = std::strlen(board::DEVICE_NAME); scan.name_is_complete = 1;
    if (ble_gap_adv_rsp_set_fields(&scan) != 0) return ESP_FAIL;
    ble_gap_adv_params p{}; p.conn_mode = BLE_GAP_CONN_MODE_UND; p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    return ble_gap_adv_start(own_addr_type, nullptr, BLE_HS_FOREVER, &p, gap_event, this) == 0 ? ESP_OK : ESP_FAIL;
}

int BleTransport::gap_event(ble_gap_event* e, void* arg) {
    auto* self = static_cast<BleTransport*>(arg);
    switch (e->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (e->connect.status == 0) { self->connected_ = true; self->connection_handle_ = e->connect.conn_handle; self->negotiated_mtu_ = ble_att_mtu(e->connect.conn_handle);
            self->counters_.reconnect_count++; ESP_LOGI(TAG, "connected handle=%u mtu=%u", self->connection_handle_, self->negotiated_mtu_); }
        else self->advertise();
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        self->connected_ = false; self->event_subscribed_ = false; self->image_subscribed_ = false; self->connection_handle_ = NO_CONNECTION;
        self->counters_.disconnect_count++; if (self->transferring_) self->counters_.transfer_failure++;
        ESP_LOGW(TAG, "disconnect reason=%d transfer_active=%d", e->disconnect.reason, self->transferring_); self->advertise(); break;
    case BLE_GAP_EVENT_MTU: self->negotiated_mtu_ = e->mtu.value; ESP_LOGI(TAG, "negotiated_mtu=%u", self->negotiated_mtu_); break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (e->subscribe.attr_handle == event_handle) self->event_subscribed_ = e->subscribe.cur_notify;
        if (e->subscribe.attr_handle == image_handle) self->image_subscribed_ = e->subscribe.cur_notify;
        ESP_LOGI(TAG, "subscribe handle=%u notify=%d", e->subscribe.attr_handle, e->subscribe.cur_notify); break;
    case BLE_GAP_EVENT_ADV_COMPLETE: self->advertise(); break;
    default: break;
    } return 0;
}

int BleTransport::gatt_access(uint16_t, uint16_t attr, ble_gatt_access_ctxt* ctxt, void*) {
    if (!instance) return BLE_ATT_ERR_UNLIKELY;
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR && (attr == control_handle || attr == result_handle)) {
        uint8_t data[256]; uint16_t length = 0;
        if (OS_MBUF_PKTLEN(ctxt->om) > sizeof(data) || ble_hs_mbuf_to_flat(ctxt->om, data, sizeof(data), &length) != 0) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        BlePacketHandler handler = attr == control_handle ? instance->control_handler_ : instance->result_handler_;
        if (handler) handler(data, length, instance->handler_context_);
        return 0;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR && attr == event_handle) {
        // Do not block the NimBLE host on the notification mutex: retrying sends need this host.
        uint8_t last_event[MAX_PACKET];
        portENTER_CRITICAL(&instance->last_event_mux_);
        const size_t length = instance->last_event_length_;
        std::memcpy(last_event, instance->last_event_, length);
        portEXIT_CRITICAL(&instance->last_event_mux_);
        return os_mbuf_append(ctxt->om, last_event, static_cast<uint16_t>(length)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR && attr == device_info_handle) {
        char info[192];
        int n = snprintf(info, sizeof(info),
            "protocol=%u;firmware=%s;build=%s;model=%s;max_image=%u;features=0x%lx",
            static_cast<unsigned>(protocol::VERSION), board::FIRMWARE_VERSION, board::BUILD_ID, board::DETECTOR_MODEL_NAME,
            static_cast<unsigned>(board::MAX_JPEG_BYTES),
            static_cast<unsigned long>(0x1fU | (instance->event_association_enabled_ ? protocol::EventAssociation : 0U)));
        const size_t info_length = std::min<size_t>(sizeof(info) - 1, static_cast<size_t>(std::max(0, n)));
        return os_mbuf_append(ctxt->om, info, static_cast<uint16_t>(info_length)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

esp_err_t BleTransport::notify(uint16_t handle, const uint8_t* data, size_t len) {
    if (!connected_) return ESP_ERR_INVALID_STATE;
    const int64_t deadline = esp_timer_get_time() + NOTIFY_RETRY_TIMEOUT_US;
    bool congestion_counted = false;
    int last_rc = BLE_HS_ENOMEM;
    while (connected_) {
        os_mbuf* om = ble_hs_mbuf_from_flat(data, static_cast<uint16_t>(len));
        if (om) {
            const int rc = ble_gatts_notify_custom(connection_handle_, handle, om);
            last_rc = rc;
            // ble_gatts_notify_custom consumes om on both success and failure.
            if (rc == 0) return ESP_OK;
            if (rc == BLE_HS_ENOTCONN) return ESP_ERR_INVALID_STATE;
            if (rc != BLE_HS_EAGAIN && rc != BLE_HS_EBUSY && rc != BLE_HS_ENOMEM) {
                ESP_LOGW(TAG, "notification failed handle=%u len=%u rc=%d", handle,
                         static_cast<unsigned>(len), rc);
                return ESP_FAIL;
            }
        }

        counters_.retry_count++;
        if (!congestion_counted) {
            counters_.queue_congestion_count++;
            congestion_counted = true;
        }
        if (esp_timer_get_time() >= deadline) {
            ESP_LOGW(TAG, "notification congestion timeout handle=%u len=%u rc=%d msys_free=%d",
                     handle, static_cast<unsigned>(len), last_rc, os_msys_num_free());
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(NOTIFY_RETRY_DELAY_MS));
    }
    return ESP_ERR_INVALID_STATE;
}
esp_err_t BleTransport::make_and_notify(uint16_t handle, protocol::PacketHeader h, const uint8_t* payload) {
    if (h.payload_length > MAX_PACKET - protocol::HEADER_SIZE) return ESP_ERR_INVALID_SIZE;
    if (h.payload_length && !payload) return ESP_ERR_INVALID_ARG;
    uint8_t packet[MAX_PACKET]; h.crc32 = protocol::crc32(payload, h.payload_length);
    protocol::encode_header(h, packet, sizeof(packet)); if (h.payload_length) std::memcpy(packet + protocol::HEADER_SIZE, payload, h.payload_length);
    if (handle == event_handle) {
        portENTER_CRITICAL(&last_event_mux_);
        last_event_length_ = protocol::HEADER_SIZE + h.payload_length;
        std::memcpy(last_event_, packet, last_event_length_);
        portEXIT_CRITICAL(&last_event_mux_);
    }
    return notify(handle, packet, protocol::HEADER_SIZE + h.payload_length);
}
esp_err_t BleTransport::send_event(protocol::MessageType type, uint32_t request, uint32_t image, const uint8_t* payload, size_t len, uint16_t flags, uint32_t reserved) {
    if (!event_mutex_ || !connected_) return ESP_ERR_INVALID_STATE;
    if (len > MAX_PACKET - protocol::HEADER_SIZE) return ESP_ERR_INVALID_SIZE;
    if (len && !payload) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(event_mutex_, pdMS_TO_TICKS(600)) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (!connected_ || !event_subscribed_) {
        xSemaphoreGive(event_mutex_);
        return connected_ ? ESP_ERR_NOT_FOUND : ESP_ERR_INVALID_STATE;
    }
    if (negotiated_mtu_ < ATT_OVERHEAD + protocol::HEADER_SIZE ||
        protocol::HEADER_SIZE + len > negotiated_mtu_ - ATT_OVERHEAD) {
        xSemaphoreGive(event_mutex_); return ESP_ERR_INVALID_SIZE;
    }
    protocol::PacketHeader h{}; h.message_type = type; h.request_id = request; h.image_id = image; h.flags = flags; h.reserved = reserved; h.payload_length = len;
    const esp_err_t result = make_and_notify(event_handle, h, payload);
    xSemaphoreGive(event_mutex_);
    return result;
}
void BleTransport::sample_rssi(uint32_t image, int64_t now) {
    if (now - last_rssi_us_ < 1'000'000 || connection_handle_ == NO_CONNECTION) return;
    int8_t rssi = 0; if (ble_gap_conn_rssi(connection_handle_, &rssi) == 0) { metrics_.add_rssi(rssi); metrics_.emit(image, "rssi_current_dbm", rssi, "dBm"); }
    last_rssi_us_ = now;
}
esp_err_t BleTransport::send_image(uint32_t request, uint32_t image, const uint8_t* jpeg, size_t len,
                                   uint16_t face_index, uint16_t face_count) {
    if (!connected_) return ESP_ERR_INVALID_STATE;
    if (!image_subscribed_) return ESP_ERR_NOT_FOUND;
    const size_t chunk = chunk_payload_size(); if (!chunk || !jpeg || !len || len > board::MAX_JPEG_BYTES) return ESP_ERR_INVALID_SIZE;
    if ((!face_count && face_index) || (face_count && face_index >= face_count)) return ESP_ERR_INVALID_ARG;
    const uint16_t sequence_flags = face_count
        ? static_cast<uint16_t>(protocol::MessageFlag::FaceSequence)
        : 0;
    const uint32_t sequence_metadata = face_count
        ? (static_cast<uint32_t>(face_count) << 16) | face_index
        : 0;
    const uint16_t total = static_cast<uint16_t>((len + chunk - 1) / chunk); const uint32_t image_crc = protocol::crc32(jpeg, len);
    uint8_t meta[8]; for (int i=0;i<4;++i) { meta[i]=static_cast<uint8_t>(len>>(8*i)); meta[4+i]=static_cast<uint8_t>(image_crc>>(8*i)); }
    esp_err_t err = send_event(protocol::MessageType::ImageBegin, request, image, meta, sizeof(meta),
                               sequence_flags, sequence_metadata); if (err != ESP_OK) return err;
    transferring_ = true; metrics_.reset_rssi(); const int64_t started = esp_timer_get_time();
    for (uint16_t i = 0; i < total && connected_; ++i) {
        if (esp_timer_get_time() - started > 15'000'000) { err = ESP_ERR_TIMEOUT; break; }
        const size_t offset = static_cast<size_t>(i) * chunk; const size_t n = std::min(chunk, len - offset);
        protocol::PacketHeader h{}; h.message_type = protocol::MessageType::ImageChunk; h.request_id = request; h.image_id = image;
        h.payload_length = n; h.chunk_index = i; h.total_chunks = total;
        err = make_and_notify(image_handle, h, jpeg + offset); if (err != ESP_OK) break;
        sample_rssi(image, esp_timer_get_time()); vTaskDelay(1);
    }
    transferring_ = false;
    if (!connected_) err = ESP_ERR_INVALID_STATE;
    if (err == ESP_OK && connected_) {
        err = send_event(protocol::MessageType::ImageEnd, request, image, meta, sizeof(meta),
                         sequence_flags, sequence_metadata);
    }
    const double ms = (esp_timer_get_time() - started) / 1000.0;
    metrics_.emit(image, "ble_transfer_ms", ms, "ms"); metrics_.emit(image, "image_bytes", len, "bytes");
    metrics_.emit(image, "chunk_count", total, "count"); metrics_.emit(image, "application_throughput", ms > 0 ? len * 8.0 / ms : 0, "kbps");
    metrics_.log_rssi(image); if (err == ESP_OK) counters_.transfer_success++; else counters_.transfer_failure++; return err;
}
}  // namespace demo
