#include "application_protocol.hpp"

namespace protocol {
namespace {
void put16(uint8_t* p, uint16_t v) { p[0] = static_cast<uint8_t>(v); p[1] = static_cast<uint8_t>(v >> 8); }
void put32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i)); }
uint16_t get16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8)); }
uint32_t get32(const uint8_t* p) { return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24); }
}

uint32_t crc32(const uint8_t* data, size_t length) {
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
    }
    return crc ^ 0xFFFFFFFFU;
}

bool is_known_type(uint8_t value) { return value >= 1 && value <= 14; }

size_t encode_header(const PacketHeader& h, uint8_t* out, size_t cap) {
    if (!out || cap < HEADER_SIZE) return 0;
    put32(out, h.magic); out[4] = h.version; out[5] = static_cast<uint8_t>(h.message_type);
    put16(out + 6, h.flags); put32(out + 8, h.request_id); put32(out + 12, h.image_id);
    put32(out + 16, h.payload_length); put16(out + 20, h.chunk_index); put16(out + 22, h.total_chunks);
    put32(out + 24, h.crc32); put32(out + 28, h.reserved); return HEADER_SIZE;
}

DecodeStatus decode_packet(const uint8_t* data, size_t len, PacketHeader& h, const uint8_t*& payload) {
    payload = nullptr;
    if (!data || len < HEADER_SIZE) return DecodeStatus::Partial;
    h.magic = get32(data); h.version = data[4]; h.message_type = static_cast<MessageType>(data[5]);
    h.flags = get16(data + 6); h.request_id = get32(data + 8); h.image_id = get32(data + 12);
    h.payload_length = get32(data + 16); h.chunk_index = get16(data + 20); h.total_chunks = get16(data + 22);
    h.crc32 = get32(data + 24); h.reserved = get32(data + 28);
    if (h.magic != MAGIC) return DecodeStatus::InvalidMagic;
    if (h.version != VERSION) return DecodeStatus::UnsupportedVersion;
    if (!is_known_type(data[5])) return DecodeStatus::UnknownType;
    if (h.payload_length > len - HEADER_SIZE) return DecodeStatus::Partial;
    if (h.payload_length != len - HEADER_SIZE) return DecodeStatus::InvalidLength;
    payload = data + HEADER_SIZE;
    if (crc32(payload, h.payload_length) != h.crc32) return DecodeStatus::CrcMismatch;
    return DecodeStatus::Ok;
}

const char* message_type_name(MessageType t) {
    static constexpr const char* names[] = {"INVALID", "HELLO", "HELLO_ACK", "START_CAPTURE", "CAPTURE_ACCEPTED", "BUSY", "STATUS", "IMAGE_BEGIN", "IMAGE_CHUNK", "IMAGE_END", "RECOGNITION_RESULT", "NO_FACE", "ERROR", "PING", "PONG"};
    const auto i = static_cast<unsigned>(t); return i < sizeof(names) / sizeof(names[0]) ? names[i] : "UNKNOWN";
}
}  // namespace protocol
