#include "application_protocol.hpp"
#include <cstring>

namespace protocol {
namespace {
void put16(uint8_t* p, uint16_t v) { p[0] = static_cast<uint8_t>(v); p[1] = static_cast<uint8_t>(v >> 8); }
void put32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i)); }
void put64(uint8_t* p, uint64_t v) { for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i)); }
uint16_t get16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8)); }
uint32_t get32(const uint8_t* p) { return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24); }
uint64_t get64(const uint8_t* p) { return get32(p) | (static_cast<uint64_t>(get32(p + 4)) << 32); }
bool reserved_zero(const uint8_t* p, size_t count) {
    for (size_t i = 0; i < count; ++i) if (p[i]) return false;
    return true;
}
bool prepare(uint8_t* p, size_t capacity, size_t length) {
    if (!p || capacity < length) return false;
    std::memset(p, 0, length); return true;
}
}

uint32_t crc32(const uint8_t* data, size_t length) {
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
    }
    return crc ^ 0xFFFFFFFFU;
}

bool is_known_type(uint8_t value) { return value >= 1 && value <= 18; }

size_t encode_payload(const EventOpenPayload& v, uint8_t* p, size_t cap) {
    if (!prepare(p, cap, EVENT_OPEN_PAYLOAD_SIZE)) return 0;
    put64(p, v.event_id); p[8] = static_cast<uint8_t>(v.trigger_type);
    return EVENT_OPEN_PAYLOAD_SIZE;
}
size_t encode_payload(const EventAudioReadyPayload& v, uint8_t* p, size_t cap) {
    if (!prepare(p, cap, EVENT_AUDIO_READY_PAYLOAD_SIZE)) return 0;
    put64(p, v.event_id); p[8] = static_cast<uint8_t>(v.audio_status);
    put64(p + 16, v.audio_first_sample); put64(p + 24, v.audio_last_sample);
    return EVENT_AUDIO_READY_PAYLOAD_SIZE;
}
size_t encode_payload(const FaceBatchEndPayload& v, uint8_t* p, size_t cap) {
    if (!prepare(p, cap, FACE_BATCH_END_PAYLOAD_SIZE)) return 0;
    put64(p, v.event_id); put32(p + 8, v.request_id);
    put16(p + 12, v.expected_face_count); put16(p + 14, v.completed_face_count);
    put16(p + 16, v.recognized_count); put16(p + 18, v.unknown_count); put16(p + 20, v.failed_count);
    p[22] = static_cast<uint8_t>(v.face_status); return FACE_BATCH_END_PAYLOAD_SIZE;
}
size_t encode_payload(const EventCompletePayload& v, uint8_t* p, size_t cap) {
    if (!prepare(p, cap, EVENT_COMPLETE_PAYLOAD_SIZE)) return 0;
    put64(p, v.event_id); p[8] = static_cast<uint8_t>(v.event_status);
    p[9] = static_cast<uint8_t>(v.audio_status); p[10] = static_cast<uint8_t>(v.face_status);
    return EVENT_COMPLETE_PAYLOAD_SIZE;
}
bool decode_payload(const uint8_t* p, size_t len, EventOpenPayload& v) {
    if (!p || len != EVENT_OPEN_PAYLOAD_SIZE || p[8] != 3 || !reserved_zero(p + 9, 7)) return false;
    v.event_id = get64(p); v.trigger_type = static_cast<TriggerSource>(p[8]); return v.event_id != 0;
}
bool decode_payload(const uint8_t* p, size_t len, EventAudioReadyPayload& v) {
    if (!p || len != EVENT_AUDIO_READY_PAYLOAD_SIZE || p[8] < 1 || p[8] > 2 || !reserved_zero(p + 9, 7)) return false;
    v.event_id = get64(p); v.audio_status = static_cast<AudioStatus>(p[8]);
    v.audio_first_sample = get64(p + 16); v.audio_last_sample = get64(p + 24);
    return v.event_id != 0 && v.audio_last_sample >= v.audio_first_sample;
}
bool decode_payload(const uint8_t* p, size_t len, FaceBatchEndPayload& v) {
    if (!p || len != FACE_BATCH_END_PAYLOAD_SIZE || p[22] < 1 || p[22] > 7 || !reserved_zero(p + 23, 9)) return false;
    v.event_id = get64(p); v.request_id = get32(p + 8);
    v.expected_face_count = get16(p + 12); v.completed_face_count = get16(p + 14);
    v.recognized_count = get16(p + 16); v.unknown_count = get16(p + 18); v.failed_count = get16(p + 20);
    v.face_status = static_cast<FaceStatus>(p[22]);
    if (!v.event_id || v.completed_face_count > v.expected_face_count ||
        static_cast<uint32_t>(v.recognized_count) + v.unknown_count + v.failed_count != v.completed_face_count) return false;
    if (v.face_status == FaceStatus::Completed &&
        (v.completed_face_count != v.expected_face_count || v.failed_count)) return false;
    if ((v.face_status == FaceStatus::NoFace || v.face_status == FaceStatus::Busy) &&
        (v.expected_face_count || v.completed_face_count)) return false;
    return true;
}
bool decode_payload(const uint8_t* p, size_t len, EventCompletePayload& v) {
    if (!p || len != EVENT_COMPLETE_PAYLOAD_SIZE || p[8] > 1 || p[9] < 1 || p[9] > 2 ||
        p[10] < 1 || p[10] > 7 || !reserved_zero(p + 11, 5)) return false;
    v.event_id = get64(p); v.event_status = static_cast<EventStatus>(p[8]);
    v.audio_status = static_cast<AudioStatus>(p[9]); v.face_status = static_cast<FaceStatus>(p[10]);
    const auto expected = v.audio_status == AudioStatus::Ready &&
        (v.face_status == FaceStatus::Completed || v.face_status == FaceStatus::NoFace)
        ? EventStatus::Complete : EventStatus::CompleteWithError;
    return v.event_id != 0 && v.event_status == expected;
}

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
    static constexpr const char* names[] = {"INVALID", "HELLO", "HELLO_ACK", "START_CAPTURE", "CAPTURE_ACCEPTED", "BUSY", "STATUS", "IMAGE_BEGIN", "IMAGE_CHUNK", "IMAGE_END", "RECOGNITION_RESULT", "NO_FACE", "ERROR", "PING", "PONG", "EVENT_OPEN", "EVENT_AUDIO_READY", "FACE_BATCH_END", "EVENT_COMPLETE"};
    const auto i = static_cast<unsigned>(t); return i < sizeof(names) / sizeof(names[0]) ? names[i] : "UNKNOWN";
}
}  // namespace protocol
