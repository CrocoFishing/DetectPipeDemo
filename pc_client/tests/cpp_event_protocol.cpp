#include "application_protocol.hpp"
#include <array>
#include <cassert>
#include <iomanip>
#include <iostream>

template <typename Payload>
void print_packet(protocol::MessageType type, const Payload& value, uint32_t request) {
    std::array<uint8_t, 64> bytes{};
    const size_t length = protocol::encode_payload(value, bytes.data() + 32, 32);
    assert(length != 0);
    protocol::PacketHeader header;
    header.message_type = type; header.request_id = request;
    header.payload_length = static_cast<uint32_t>(length);
    header.crc32 = protocol::crc32(bytes.data() + 32, length);
    assert(protocol::encode_header(header, bytes.data(), 32) == 32);
    const uint8_t* payload = nullptr;
    protocol::PacketHeader decoded;
    assert(protocol::decode_packet(bytes.data(), length + 32, decoded, payload) == protocol::DecodeStatus::Ok);
    Payload parsed;
    assert(protocol::decode_payload(payload, length, parsed));
    for (size_t i = 0; i < length + 32; ++i) {
        std::cout << std::hex << std::uppercase << std::setfill('0') << std::setw(2)
                  << static_cast<unsigned>(bytes[i]);
    }
    std::cout << '\n';
}

int main() {
    static_assert(protocol::VERSION == 2 && protocol::HEADER_SIZE == 32);
    constexpr uint64_t id = 0xFEDCBA9876543210ULL;
    constexpr uint32_t request = 0x80000012U;
    print_packet(protocol::MessageType::EventOpen, protocol::EventOpenPayload{id, protocol::TriggerSource::Event1}, request);
    print_packet(protocol::MessageType::EventAudioReady,
                 protocol::EventAudioReadyPayload{id, protocol::AudioStatus::Ready,
                     0x1122334455667788ULL, 0x8877665544332211ULL}, request);
    print_packet(protocol::MessageType::FaceBatchEnd,
                 protocol::FaceBatchEndPayload{id, request, 3, 3, 1, 1, 1, protocol::FaceStatus::Failed}, request);
    print_packet(protocol::MessageType::EventComplete,
                 protocol::EventCompletePayload{id, protocol::EventStatus::CompleteWithError,
                     protocol::AudioStatus::Ready, protocol::FaceStatus::Failed}, request);
    print_packet(protocol::MessageType::SdExportStatus,
                 protocol::SdExportStatusPayload{id, protocol::SdExportState::Failed,
                     protocol::ErrorCode::SdIo}, 7);
    std::array<uint8_t, 32> bytes{};
    protocol::FaceBatchEndPayload parsed;
    protocol::FaceBatchEndPayload invalid{id, request, 3, 2, 1, 0, 0, protocol::FaceStatus::Partial};
    assert(protocol::encode_payload(invalid, bytes.data(), bytes.size()) == 32);
    assert(!protocol::decode_payload(bytes.data(), bytes.size(), parsed));
    invalid.completed_face_count = 1;
    assert(protocol::encode_payload(invalid, bytes.data(), bytes.size()) == 32);
    assert(protocol::decode_payload(bytes.data(), bytes.size(), parsed));
    bytes[31] = 1;
    assert(!protocol::decode_payload(bytes.data(), bytes.size(), parsed));
    protocol::EventCompletePayload invalid_complete{id, protocol::EventStatus::Complete,
        protocol::AudioStatus::Failed, protocol::FaceStatus::Completed};
    protocol::EventCompletePayload parsed_complete;
    assert(protocol::encode_payload(invalid_complete, bytes.data(), bytes.size()) == 16);
    assert(!protocol::decode_payload(bytes.data(), 16, parsed_complete));
    protocol::EventCompletePayload prior_error{id, protocol::EventStatus::CompleteWithError,
        protocol::AudioStatus::Ready, protocol::FaceStatus::Completed};
    assert(protocol::encode_payload(prior_error, bytes.data(), bytes.size()) == 16);
    assert(protocol::decode_payload(bytes.data(), 16, parsed_complete));
    protocol::SdExportStatusPayload export_status{id, protocol::SdExportState::Ready, protocol::ErrorCode::None};
    protocol::SdExportStatusPayload parsed_export;
    assert(protocol::encode_payload(export_status, bytes.data(), bytes.size()) == 16);
    assert(protocol::decode_payload(bytes.data(), 16, parsed_export));
    assert(parsed_export.session_id == id && parsed_export.state == protocol::SdExportState::Ready);
    assert(!protocol::decode_payload(bytes.data(), 15, parsed_export));
    bytes[9] = 1;
    assert(!protocol::decode_payload(bytes.data(), 16, parsed_export));
    bytes[9] = 0; bytes[12] = 1;
    assert(!protocol::decode_payload(bytes.data(), 16, parsed_export));
    bytes[12] = 0; bytes[8] = 8;
    assert(!protocol::decode_payload(bytes.data(), 16, parsed_export));
    bytes[8] = 2; bytes[10] = 32;
    assert(!protocol::decode_payload(bytes.data(), 16, parsed_export));
    assert(protocol::is_known_type(19) && protocol::is_known_type(22) && !protocol::is_known_type(23));
    return 0;
}
