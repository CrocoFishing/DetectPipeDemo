#pragma once

#include <cstddef>
#include <cstdint>

namespace protocol {

inline constexpr uint32_t MAGIC = 0x45434146U;
inline constexpr uint8_t VERSION = 1;
inline constexpr size_t HEADER_SIZE = 32;

enum class MessageType : uint8_t {
    Hello = 1, HelloAck = 2, StartCapture = 3, CaptureAccepted = 4,
    Busy = 5, Status = 6, ImageBegin = 7, ImageChunk = 8, ImageEnd = 9,
    RecognitionResult = 10, NoFace = 11, Error = 12, Ping = 13, Pong = 14,
};
enum class Command : uint16_t { StartCapture = 1 };
enum class StatusCode : uint8_t { Ok = 0, Unknown = 1, NoFace = 2, Failed = 3 };
enum class ErrorCode : uint16_t {
    None = 0, InvalidPacket = 1, UnsupportedVersion = 2, CrcMismatch = 3,
    Busy = 4, DuplicateRequest = 5, CameraInit = 6, PsramUnavailable = 7,
    CaptureFailed = 8, DetectorLoad = 9, InvalidBoundingBox = 10,
    CropAllocation = 11, JpegEncode = 12, BleNotConnected = 13,
    NotifyNotSubscribed = 14, BleCongestion = 15, TransferDisconnected = 16,
    ImageTimeout = 17, ChunkMissing = 18, RecognitionTimeout = 19,
    StaleResult = 20, IncorrectImageId = 21, Internal = 22,
};
enum FeatureFlag : uint32_t {
    ExternalButton = 1U << 0, BleTrigger = 1U << 1, FaceDetection = 1U << 2,
    CroppedJpeg = 1U << 3, RssiMetrics = 1U << 4,
};

struct PacketHeader {
    uint32_t magic{MAGIC};
    uint8_t version{VERSION};
    MessageType message_type{MessageType::Error};
    uint16_t flags{0};
    uint32_t request_id{0};
    uint32_t image_id{0};
    uint32_t payload_length{0};
    uint16_t chunk_index{0};
    uint16_t total_chunks{0};
    uint32_t crc32{0};
    uint32_t reserved{0};
};

enum class DecodeStatus { Ok, Partial, InvalidMagic, UnsupportedVersion, InvalidLength, CrcMismatch, UnknownType };

uint32_t crc32(const uint8_t* data, size_t length);
bool is_known_type(uint8_t value);
size_t encode_header(const PacketHeader& header, uint8_t* output, size_t capacity);
DecodeStatus decode_packet(const uint8_t* data, size_t length, PacketHeader& header, const uint8_t*& payload);
const char* message_type_name(MessageType type);

}  // namespace protocol

