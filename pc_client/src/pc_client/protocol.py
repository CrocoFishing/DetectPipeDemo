from __future__ import annotations

import enum
import struct
import zlib
from dataclasses import dataclass

MAGIC = 0x45434146
VERSION = 1
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


class MessageFlag(enum.IntFlag):
    FACE_SEQUENCE = 0x0001


class Command(enum.IntEnum):
    START_CAPTURE = 1


class StatusCode(enum.IntEnum):
    OK = 0
    UNKNOWN = 1
    NO_FACE = 2
    FAILED = 3


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
