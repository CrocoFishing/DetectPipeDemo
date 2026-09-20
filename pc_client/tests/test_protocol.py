import struct
import time

import pytest

from pc_client.assembler import ImageAssembler, make_image_packets
from pc_client.protocol import (
    HEADER_SIZE,
    MAGIC,
    MessageFlag,
    MessageType,
    PartialPacket,
    ProtocolError,
    decode_packet,
    make_packet,
    pack_face_sequence,
)


def test_serialize_deserialize() -> None:
    encoded = make_packet(MessageType.PING, request_id=9, image_id=4, payload=b"hello", flags=3)
    packet = decode_packet(encoded)
    assert packet.payload == b"hello"
    assert packet.header.request_id == 9
    assert packet.header.flags == 3


def test_partial_packet() -> None:
    with pytest.raises(PartialPacket):
        decode_packet(b"\0" * (HEADER_SIZE - 1))
    packet = make_packet(MessageType.PING, payload=b"abcd")
    with pytest.raises(PartialPacket):
        decode_packet(packet[:-1])


def test_invalid_magic() -> None:
    packet = bytearray(make_packet(MessageType.PING))
    packet[:4] = struct.pack("<I", MAGIC ^ 1)
    with pytest.raises(ProtocolError, match="magic"):
        decode_packet(packet)


def test_unsupported_version() -> None:
    packet = bytearray(make_packet(MessageType.PING))
    packet[4] = 99
    with pytest.raises(ProtocolError, match="version"):
        decode_packet(packet)


def test_invalid_length() -> None:
    packet = make_packet(MessageType.PING) + b"extra"
    with pytest.raises(ProtocolError, match="length"):
        decode_packet(packet)


def test_crc_mismatch() -> None:
    packet = bytearray(make_packet(MessageType.PING, payload=b"abc"))
    packet[-1] ^= 0x80
    with pytest.raises(ProtocolError, match="CRC"):
        decode_packet(packet)


def test_unknown_message_type() -> None:
    packet = bytearray(make_packet(MessageType.PING))
    packet[5] = 200
    with pytest.raises(ProtocolError, match="unknown"):
        decode_packet(packet)


def test_image_reassembly_out_of_order_and_duplicate() -> None:
    data = bytes(range(251)) * 4
    begin, chunks, end = make_image_packets(data, 7, 11, 100)
    assembler = ImageAssembler()
    assembler.begin(begin)
    assembler.add(chunks[1])
    assembler.add(chunks[0])
    assembler.add(chunks[1])
    for chunk in chunks[2:]:
        assembler.add(chunk)
    assert assembler.finish(end) == data
    assert assembler.duplicated_sequence_count == 1
    assert assembler.out_of_order_count >= 1


def test_missing_chunk() -> None:
    begin, chunks, end = make_image_packets(b"x" * 500, 1, 2, 100)
    assembler = ImageAssembler()
    assembler.begin(begin)
    for chunk in chunks[:-1]:
        assembler.add(chunk)
    with pytest.raises(ProtocolError, match="missing"):
        assembler.finish(end)


def test_reassembly_crc_failure() -> None:
    begin, chunks, end = make_image_packets(b"x" * 500, 1, 2, 100)
    assembler = ImageAssembler()
    assembler.begin(begin)
    chunks[0] = decode_packet(
        make_packet(
            MessageType.IMAGE_CHUNK,
            request_id=1,
            image_id=2,
            payload=b"y" * 100,
            chunk_index=0,
            total_chunks=5,
        )
    )
    for chunk in chunks:
        assembler.add(chunk)
    with pytest.raises(ProtocolError, match="CRC"):
        assembler.finish(end)


def test_reassembly_timeout() -> None:
    begin, chunks, end = make_image_packets(b"test", 1, 2, 2)
    assembler = ImageAssembler(timeout_s=0.01)
    assembler.begin(begin)
    assembler.started_at = time.monotonic() - 1
    with pytest.raises(TimeoutError):
        assembler.add(chunks[0])
    with pytest.raises(TimeoutError):
        assembler.finish(end)


def test_face_sequence_metadata_round_trip_and_legacy_default() -> None:
    data = b"face-image"
    begin, chunks, end = make_image_packets(data, 7, 11, 4, face_index=1, face_count=3)
    assert begin.header.flags & MessageFlag.FACE_SEQUENCE
    assembler = ImageAssembler()
    assembler.begin(begin)
    assert (assembler.face_index, assembler.face_count) == (1, 3)
    for chunk in chunks:
        assembler.add(chunk)
    assert assembler.finish(end) == data

    legacy_begin, _, _ = make_image_packets(data, 8, 12, 4)
    assembler.begin(legacy_begin)
    assert (assembler.face_index, assembler.face_count) == (0, 1)


def test_invalid_face_sequence_metadata_is_rejected() -> None:
    flags, _ = pack_face_sequence(0, 2)
    metadata = struct.pack("<II", 1, 0)
    invalid = decode_packet(
        make_packet(
            MessageType.IMAGE_BEGIN,
            request_id=1,
            image_id=2,
            payload=metadata,
            flags=flags,
            reserved=1,
        )
    )
    with pytest.raises(ProtocolError, match="face sequence"):
        ImageAssembler().begin(invalid)


def test_image_end_face_sequence_mismatch_is_rejected() -> None:
    begin, chunks, _ = make_image_packets(b"abc", 1, 2, 3, face_index=0, face_count=2)
    _, _, mismatched_end = make_image_packets(b"abc", 1, 2, 3, face_index=1, face_count=2)
    assembler = ImageAssembler()
    assembler.begin(begin)
    assembler.add(chunks[0])
    with pytest.raises(ProtocolError, match="metadata mismatch"):
        assembler.finish(mismatched_end)
