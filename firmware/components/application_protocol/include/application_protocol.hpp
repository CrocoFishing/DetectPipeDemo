#pragma once

#include <cstddef>
#include <cstdint>

namespace protocol {

inline constexpr uint32_t MAGIC = 0x45434146U;
inline constexpr uint8_t VERSION = 2;
inline constexpr size_t HEADER_SIZE = 32;

enum class MessageType : uint8_t {
    Hello = 1, HelloAck = 2, StartCapture = 3, CaptureAccepted = 4,
    Busy = 5, Status = 6, ImageBegin = 7, ImageChunk = 8, ImageEnd = 9,
    RecognitionResult = 10, NoFace = 11, Error = 12, Ping = 13, Pong = 14,
    EventOpen = 15, EventAudioReady = 16, FaceBatchEnd = 17, EventComplete = 18,
    SdExportStart = 19, SdExportCancel = 20, SdExportGetStatus = 21, SdExportStatus = 22,
};
enum class Command : uint16_t { StartCapture = 1 };
enum class StatusCode : uint8_t { Ok = 0, Unknown = 1, NoFace = 2, Failed = 3 };
enum class TriggerSource : uint8_t { Event1 = 3 };
enum class AudioStatus : uint8_t { Pending = 0, Ready = 1, Failed = 2 };
enum class FaceStatus : uint8_t {
    Pending = 0, Completed = 1, NoFace = 2, Partial = 3,
    Busy = 4, Timeout = 5, Disconnected = 6, Failed = 7,
};
enum class EventStatus : uint8_t { Complete = 0, CompleteWithError = 1 };
enum class SdExportState : uint8_t {
    Idle = 0, Preparing = 1, Ready = 2, Transferring = 3,
    Restoring = 4, Completed = 5, Cancelled = 6, Failed = 7,
};
enum MessageFlag : uint16_t { FaceSequence = 1U << 0 };
enum class ErrorCode : uint16_t {
    None = 0, InvalidPacket = 1, UnsupportedVersion = 2, CrcMismatch = 3,
    Busy = 4, DuplicateRequest = 5, CameraInit = 6, PsramUnavailable = 7,
    CaptureFailed = 8, DetectorLoad = 9, InvalidBoundingBox = 10,
    CropAllocation = 11, JpegEncode = 12, BleNotConnected = 13,
    NotifyNotSubscribed = 14, BleCongestion = 15, TransferDisconnected = 16,
    ImageTimeout = 17, ChunkMissing = 18, RecognitionTimeout = 19,
    StaleResult = 20, IncorrectImageId = 21, Internal = 22,
    SdUnavailable = 23, ExportBusy = 24, QuiesceTimeout = 25, WifiStartFailed = 26,
    SdIo = 27, InvalidSession = 28, AudioResumeFailed = 29, PathUnsupported = 30,
    ExportExpired = 31,
};
enum FeatureFlag : uint32_t {
    ExternalButton = 1U << 0, BleTrigger = 1U << 1, FaceDetection = 1U << 2,
    CroppedJpeg = 1U << 3, RssiMetrics = 1U << 4, EventAssociation = 1U << 5,
    WifiSdExport = 1U << 6,
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

// These are host structs, never copied directly onto the wire. Serializers below
// keep alignment/padding independent of the compiler and the 32-byte header.
struct EventOpenPayload {
    uint64_t event_id{0};
    TriggerSource trigger_type{TriggerSource::Event1};
};
struct EventAudioReadyPayload {
    uint64_t event_id{0};
    AudioStatus audio_status{AudioStatus::Pending};
    uint64_t audio_first_sample{0};
    uint64_t audio_last_sample{0};
};
struct FaceBatchEndPayload {
    uint64_t event_id{0};
    uint32_t request_id{0};
    uint16_t expected_face_count{0};
    uint16_t completed_face_count{0};
    uint16_t recognized_count{0};
    uint16_t unknown_count{0};
    uint16_t failed_count{0};
    FaceStatus face_status{FaceStatus::Pending};
};
struct EventCompletePayload {
    uint64_t event_id{0};
    EventStatus event_status{EventStatus::Complete};
    AudioStatus audio_status{AudioStatus::Pending};
    FaceStatus face_status{FaceStatus::Pending};
};
struct SdExportStatusPayload {
    uint64_t session_id{0};
    SdExportState state{SdExportState::Idle};
    ErrorCode error{ErrorCode::None};
};
inline constexpr size_t EVENT_OPEN_PAYLOAD_SIZE = 16;
inline constexpr size_t EVENT_AUDIO_READY_PAYLOAD_SIZE = 32;
inline constexpr size_t FACE_BATCH_END_PAYLOAD_SIZE = 32;
inline constexpr size_t EVENT_COMPLETE_PAYLOAD_SIZE = 16;
inline constexpr size_t SD_EXPORT_STATUS_PAYLOAD_SIZE = 16;
inline constexpr size_t SD_EXPORT_SESSION_PAYLOAD_SIZE = 8;

size_t encode_payload(const EventOpenPayload& value, uint8_t* output, size_t capacity);
size_t encode_payload(const EventAudioReadyPayload& value, uint8_t* output, size_t capacity);
size_t encode_payload(const FaceBatchEndPayload& value, uint8_t* output, size_t capacity);
size_t encode_payload(const EventCompletePayload& value, uint8_t* output, size_t capacity);
size_t encode_payload(const SdExportStatusPayload& value, uint8_t* output, size_t capacity);
bool decode_payload(const uint8_t* data, size_t length, EventOpenPayload& value);
bool decode_payload(const uint8_t* data, size_t length, EventAudioReadyPayload& value);
bool decode_payload(const uint8_t* data, size_t length, FaceBatchEndPayload& value);
bool decode_payload(const uint8_t* data, size_t length, EventCompletePayload& value);
bool decode_payload(const uint8_t* data, size_t length, SdExportStatusPayload& value);

uint32_t crc32(const uint8_t* data, size_t length);
bool is_known_type(uint8_t value);
size_t encode_header(const PacketHeader& header, uint8_t* output, size_t capacity);
DecodeStatus decode_packet(const uint8_t* data, size_t length, PacketHeader& header, const uint8_t*& payload);
const char* message_type_name(MessageType type);

}  // namespace protocol
