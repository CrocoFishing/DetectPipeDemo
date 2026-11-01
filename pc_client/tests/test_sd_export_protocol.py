import struct

import pytest

from pc_client.protocol import (
    ErrorCode,
    MessageType,
    ProtocolError,
    SdExportState,
    SdExportStatus,
    decode_packet,
    make_packet,
    sd_export_cancel_packet,
    sd_export_get_status_packet,
    sd_export_start_packet,
)

SESSION = 0xFEDCBA9876543210


def test_sd_export_commands_have_exact_wire_shapes() -> None:
    start = decode_packet(sd_export_start_packet(1))
    assert start.header.message_type is MessageType.SD_EXPORT_START
    assert start.header.request_id == 1
    assert start.payload == b""
    cancel = decode_packet(sd_export_cancel_packet(2, SESSION))
    assert cancel.header.message_type is MessageType.SD_EXPORT_CANCEL
    assert cancel.payload == bytes.fromhex("1032547698BADCFE")
    status = decode_packet(sd_export_get_status_packet(3))
    assert status.header.message_type is MessageType.SD_EXPORT_GET_STATUS
    assert status.payload == bytes(8)
    assert decode_packet(sd_export_get_status_packet(4, SESSION)).payload == cancel.payload


@pytest.mark.parametrize("request_id", [0, -1, 0x80000000, 0x100000000])
def test_export_commands_reject_invalid_client_request_id(request_id: int) -> None:
    with pytest.raises(ValueError, match="request_id"):
        sd_export_start_packet(request_id)
    with pytest.raises(ValueError, match="request_id"):
        sd_export_cancel_packet(request_id, SESSION)
    with pytest.raises(ValueError, match="request_id"):
        sd_export_get_status_packet(request_id)


@pytest.mark.parametrize("session", [0, -1, 0x10000000000000000])
def test_cancel_rejects_invalid_session(session: int) -> None:
    with pytest.raises(ValueError, match="session_id"):
        sd_export_cancel_packet(1, session)


def test_sd_export_status_golden_layout_and_packet_metadata() -> None:
    value = SdExportStatus(SESSION, SdExportState.FAILED, ErrorCode.SD_IO)
    assert value.encode() == bytes.fromhex("1032547698BADCFE07001B0000000000")
    assert SdExportStatus.decode(value.encode()) == value
    packet = decode_packet(make_packet(MessageType.SD_EXPORT_STATUS, request_id=7, payload=value.encode()))
    assert SdExportStatus.from_packet(packet) == value
    assert len(packet.encode()) + 3 <= 67
    for key in ("flags", "image_id", "chunk_index", "total_chunks", "reserved"):
        bad = decode_packet(make_packet(MessageType.SD_EXPORT_STATUS, payload=value.encode(), **{key: 1}))
        with pytest.raises(ProtocolError, match="header"):
            SdExportStatus.from_packet(bad)


def test_export_status_rejects_truncation_padding_and_invalid_enums() -> None:
    encoded = SdExportStatus(SESSION, SdExportState.READY).encode()
    for data in (encoded[:-1], encoded + b"\0"):
        with pytest.raises(ProtocolError, match="length"):
            SdExportStatus.decode(data)
    for offset in (9, 12, 13, 14, 15):
        damaged = bytearray(encoded)
        damaged[offset] = 1
        with pytest.raises(ProtocolError, match="reserved"):
            SdExportStatus.decode(bytes(damaged))
    for state, error in ((8, 0), (2, 32)):
        with pytest.raises(ProtocolError, match="state or error"):
            SdExportStatus.decode(struct.pack("<QBBHI", SESSION, state, 0, error, 0))
    assert SdExportStatus.decode(bytes(16)) == SdExportStatus(0, SdExportState.IDLE)
