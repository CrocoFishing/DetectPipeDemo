import struct

import pytest

from pc_client.protocol import (
    AudioStatus,
    EventAudioReady,
    EventComplete,
    EventOpen,
    EventStatus,
    FaceBatchEnd,
    FaceStatus,
    ProtocolError,
    event_id_hex,
)

EID = 0xFEDCBA9876543210


def test_event_payload_golden_layouts_keep_uint64_and_padding() -> None:
    opened = EventOpen(EID)
    assert opened.encode() == bytes.fromhex("1032547698BADCFE0300000000000000")
    assert EventOpen.decode(opened.encode()) == opened
    audio = EventAudioReady(EID, AudioStatus.READY, 0x1122334455667788, 0x8877665544332211)
    assert len(audio.encode()) == 32
    assert audio.encode()[16:] == bytes.fromhex("88776655443322111122334455667788")
    assert EventAudioReady.decode(audio.encode()) == audio
    batch = FaceBatchEnd(EID, 0x80000012, 3, 3, 1, 1, 1, FaceStatus.FAILED)
    assert batch.encode() == bytes.fromhex("1032547698BADCFE120000800300030001000100010007") + bytes(9)
    assert FaceBatchEnd.decode(batch.encode()) == batch
    complete = EventComplete(EID, EventStatus.COMPLETE_WITH_ERROR, AudioStatus.READY, FaceStatus.FAILED)
    assert complete.encode() == bytes.fromhex("1032547698BADCFE0101070000000000")
    assert EventComplete.decode(complete.encode()) == complete
    assert event_id_hex(EID) == "FEDCBA9876543210"


@pytest.mark.parametrize(
    "value,reserved_start",
    [
        (EventOpen(EID), 9),
        (EventAudioReady(EID, AudioStatus.READY, 0, 1), 9),
        (FaceBatchEnd(EID, 1, 0, 0, 0, 0, 0, FaceStatus.NO_FACE), 23),
        (EventComplete(EID, EventStatus.COMPLETE, AudioStatus.READY, FaceStatus.NO_FACE), 11),
    ],
)
def test_event_payloads_reject_truncation_nonzero_padding_and_zero_id(
    value: object, reserved_start: int
) -> None:
    encoded = value.encode()  # type: ignore[attr-defined]
    decoder = type(value).decode  # type: ignore[attr-defined]
    with pytest.raises(ProtocolError, match="length"):
        decoder(encoded[:-1])
    damaged = bytearray(encoded)
    damaged[reserved_start] = 1
    with pytest.raises(ProtocolError, match="reserved"):
        decoder(bytes(damaged))
    with pytest.raises(ProtocolError, match="event_id"):
        decoder(bytes(8) + encoded[8:])


def test_invalid_status_and_counter_payloads_are_rejected() -> None:
    with pytest.raises(ProtocolError, match="terminal audio"):
        EventAudioReady.decode(EventAudioReady(EID, AudioStatus.PENDING, 0, 1).encode())
    with pytest.raises(ProtocolError, match="terminal audio"):
        EventAudioReady.decode(EventAudioReady(EID, AudioStatus.READY, 2, 1).encode())
    with pytest.raises(ProtocolError, match="terminal face counters"):
        FaceBatchEnd.decode(FaceBatchEnd(EID, 1, 3, 2, 1, 0, 0, FaceStatus.PARTIAL).encode())
    with pytest.raises(ProtocolError, match="completed face"):
        FaceBatchEnd.decode(FaceBatchEnd(EID, 1, 3, 2, 1, 1, 0, FaceStatus.COMPLETED).encode())
    with pytest.raises(ProtocolError, match="before both"):
        EventComplete.decode(
            EventComplete(EID, EventStatus.COMPLETE, AudioStatus.PENDING, FaceStatus.COMPLETED).encode()
        )
    with pytest.raises(ProtocolError, match="disagrees"):
        EventComplete.decode(
            EventComplete(EID, EventStatus.COMPLETE, AudioStatus.FAILED, FaceStatus.COMPLETED).encode()
        )
    with pytest.raises(ProtocolError, match="trigger"):
        EventOpen.decode(struct.pack("<QB7x", EID, 99))


def test_prior_batch_error_is_valid_with_successful_latest_batch() -> None:
    value = EventComplete(EID, EventStatus.COMPLETE_WITH_ERROR, AudioStatus.READY, FaceStatus.COMPLETED)
    assert EventComplete.decode(value.encode()) == value
