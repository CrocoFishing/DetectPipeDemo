from __future__ import annotations

import enum
import struct
import zlib
from dataclasses import dataclass

MAGIC = 0x45434146
VERSION = 2
HEADER_FORMAT = "<IBBHIIIHHII"
HEADER_SIZE = struct.calcsize(HEADER_FORMAT)
SERVICE_UUID = "7f510000-b7b2-4f6a-9f3a-6c8a2d7e1000"
CONTROL_UUID = "7f510001-b7b2-4f6a-9f3a-6c8a2d7e1000"
EVENT_UUID = "7f510002-b7b2-4f6a-9f3a-6c8a2d7e1000"
IMAGE_UUID = "7f510003-b7b2-4f6a-9f3a-6c8a2d7e1000"
RESULT_UUID = "7f510004-b7b2-4f6a-9f3a-6c8a2d7e1000"
DEVICE_INFO_UUID = "7f510005-b7b2-4f6a-9f3a-6c8a2d7e1000"


class ProtocolError(ValueError):
    pass


class PartialPacket(ProtocolError):
    pass


class MessageType(enum.IntEnum):
    HELLO = 1
    HELLO_ACK = 2
    START_CAPTURE = 3
    CAPTURE_ACCEPTED = 4
    BUSY = 5
    STATUS = 6
    IMAGE_BEGIN = 7
    IMAGE_CHUNK = 8
    IMAGE_END = 9
    RECOGNITION_RESULT = 10
    NO_FACE = 11
    ERROR = 12
    PING = 13
    PONG = 14
    EVENT_OPEN = 15
    EVENT_AUDIO_READY = 16
    FACE_BATCH_END = 17
    EVENT_COMPLETE = 18


class MessageFlag(enum.IntFlag):
    FACE_SEQUENCE = 0x0001


class Command(enum.IntEnum):
    START_CAPTURE = 1


class StatusCode(enum.IntEnum):
    OK = 0
    UNKNOWN = 1
    NO_FACE = 2
    FAILED = 3


class TriggerSource(enum.IntEnum):
    EVENT1 = 3


class AudioStatus(enum.IntEnum):
    PENDING = 0
    READY = 1
    FAILED = 2


class FaceStatus(enum.IntEnum):
    PENDING = 0
    COMPLETED = 1
    NO_FACE = 2
    PARTIAL = 3
    BUSY = 4
    TIMEOUT = 5
    DISCONNECTED = 6
    FAILED = 7


class EventStatus(enum.IntEnum):
    COMPLETE = 0
    COMPLETE_WITH_ERROR = 1


EVENT_OPEN_FORMAT = "<QB7x"
EVENT_AUDIO_READY_FORMAT = "<QB7xQQ"
FACE_BATCH_END_FORMAT = "<QIHHHHHB9x"
EVENT_COMPLETE_FORMAT = "<QBBB5x"


def event_id_hex(event_id: int) -> str:
    if not 0 < event_id <= 0xFFFFFFFFFFFFFFFF:
        raise ProtocolError("event_id must be a nonzero uint64")
    return f"{event_id:016X}"


def _payload_values(payload: bytes, format_: str, padding_start: int, padding_end: int) -> tuple[int, ...]:
    if len(payload) != struct.calcsize(format_):
        raise ProtocolError("invalid event payload length")
    if any(payload[padding_start:padding_end]):
        raise ProtocolError("event reserved bytes must be zero")
    return struct.unpack(format_, payload)


@dataclass(frozen=True, slots=True)
class EventOpen:
    event_id: int
    trigger_type: TriggerSource = TriggerSource.EVENT1

    def encode(self) -> bytes:
        event_id_hex(self.event_id)
        return struct.pack(EVENT_OPEN_FORMAT, self.event_id, self.trigger_type)

    @classmethod
    def decode(cls, payload: bytes) -> EventOpen:
        event_id, trigger = _payload_values(payload, EVENT_OPEN_FORMAT, 9, 16)
        event_id_hex(event_id)
        try:
            return cls(event_id, TriggerSource(trigger))
        except ValueError as exc:
            raise ProtocolError("invalid event trigger") from exc


@dataclass(frozen=True, slots=True)
class EventAudioReady:
    event_id: int
    audio_status: AudioStatus
    audio_first_sample: int
    audio_last_sample: int

    def encode(self) -> bytes:
        return struct.pack(
            EVENT_AUDIO_READY_FORMAT,
            self.event_id,
            self.audio_status,
            self.audio_first_sample,
            self.audio_last_sample,
        )

    @classmethod
    def decode(cls, payload: bytes) -> EventAudioReady:
        event_id, status, first, last = _payload_values(payload, EVENT_AUDIO_READY_FORMAT, 9, 16)
        event_id_hex(event_id)
        try:
            audio = AudioStatus(status)
        except ValueError as exc:
            raise ProtocolError("invalid audio status") from exc
        if audio is AudioStatus.PENDING or last < first:
            raise ProtocolError("invalid terminal audio payload")
        return cls(event_id, audio, first, last)


@dataclass(frozen=True, slots=True)
class FaceBatchEnd:
    event_id: int
    request_id: int
    expected_face_count: int
    completed_face_count: int
    recognized_count: int
    unknown_count: int
    failed_count: int
    face_status: FaceStatus

    def encode(self) -> bytes:
        return struct.pack(
            FACE_BATCH_END_FORMAT,
            self.event_id,
            self.request_id,
            self.expected_face_count,
            self.completed_face_count,
            self.recognized_count,
            self.unknown_count,
            self.failed_count,
            self.face_status,
        )

    @classmethod
    def decode(cls, payload: bytes) -> FaceBatchEnd:
        values = _payload_values(payload, FACE_BATCH_END_FORMAT, 23, 32)
        event_id_hex(values[0])
        try:
            status = FaceStatus(values[-1])
        except ValueError as exc:
            raise ProtocolError("invalid face status") from exc
        _, _, expected, completed, recognized, unknown, failed, _ = values
        if status is FaceStatus.PENDING or completed > expected or recognized + unknown + failed != completed:
            raise ProtocolError("invalid terminal face counters")
        if status is FaceStatus.COMPLETED and (completed != expected or failed):
            raise ProtocolError("invalid completed face counters")
        if status in (FaceStatus.NO_FACE, FaceStatus.BUSY) and (expected or completed):
            raise ProtocolError("empty face status has nonzero counters")
        return cls(values[0], values[1], expected, completed, recognized, unknown, failed, status)


@dataclass(frozen=True, slots=True)
class EventComplete:
    event_id: int
    event_status: EventStatus
    audio_status: AudioStatus
    face_status: FaceStatus

    def encode(self) -> bytes:
        return struct.pack(
            EVENT_COMPLETE_FORMAT, self.event_id, self.event_status, self.audio_status, self.face_status
        )

    @classmethod
    def decode(cls, payload: bytes) -> EventComplete:
        event_id, event, audio, face = _payload_values(payload, EVENT_COMPLETE_FORMAT, 11, 16)
        event_id_hex(event_id)
        try:
            value = cls(event_id, EventStatus(event), AudioStatus(audio), FaceStatus(face))
        except ValueError as exc:
            raise ProtocolError("invalid event completion status") from exc
        if value.audio_status is AudioStatus.PENDING or value.face_status is FaceStatus.PENDING:
            raise ProtocolError("event completed before both branches terminal")
        expected = (
            EventStatus.COMPLETE
            if value.audio_status is AudioStatus.READY
            and value.face_status in (FaceStatus.COMPLETED, FaceStatus.NO_FACE)
            else EventStatus.COMPLETE_WITH_ERROR
        )
        if value.event_status is not expected:
            raise ProtocolError("event completion status disagrees with branches")
        return value


class ErrorCode(enum.IntEnum):
    NONE = 0
    INVALID_PACKET = 1
    UNSUPPORTED_VERSION = 2
    CRC_MISMATCH = 3
    BUSY = 4
    DUPLICATE_REQUEST = 5
    CAMERA_INIT = 6
    PSRAM_UNAVAILABLE = 7
    CAPTURE_FAILED = 8
    DETECTOR_LOAD = 9
    INVALID_BOUNDING_BOX = 10
    CROP_ALLOCATION = 11
    JPEG_ENCODE = 12
    BLE_NOT_CONNECTED = 13
    NOTIFY_NOT_SUBSCRIBED = 14
    BLE_CONGESTION = 15
    TRANSFER_DISCONNECTED = 16
    IMAGE_TIMEOUT = 17
    CHUNK_MISSING = 18
    RECOGNITION_TIMEOUT = 19
    STALE_RESULT = 20
    INCORRECT_IMAGE_ID = 21
    INTERNAL = 22


@dataclass(frozen=True, slots=True)
class PacketHeader:
    message_type: MessageType
    flags: int = 0
    request_id: int = 0
    image_id: int = 0
    payload_length: int = 0
    chunk_index: int = 0
    total_chunks: int = 0
    crc32: int = 0
    reserved: int = 0
    version: int = VERSION
    magic: int = MAGIC

    def encode(self) -> bytes:
        return struct.pack(
            HEADER_FORMAT,
            self.magic,
            self.version,
            int(self.message_type),
            self.flags,
            self.request_id,
            self.image_id,
            self.payload_length,
            self.chunk_index,
            self.total_chunks,
            self.crc32,
            self.reserved,
        )


@dataclass(frozen=True, slots=True)
class Packet:
    header: PacketHeader
    payload: bytes

    def encode(self) -> bytes:
        header = PacketHeader(
            message_type=self.header.message_type,
            flags=self.header.flags,
            request_id=self.header.request_id,
            image_id=self.header.image_id,
            payload_length=len(self.payload),
            chunk_index=self.header.chunk_index,
            total_chunks=self.header.total_chunks,
            crc32=crc32(self.payload),
            reserved=self.header.reserved,
        )
        return header.encode() + self.payload


def pack_face_sequence(face_index: int, face_count: int) -> tuple[int, int]:
    if not 0 < face_count <= 0xFFFF:
        raise ValueError("face_count must be between 1 and 65535")
    if not 0 <= face_index < face_count:
        raise ValueError("face_index must be less than face_count")
    return int(MessageFlag.FACE_SEQUENCE), (face_count << 16) | face_index


def unpack_face_sequence(header: PacketHeader) -> tuple[int, int]:
    if not header.flags & MessageFlag.FACE_SEQUENCE:
        return 0, 1
    face_index = header.reserved & 0xFFFF
    face_count = (header.reserved >> 16) & 0xFFFF
    if face_count == 0 or face_index >= face_count:
        raise ProtocolError("invalid face sequence metadata")
    return face_index, face_count


def crc32(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


def decode_packet(data: bytes) -> Packet:
    if len(data) < HEADER_SIZE:
        raise PartialPacket(f"need {HEADER_SIZE} header bytes, got {len(data)}")
    values = struct.unpack_from(HEADER_FORMAT, data)
    magic, version, message_value, flags, request_id, image_id, length, index, total, checksum, reserved = (
        values
    )
    if magic != MAGIC:
        raise ProtocolError("invalid magic")
    if version != VERSION:
        raise ProtocolError(f"unsupported version {version}")
    try:
        message_type = MessageType(message_value)
    except ValueError as exc:
        raise ProtocolError(f"unknown message type {message_value}") from exc
    if len(data) < HEADER_SIZE + length:
        raise PartialPacket("partial payload")
    if len(data) != HEADER_SIZE + length:
        raise ProtocolError("invalid packet length")
    payload = data[HEADER_SIZE:]
    if crc32(payload) != checksum:
        raise ProtocolError("CRC mismatch")
    return Packet(
        PacketHeader(message_type, flags, request_id, image_id, length, index, total, checksum, reserved),
        payload,
    )


def make_packet(
    message_type: MessageType,
    *,
    request_id: int = 0,
    image_id: int = 0,
    payload: bytes = b"",
    flags: int = 0,
    chunk_index: int = 0,
    total_chunks: int = 0,
    reserved: int = 0,
) -> bytes:
    return Packet(
        PacketHeader(
            message_type,
            flags,
            request_id,
            image_id,
            len(payload),
            chunk_index,
            total_chunks,
            crc32(payload),
            reserved,
        ),
        payload,
    ).encode()


def start_capture_packet(request_id: int, timestamp_s: int, desired_test_bytes: int = 0) -> bytes:
    if request_id <= 0 or request_id & 0x80000000:
        raise ValueError("client request_id must be non-zero with bit 31 clear")
    payload = struct.pack("<HHI", Command.START_CAPTURE, 0, timestamp_s & 0xFFFFFFFF)
    return make_packet(
        MessageType.START_CAPTURE, request_id=request_id, payload=payload, reserved=desired_test_bytes
    )


def recognition_result_packet(
    *,
    request_id: int,
    image_id: int,
    status: StatusCode,
    person_id: str,
    person_name: str,
    similarity: float,
    processing_time_ms: int,
) -> bytes:
    pid = person_id.encode("utf-8")[:32]
    name = person_name.encode("utf-8")[:64]
    payload = (
        struct.pack("<BBBBfI", int(status), len(pid), len(name), 0, similarity, processing_time_ms)
        + pid
        + name
    )
    return make_packet(
        MessageType.RECOGNITION_RESULT, request_id=request_id, image_id=image_id, payload=payload
    )
