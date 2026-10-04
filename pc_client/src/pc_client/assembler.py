from __future__ import annotations

import struct
import time
from dataclasses import dataclass, field

from .protocol import (
    MessageFlag,
    MessageType,
    Packet,
    ProtocolError,
    crc32,
    pack_face_sequence,
    unpack_face_sequence,
)


@dataclass(slots=True)
class ImageAssembler:
    timeout_s: float = 15.0
    request_id: int = 0
    image_id: int = 0
    face_index: int = 0
    face_count: int = 1
    expected_bytes: int = 0
    expected_crc32: int = 0
    total_chunks: int = 0
    started_at: float = 0.0
    chunks: dict[int, bytes] = field(default_factory=dict)
    duplicated_sequence_count: int = 0
    out_of_order_count: int = 0
    _next_sequence: int = 0
    _face_sequence_flagged: bool = False

    def begin(self, packet: Packet) -> None:
        if packet.header.message_type is not MessageType.IMAGE_BEGIN or len(packet.payload) != 8:
            raise ProtocolError("invalid IMAGE_BEGIN")
        self.request_id = packet.header.request_id
        self.image_id = packet.header.image_id
        self._face_sequence_flagged = bool(packet.header.flags & MessageFlag.FACE_SEQUENCE)
        self.face_index, self.face_count = unpack_face_sequence(packet.header)
        self.expected_bytes, self.expected_crc32 = struct.unpack("<II", packet.payload)
        self.total_chunks = 0
        self.started_at = time.monotonic()
        self.chunks.clear()
        self.duplicated_sequence_count = 0
        self.out_of_order_count = 0
        self._next_sequence = 0

    def add(self, packet: Packet) -> None:
        if packet.header.message_type is not MessageType.IMAGE_CHUNK:
            raise ProtocolError("expected IMAGE_CHUNK")
        if packet.header.request_id != self.request_id or packet.header.image_id != self.image_id:
            raise ProtocolError("incorrect request_id or image_id")
        if self.expired():
            raise TimeoutError("image reassembly timeout")
        index = packet.header.chunk_index
        if packet.header.total_chunks == 0 or index >= packet.header.total_chunks:
            raise ProtocolError("invalid chunk sequence")
        if self.total_chunks and packet.header.total_chunks != self.total_chunks:
            raise ProtocolError("total_chunks changed")
        self.total_chunks = packet.header.total_chunks
        if index in self.chunks:
            self.duplicated_sequence_count += 1
            if self.chunks[index] != packet.payload:
                raise ProtocolError("duplicate chunk content mismatch")
            return
        if index != self._next_sequence:
            self.out_of_order_count += 1
        self.chunks[index] = packet.payload
        while self._next_sequence in self.chunks:
            self._next_sequence += 1

    def finish(self, packet: Packet) -> bytes:
        if packet.header.message_type is not MessageType.IMAGE_END or len(packet.payload) != 8:
            raise ProtocolError("invalid IMAGE_END")
        if packet.header.request_id != self.request_id or packet.header.image_id != self.image_id:
            raise ProtocolError("incorrect IMAGE_END ids")
        end_flagged = bool(packet.header.flags & MessageFlag.FACE_SEQUENCE)
        if end_flagged != self._face_sequence_flagged:
            raise ProtocolError("IMAGE_END face sequence flag mismatch")
        if unpack_face_sequence(packet.header) != (self.face_index, self.face_count):
            raise ProtocolError("IMAGE_END face sequence metadata mismatch")
        if self.expired():
            raise TimeoutError("image reassembly timeout")
        end_bytes, end_crc = struct.unpack("<II", packet.payload)
        if (end_bytes, end_crc) != (self.expected_bytes, self.expected_crc32):
            raise ProtocolError("IMAGE_END metadata mismatch")
        missing = self.missing_sequences()
        if missing:
            raise ProtocolError(f"missing chunks: {missing}")
        image = b"".join(self.chunks[i] for i in range(self.total_chunks))
        if len(image) != self.expected_bytes:
            raise ProtocolError("image length mismatch")
        if crc32(image) != self.expected_crc32:
            raise ProtocolError("image CRC mismatch")
        return image

    def missing_sequences(self) -> list[int]:
        return [i for i in range(self.total_chunks) if i not in self.chunks]

    def expired(self) -> bool:
        return bool(self.started_at and time.monotonic() - self.started_at > self.timeout_s)


def make_image_packets(
    data: bytes,
    request_id: int,
    image_id: int,
    chunk_size: int,
    *,
    face_index: int | None = None,
    face_count: int | None = None,
) -> tuple[Packet, list[Packet], Packet]:
    from .protocol import decode_packet, make_packet

    if (face_index is None) != (face_count is None):
        raise ValueError("face_index and face_count must be provided together")
    flags, reserved = (0, 0)
    if face_index is not None and face_count is not None:
        flags, reserved = pack_face_sequence(face_index, face_count)
    metadata = struct.pack("<II", len(data), crc32(data))
    begin = decode_packet(
        make_packet(
            MessageType.IMAGE_BEGIN,
            request_id=request_id,
            image_id=image_id,
            payload=metadata,
            flags=flags,
            reserved=reserved,
        )
    )
    count = (len(data) + chunk_size - 1) // chunk_size
    chunks = [
        decode_packet(
            make_packet(
                MessageType.IMAGE_CHUNK,
                request_id=request_id,
                image_id=image_id,
                payload=data[i * chunk_size : (i + 1) * chunk_size],
                chunk_index=i,
                total_chunks=count,
            )
        )
        for i in range(count)
    ]
    end = decode_packet(
        make_packet(
            MessageType.IMAGE_END,
            request_id=request_id,
            image_id=image_id,
            payload=metadata,
            flags=flags,
            reserved=reserved,
        )
    )
    return begin, chunks, end
