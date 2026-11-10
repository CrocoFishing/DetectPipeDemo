import asyncio
from pathlib import Path

import pytest

from pc_client.assembler import make_image_packets
from pc_client.ble_client import BleAdapterUnavailable, BleFaceSession
from pc_client.event_store import EventStore
from pc_client.protocol import (
    AudioStatus,
    EventAudioReady,
    EventComplete,
    EventOpen,
    EventStatus,
    FaceBatchEnd,
    FaceStatus,
    MessageType,
    ProtocolError,
    StatusCode,
    decode_packet,
    make_packet,
)


def event(sequence: int, boot: int = 37) -> int:
    return (boot << 32) | sequence


def test_multiple_batches_keep_all_attempts_and_allow_face_index_restart(tmp_path: Path) -> None:
    with EventStore(tmp_path / "events.db") as store:
        eid = event(1)
        for request, status in ((10, StatusCode.OK), (11, StatusCode.NO_FACE), (12, StatusCode.UNKNOWN)):
            store.open_event(EventOpen(eid), request)
            face(store, eid, request, request, status=status)
            batch_status = FaceStatus.FAILED if status is StatusCode.NO_FACE else FaceStatus.COMPLETED
            store.face_batch_end(
                FaceBatchEnd(
                    eid,
                    request,
                    1,
                    1,
                    int(status is StatusCode.OK),
                    int(status is StatusCode.UNKNOWN),
                    int(status is StatusCode.NO_FACE),
                    batch_status,
                ),
                request,
            )
        store.audio_ready(EventAudioReady(eid, AudioStatus.READY, 0, 4800000), 12)
        store.complete_event(
            EventComplete(eid, EventStatus.COMPLETE_WITH_ERROR, AudioStatus.READY, FaceStatus.COMPLETED), 12
        )
        assert [row["request_id"] for row in store.batches(eid)] == [10, 11, 12]
        assert [row["face_index"] for row in store.faces(eid)] == [0, 0, 0]
        assert [row["recognition_status"] for row in store.faces(eid)] == [0, 2, 1]
        store.open_event(EventOpen(eid), 10)  # Late exact duplicate cannot regress the latest batch.
        assert store.event(eid)["request_id"] == 12


def test_new_batches_require_previous_terminal_and_stop_after_audio(tmp_path: Path) -> None:
    with EventStore(tmp_path / "events.db") as store:
        eid = event(1)
        store.open_event(EventOpen(eid), 10)
        with pytest.raises(ProtocolError, match="pending"):
            store.open_event(EventOpen(eid), 11)
        store.face_batch_end(FaceBatchEnd(eid, 10, 0, 0, 0, 0, 0, FaceStatus.NO_FACE), 10)
        store.open_event(EventOpen(eid), 11)
        with pytest.raises(ProtocolError, match="another event"):
            store.open_event(EventOpen(event(2)), 10)
        store.face_batch_end(FaceBatchEnd(eid, 10, 0, 0, 0, 0, 0, FaceStatus.NO_FACE), 10)
        assert store.event(eid)["face_status"] == FaceStatus.PENDING
        store.audio_ready(EventAudioReady(eid, AudioStatus.READY, 0, 1), 11)
        with pytest.raises(ProtocolError, match="stale"):
            store.open_event(EventOpen(eid), 12)
        with pytest.raises(ProtocolError, match="before audio"):
            store.complete_event(
                EventComplete(eid, EventStatus.COMPLETE, AudioStatus.READY, FaceStatus.NO_FACE), 11
            )


def test_audio_shell_before_first_open_keeps_request_ownership(tmp_path: Path) -> None:
    with EventStore(tmp_path / "events.db") as store:
        eid = event(1)
        store.audio_ready(EventAudioReady(eid, AudioStatus.READY, 0, 1), 10)
        with pytest.raises(ProtocolError, match="another event"):
            store.open_event(EventOpen(event(2)), 10)
        store.open_event(EventOpen(eid), 10)
        store.face_batch_end(FaceBatchEnd(eid, 10, 0, 0, 0, 0, 0, FaceStatus.NO_FACE), 10)
        store.complete_event(
            EventComplete(eid, EventStatus.COMPLETE, AudioStatus.READY, FaceStatus.NO_FACE), 10
        )
        assert store.event(eid)["completed_at"] is not None


def test_legacy_store_migration_retains_faces_and_opens_next_batch(tmp_path: Path) -> None:
    import sqlite3

    path = tmp_path / "events.db"
    eid = event(1)
    with EventStore(path) as store:
        store.open_event(EventOpen(eid), 10)
        face(store, eid, 10, 1)
        store.face_batch_end(FaceBatchEnd(eid, 10, 1, 1, 0, 1, 0, FaceStatus.COMPLETED), 10)
    with sqlite3.connect(path) as connection:
        schema = connection.execute("SELECT sql FROM sqlite_master WHERE name='event_faces'").fetchone()[0]
        connection.execute("ALTER TABLE event_faces RENAME TO current_faces")
        connection.execute(
            schema.replace("UNIQUE(event_id, request_id, face_index)", "UNIQUE(event_id, face_index)")
        )
        connection.execute("INSERT INTO event_faces SELECT * FROM current_faces")
        connection.execute("DROP TABLE current_faces")
        connection.execute("DROP TABLE event_batches")
        connection.execute("PRAGMA user_version=1")
    with EventStore(path) as store:
        assert len(store.faces(eid)) == 1
        assert store.batches(eid)[0]["face_terminal_received"] == 1
        store.open_event(EventOpen(eid), 11)
        face(store, eid, 11, 2)
        assert [row["face_index"] for row in store.faces(eid)] == [0, 0]


def packet(kind: MessageType, request: int, value: object) -> bytes:
    return make_packet(kind, request_id=request, payload=value.encode())  # type: ignore[attr-defined]


def face(
    store: EventStore,
    eid: int,
    request: int,
    image: int,
    index: int = 0,
    count: int = 1,
    status: StatusCode = StatusCode.UNKNOWN,
) -> None:
    store.accept_face(event_id=eid, request_id=request, image_id=image, face_index=index, face_count=count)
    store.record_recognition(
        event_id=eid,
        request_id=request,
        image_id=image,
        status=status,
        person_id="P1" if status is StatusCode.OK else "",
        person_name="Alice" if status is StatusCode.OK else status.name,
        similarity=0.7 if status is StatusCode.OK else 0.3,
        processing_time_ms=52,
    )


@pytest.mark.parametrize("audio_first", [True, False])
def test_100_events_preserve_all_faces_and_both_completion_orders(tmp_path: Path, audio_first: bool) -> None:
    path = tmp_path / "events.db"
    with EventStore(path) as store:
        for seq in range(1, 101):
            eid = event(seq)
            request = 0x80000000 | seq
            store.open_event(EventOpen(eid), request)
            store.open_event(EventOpen(eid), request)  # controller and manager may repeat same OPEN
            for index, status in enumerate((StatusCode.OK, StatusCode.UNKNOWN, StatusCode.OK)):
                face(store, eid, request, seq * 10 + index, index, 3, status)
            audio = EventAudioReady(eid, AudioStatus.READY, seq * 1000, seq * 1000 + 500)
            batch = FaceBatchEnd(eid, request, 3, 3, 2, 1, 0, FaceStatus.COMPLETED)
            terminals = ((store.audio_ready, audio), (store.face_batch_end, batch))
            for handler, value in terminals if audio_first else reversed(terminals):
                handler(value, request)
            store.complete_event(
                EventComplete(eid, EventStatus.COMPLETE, AudioStatus.READY, FaceStatus.COMPLETED), request
            )
        assert (
            store.connection.execute("SELECT count(*) FROM events WHERE completed_at IS NOT NULL").fetchone()[
                0
            ]
            == 100
        )
        assert store.connection.execute("SELECT count(*) FROM event_faces").fetchone()[0] == 300
    with EventStore(path) as reopened:
        result = reopened.event(event(100))
        assert result["event_id"] == "0000002500000064"
        assert result["audio_file"] == "event_0000002500000064.wav"
        assert [f["recognition_status"] for f in reopened.faces(event(100))] == [0, 1, 0]


@pytest.mark.parametrize(
    "status",
    [FaceStatus.NO_FACE, FaceStatus.BUSY, FaceStatus.TIMEOUT, FaceStatus.DISCONNECTED, FaceStatus.FAILED],
)
def test_empty_failure_batches_keep_audio(tmp_path: Path, status: FaceStatus) -> None:
    with EventStore(tmp_path / "events.db") as store:
        eid = event(1)
        request = 0 if status is FaceStatus.BUSY else 10
        store.open_event(EventOpen(eid), request)
        store.audio_ready(EventAudioReady(eid, AudioStatus.READY, 0, 6400), request)
        store.face_batch_end(FaceBatchEnd(eid, request, 0, 0, 0, 0, 0, status), request)
        final = EventStatus.COMPLETE if status is FaceStatus.NO_FACE else EventStatus.COMPLETE_WITH_ERROR
        store.complete_event(EventComplete(eid, final, AudioStatus.READY, status), request)
        assert store.event(eid)["audio_file"]
        assert store.faces(eid) == []


def test_partial_disconnect_and_audio_failure_never_delete_faces(tmp_path: Path) -> None:
    with EventStore(tmp_path / "events.db") as store:
        eid = event(1)
        store.open_event(EventOpen(eid), 10)
        face(store, eid, 10, 1, count=3, status=StatusCode.UNKNOWN)
        store.mark_disconnected()
        assert store.event(eid)["face_status"] == FaceStatus.DISCONNECTED
        assert store.event(eid)["audio_status"] == AudioStatus.PENDING
        assert store.event(eid)["completed_at"] is None
        store.audio_ready(EventAudioReady(eid, AudioStatus.FAILED, 100, 200), 10)
        store.face_batch_end(FaceBatchEnd(eid, 10, 3, 1, 0, 1, 0, FaceStatus.PARTIAL), 10)
        store.complete_event(
            EventComplete(eid, EventStatus.COMPLETE_WITH_ERROR, AudioStatus.FAILED, FaceStatus.PARTIAL), 10
        )
        assert store.faces(eid)[0]["person_name"] == "UNKNOWN"
        assert store.event(eid)["disconnect_count"] == 1


def test_rapid_busy_event_does_not_steal_request_or_audio(tmp_path: Path) -> None:
    with EventStore(tmp_path / "events.db") as store:
        store.open_event(EventOpen(event(1)), 10)
        for seq in (2, 3):
            store.open_event(EventOpen(event(seq)), 0)
            store.face_batch_end(FaceBatchEnd(event(seq), 0, 0, 0, 0, 0, 0, FaceStatus.BUSY), 0)
            store.audio_ready(EventAudioReady(event(seq), AudioStatus.READY, 0, 300), 0)
            store.complete_event(
                EventComplete(
                    event(seq), EventStatus.COMPLETE_WITH_ERROR, AudioStatus.READY, FaceStatus.BUSY
                ),
                0,
            )
        assert store.event_for_request(10, "00000025") == "0000002500000001"
        assert store.event_for_request(0, "00000025") is None
        face(store, event(1), 10, 1)
        assert len(store.faces(event(1))) == 1


def test_boot_scoped_request_and_image_reuse_with_unsigned_id(tmp_path: Path) -> None:
    with EventStore(tmp_path / "events.db") as store:
        for boot in (37, 0xFFFFFFFE):
            eid = event(1, boot)
            store.open_event(EventOpen(eid), 10)
            face(store, eid, 10, 1)
        assert store.event(event(1, 0xFFFFFFFE))["event_id"] == "FFFFFFFE00000001"
        assert store.event_for_request(10, "FFFFFFFE") == "FFFFFFFE00000001"


def test_terminals_before_open_and_duplicate_notifications_are_idempotent(tmp_path: Path) -> None:
    with EventStore(tmp_path / "events.db") as store:
        eid = event(1)
        audio = EventAudioReady(eid, AudioStatus.READY, 10, 20)
        store.audio_ready(audio, 0)
        batch = FaceBatchEnd(eid, 10, 0, 0, 0, 0, 0, FaceStatus.NO_FACE)
        store.face_batch_end(batch, 10)
        store.open_event(EventOpen(eid), 10)
        store.audio_ready(audio, 10)
        store.face_batch_end(batch, 10)
        complete = EventComplete(eid, EventStatus.COMPLETE, AudioStatus.READY, FaceStatus.NO_FACE)
        store.complete_event(complete, 10)
        store.complete_event(complete, 10)
        store.open_event(EventOpen(eid), 10)
        with pytest.raises(ProtocolError, match="conflicting"):
            store.audio_ready(EventAudioReady(eid, AudioStatus.FAILED, 10, 20), 10)


def test_ownership_crossover_duplicates_stale_and_completion_preconditions(tmp_path: Path) -> None:
    with EventStore(tmp_path / "events.db") as store:
        first, second = event(1), event(2)
        store.open_event(EventOpen(first), 10)
        with pytest.raises(ProtocolError, match="another event"):
            store.open_event(EventOpen(second), 10)
        store.open_event(EventOpen(second), 20)
        with pytest.raises(ProtocolError, match="terminal request"):
            store.face_batch_end(FaceBatchEnd(first, 0, 0, 0, 0, 0, 0, FaceStatus.NO_FACE), 0)
        face(store, first, 10, 1)
        with pytest.raises(ProtocolError, match="already belongs"):
            store.accept_face(event_id=second, request_id=20, image_id=1, face_index=0, face_count=1)
        with pytest.raises(ProtocolError, match="request"):
            face(store, first, 20, 2)
        with pytest.raises(ProtocolError, match="accepted image"):
            store.record_recognition(
                event_id=second,
                request_id=20,
                image_id=1,
                status=StatusCode.UNKNOWN,
                person_id="",
                person_name="UNKNOWN",
                similarity=0.1,
                processing_time_ms=1,
            )
        with pytest.raises(ProtocolError, match="duplicate"):
            store.record_recognition(
                event_id=first,
                request_id=10,
                image_id=1,
                status=StatusCode.UNKNOWN,
                person_id="",
                person_name="UNKNOWN",
                similarity=0.1,
                processing_time_ms=1,
            )
        with pytest.raises(ProtocolError, match="before audio"):
            store.complete_event(
                EventComplete(first, EventStatus.COMPLETE, AudioStatus.READY, FaceStatus.COMPLETED), 10
            )
        store.audio_ready(EventAudioReady(first, AudioStatus.READY, 0, 1), 10)
        store.face_batch_end(FaceBatchEnd(first, 10, 1, 1, 0, 1, 0, FaceStatus.COMPLETED), 10)
        store.complete_event(
            EventComplete(first, EventStatus.COMPLETE, AudioStatus.READY, FaceStatus.COMPLETED), 10
        )
        with pytest.raises(ProtocolError, match="stale"):
            store.accept_face(event_id=first, request_id=10, image_id=2, face_index=0, face_count=1)
        assert len(store.faces(first)) == 1


def test_failed_recognition_is_retained(tmp_path: Path) -> None:
    with EventStore(tmp_path / "events.db") as store:
        eid = event(1)
        store.open_event(EventOpen(eid), 10)
        face(store, eid, 10, 1, status=StatusCode.FAILED)
        store.audio_ready(EventAudioReady(eid, AudioStatus.READY, 0, 50), 10)
        store.face_batch_end(FaceBatchEnd(eid, 10, 1, 1, 0, 0, 1, FaceStatus.FAILED), 10)
        store.complete_event(
            EventComplete(eid, EventStatus.COMPLETE_WITH_ERROR, AudioStatus.READY, FaceStatus.FAILED), 10
        )
        assert store.faces(eid)[0]["recognition_status"] == StatusCode.FAILED


class FakeClient:
    is_connected = True

    def __init__(self) -> None:
        self.writes: list[bytes] = []

    async def write_gatt_char(self, _: str, data: bytes, response: bool) -> None:
        self.writes.append(data)


def test_session_routes_multiple_requests_and_receiver_no_face(tmp_path: Path) -> None:
    received = []
    with EventStore(tmp_path / "events.db") as store:
        session = BleFaceSession("test", received.append, event_store=store)
        session.client = FakeClient()
        eid = event(1)
        for request, status in ((10, StatusCode.NO_FACE), (11, StatusCode.UNKNOWN)):
            session._notification(None, bytearray(packet(MessageType.EVENT_OPEN, request, EventOpen(eid))))
            begin, chunks, end = make_image_packets(b"jpeg", request, request, 2, face_index=0, face_count=1)
            for value in (begin, *chunks, end):
                session._notification(None, bytearray(value.encode()))
            image = received[-1]
            assert image.event_id == "0000002500000001"
            asyncio.run(
                session.send_recognition_result(
                    request, request, status, "", status.name, 0.0, 1, event_id=image.event_id
                )
            )
            assert session.complete_face(image, status) is not None
            failed = int(status is StatusCode.NO_FACE)
            batch = FaceBatchEnd(
                eid,
                request,
                1,
                1,
                0,
                1 - failed,
                failed,
                FaceStatus.FAILED if failed else FaceStatus.COMPLETED,
            )
            session._notification(None, bytearray(packet(MessageType.FACE_BATCH_END, request, batch)))
        session._notification(
            None,
            bytearray(
                packet(MessageType.EVENT_AUDIO_READY, 11, EventAudioReady(eid, AudioStatus.READY, 0, 1))
            ),
        )
        session._notification(
            None,
            bytearray(
                packet(
                    MessageType.EVENT_COMPLETE,
                    11,
                    EventComplete(
                        eid, EventStatus.COMPLETE_WITH_ERROR, AudioStatus.READY, FaceStatus.COMPLETED
                    ),
                )
            ),
        )
        assert len(store.batches(eid)) == 2 and len(store.faces(eid)) == 2
        assert session._event_requests == {}
        assert decode_packet(session.client.writes[0]).payload[0] == StatusCode.NO_FACE


def test_session_routes_existing_image_protocol_and_keeps_result_on_disconnect(tmp_path: Path) -> None:
    received = []
    with EventStore(tmp_path / "events.db") as store:
        session = BleFaceSession("test", received.append, event_store=store)
        session.client = FakeClient()
        eid = event(1)
        session._notification(None, bytearray(packet(MessageType.EVENT_OPEN, 10, EventOpen(eid))))
        begin, chunks, end = make_image_packets(b"jpeg", 10, 1, 2, face_index=0, face_count=2)
        for value in (begin, *chunks, end):
            session._notification(None, bytearray(value.encode()))
        assert received[0].event_id == "0000002500000001"
        asyncio.run(
            session.send_recognition_result(
                10, 1, StatusCode.UNKNOWN, "", "UNKNOWN", 0.2, 3, event_id=received[0].event_id
            )
        )
        assert store.faces(eid)[0]["result_sent"] == 1
        begin, chunks, end = make_image_packets(b"jpeg2", 10, 2, 2, face_index=1, face_count=2)
        for value in (begin, *chunks, end):
            session._notification(None, bytearray(value.encode()))
        session.client.is_connected = False
        session._disconnected(None)
        with pytest.raises(BleAdapterUnavailable):
            asyncio.run(
                session.send_recognition_result(
                    10, 2, StatusCode.OK, "P1", "Alice", 0.7, 4, event_id=received[1].event_id
                )
            )
        assert [f["recognition_status"] for f in store.faces(eid)] == [StatusCode.UNKNOWN, StatusCode.OK]
        assert store.faces(eid)[1]["result_sent"] == 0
        assert store.event(eid)["face_status"] == FaceStatus.DISCONNECTED
        assert decode_packet(session.client.writes[0]).header.message_type is MessageType.RECOGNITION_RESULT


def test_session_reboot_does_not_send_old_recognition_to_reused_request(tmp_path: Path) -> None:
    with EventStore(tmp_path / "events.db") as store:
        session = BleFaceSession("test", event_store=store)
        session.client = FakeClient()
        session._notification(None, bytearray(packet(MessageType.EVENT_OPEN, 10, EventOpen(event(1)))))
        store.accept_face(event_id=event(1), request_id=10, image_id=1, face_index=0, face_count=1)
        session._notification(None, bytearray(packet(MessageType.EVENT_OPEN, 10, EventOpen(event(1, 38)))))
        with pytest.raises(ProtocolError, match="previous boot"):
            asyncio.run(
                session.send_recognition_result(
                    10, 1, StatusCode.UNKNOWN, "", "UNKNOWN", 0.1, 1, event_id="0000002500000001"
                )
            )
        assert session.client.writes == []
        assert store.faces(event(1))[0]["recognition_status"] == StatusCode.UNKNOWN


def test_session_completed_maps_are_released_but_late_images_remain_stale(tmp_path: Path) -> None:
    with EventStore(tmp_path / "events.db") as store:
        session = BleFaceSession("test", event_store=store)
        eid = event(1)
        for kind, value in (
            (MessageType.EVENT_OPEN, EventOpen(eid)),
            (MessageType.EVENT_AUDIO_READY, EventAudioReady(eid, AudioStatus.READY, 0, 10)),
            (MessageType.FACE_BATCH_END, FaceBatchEnd(eid, 10, 0, 0, 0, 0, 0, FaceStatus.NO_FACE)),
            (
                MessageType.EVENT_COMPLETE,
                EventComplete(eid, EventStatus.COMPLETE, AudioStatus.READY, FaceStatus.NO_FACE),
            ),
        ):
            session._notification(None, bytearray(packet(kind, 10, value)))
        assert session._event_requests == {}
        assert session._open_events == set()
        assert session._event_for_request(10) == "0000002500000001"
        begin, _, _ = make_image_packets(b"late", 10, 1, 4)
        session._notification(None, bytearray(begin.encode()))
        assert session.last_image is None
        assert store.faces(eid) == []


def test_session_older_boot_open_does_not_replace_current_ownership(tmp_path: Path) -> None:
    with EventStore(tmp_path / "events.db") as store:
        session = BleFaceSession("test", event_store=store)
        newer, older = event(1, 38), event(1, 37)
        session._notification(None, bytearray(packet(MessageType.EVENT_OPEN, 10, EventOpen(newer))))
        session._notification(None, bytearray(packet(MessageType.EVENT_OPEN, 10, EventOpen(older))))
        assert session._event_for_request(10) == "0000002600000001"
        assert session._open_events == {"0000002600000001"}
        session._disconnected(None)
        assert store.event(newer)["disconnect_count"] == 1
        assert store.event(older)["disconnect_count"] == 0


def test_reconnect_manual_reused_request_does_not_inherit_previous_event(tmp_path: Path) -> None:
    received = []
    with EventStore(tmp_path / "events.db") as store:
        session = BleFaceSession("test", received.append, event_store=store)
        session.client = FakeClient()
        old, new = event(1, 37), event(1, 38)
        session._notification(None, bytearray(packet(MessageType.EVENT_OPEN, 10, EventOpen(old))))
        # A partial old batch retains an unsent recognition owned by boot 37.
        store.accept_face(event_id=old, request_id=10, image_id=1, face_index=0, face_count=2)
        session._owned_images[(10, 1)] = "0000002500000001"
        session._disconnected(None)
        assert session._boot_session is None
        assert session._event_requests == session._owned_images == {}
        assert session._event_for_request(10) is None
        assert store.event(old)["disconnect_count"] == 1
        # After reboot, a button/manual capture can reuse IDs before any new VAD.
        session.client = FakeClient()
        begin, chunks, end = make_image_packets(b"manual", 10, 1, 3)
        for value in (begin, *chunks, end):
            session._notification(None, bytearray(value.encode()))
        assert received[-1].event_id is None
        assert len(store.faces(old)) == 1
        assert store.faces(old)[0]["recognition_status"] is None
        # An old running recognizer cannot send its result to that manual request.
        with pytest.raises(ProtocolError, match="previous boot"):
            asyncio.run(
                session.send_recognition_result(
                    10, 1, StatusCode.UNKNOWN, "", "UNKNOWN", 0.1, 1, event_id="0000002500000001"
                )
            )
        assert session.client.writes == []
        assert store.faces(old)[0]["result_sent"] == 0
        # The next VAD OPEN establishes the new boot's durable association.
        session._notification(None, bytearray(packet(MessageType.EVENT_OPEN, 11, EventOpen(new))))
        begin, chunks, end = make_image_packets(b"new face", 11, 2, 3)
        for value in (begin, *chunks, end):
            session._notification(None, bytearray(value.encode()))
        assert received[-1].event_id == "0000002600000001"
        assert len(store.faces(new)) == 1
        assert store.faces(old)[0]["recognition_status"] == StatusCode.UNKNOWN
