#include <cstring>
#include "application_protocol.hpp"
#include "esp_log.h"
extern "C" void app_main() {
    uint8_t packet[64]{}; const uint8_t payload[]{1,2,3,4}; protocol::PacketHeader h{};
    h.message_type=protocol::MessageType::Ping; h.request_id=7; h.payload_length=sizeof(payload); h.crc32=protocol::crc32(payload,sizeof(payload));
    protocol::encode_header(h,packet,sizeof(packet)); std::memcpy(packet+protocol::HEADER_SIZE,payload,sizeof(payload));
    protocol::PacketHeader decoded{}; const uint8_t* p{}; auto ok=protocol::decode_packet(packet,protocol::HEADER_SIZE+sizeof(payload),decoded,p);
    packet[0]^=1; auto bad=protocol::decode_packet(packet,protocol::HEADER_SIZE+sizeof(payload),decoded,p);
    ESP_LOGI("phase04","[TEST] phase=4 valid=%d invalid_magic=%d result=%s",static_cast<int>(ok),static_cast<int>(bad),ok==protocol::DecodeStatus::Ok&&bad==protocol::DecodeStatus::InvalidMagic?"PASS":"FAIL");
}

