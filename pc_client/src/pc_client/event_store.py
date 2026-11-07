"""Durable Event 1 ownership, independent of the face identity database."""

from __future__ import annotations

import math
import sqlite3
from datetime import UTC, datetime
from pathlib import Path
from typing import Any

from .protocol import (
    AudioStatus,
    EventAudioReady,
    EventComplete,
    EventOpen,
    FaceBatchEnd,
    FaceStatus,
    MessageType,
    Packet,
    ProtocolError,
    StatusCode,
    event_id_hex,
)


def _now() -> str:
    return datetime.now(UTC).isoformat()


class EventStore:
    """IDs are unsigned 64-bit values stored as uppercase, fixed-width hex.

    Request/image counters restart at boot, so ownership uniqueness is scoped
    by the Event ID's boot session. No completed association is ever deleted.
    """

    def __init__(self, path: str | Path = "event_store.db") -> None:
        self.path = Path(path)
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.connection = sqlite3.connect(self.path)
        self.connection.row_factory = sqlite3.Row
        self.connection.execute("PRAGMA foreign_keys=ON")
        self.connection.executescript("""
            CREATE TABLE IF NOT EXISTS events(
                event_id TEXT PRIMARY KEY CHECK(length(event_id)=16 AND event_id=upper(event_id)),
                boot_session TEXT NOT NULL,
                request_id INTEGER,
                trigger_type INTEGER,
                opened_at TEXT,
                completed_at TEXT,
                audio_status INTEGER NOT NULL DEFAULT 0,
                audio_file TEXT,
                audio_first_sample TEXT,
                audio_last_sample TEXT,
                face_status INTEGER NOT NULL DEFAULT 0,
                face_count INTEGER,
                completed_face_count INTEGER,
                recognized_count INTEGER,
                unknown_count INTEGER,
                failed_count INTEGER,
                event_status INTEGER,
                audio_terminal_received INTEGER NOT NULL DEFAULT 0,
                face_terminal_received INTEGER NOT NULL DEFAULT 0,
                disconnect_count INTEGER NOT NULL DEFAULT 0,
                UNIQUE(boot_session, request_id)
            );
            CREATE TABLE IF NOT EXISTS event_faces(
                event_id TEXT NOT NULL REFERENCES events(event_id),
                boot_session TEXT NOT NULL,
                request_id INTEGER NOT NULL,
                image_id INTEGER NOT NULL,
                face_index INTEGER NOT NULL,
                face_count INTEGER NOT NULL,
                received_at TEXT NOT NULL,
                recognition_status INTEGER,
                person_id TEXT,
                person_name TEXT,
                similarity REAL,
                processing_time_ms INTEGER,
                recognized_at TEXT,
                result_sent INTEGER NOT NULL DEFAULT 0,
                PRIMARY KEY(event_id, image_id),
                UNIQUE(boot_session, image_id),
                UNIQUE(event_id, face_index)
            );
        """)
        self.connection.commit()

    def close(self) -> None:
        self.connection.close()

    def __enter__(self) -> EventStore:
        return self

    def __exit__(self, *_: object) -> None:
        self.close()

    def event(self, event_id: int | str) -> dict[str, Any]:
        key = event_id_hex(event_id) if isinstance(event_id, int) else event_id
        row = self.connection.execute("SELECT * FROM events WHERE event_id=?", (key,)).fetchone()
        if row is None:
            raise ProtocolError("unknown event_id")
        return dict(row)

    def faces(self, event_id: int | str) -> list[dict[str, Any]]:
        key = event_id_hex(event_id) if isinstance(event_id, int) else event_id
        return [
            dict(row)
            for row in self.connection.execute(
                "SELECT * FROM event_faces WHERE event_id=? ORDER BY face_index", (key,)
            )
        ]

    def event_for_request(self, request_id: int, boot_session: str) -> str | None:
        if request_id == 0:
            return None
        row = self.connection.execute(
            "SELECT event_id FROM events WHERE boot_session=? AND request_id=?",
            (boot_session, request_id),
        ).fetchone()
        return str(row[0]) if row else None

    def _ensure_event(self, event_id: int, request_id: int) -> dict[str, Any]:
        key = event_id_hex(event_id)
        if not 0 <= request_id <= 0xFFFFFFFF:
            raise ProtocolError("invalid request_id")
        row = self.connection.execute("SELECT * FROM events WHERE event_id=?", (key,)).fetchone()
        if row is None:
            owner = self.event_for_request(request_id, key[:8])
            if owner is not None and owner != key:
                raise ProtocolError("request_id already belongs to another event")
            self.connection.execute(
                "INSERT INTO events(event_id,boot_session,request_id) VALUES(?,?,?)",
                (key, key[:8], request_id or None),
            )
        else:
            current = row["request_id"]
            if current is not None and request_id not in (0, current):
                raise ProtocolError("request_id disagrees with event ownership")
            if current is None and request_id:
                if row["completed_at"] is not None:
                    raise ProtocolError("cannot bind request to completed event")
                owner = self.event_for_request(request_id, key[:8])
                if owner is not None and owner != key:
                    raise ProtocolError("request_id already belongs to another event")
                self.connection.execute("UPDATE events SET request_id=? WHERE event_id=?", (request_id, key))
        return self.event(key)

    def open_event(self, value: EventOpen, request_id: int) -> None:
        # Decode local values too, so callers cannot bypass wire validation.
        value = EventOpen.decode(value.encode())
        with self.connection:
            row = self._ensure_event(value.event_id, request_id)
            if row["trigger_type"] is not None:
                if row["trigger_type"] != value.trigger_type or (row["request_id"] or 0) != request_id:
                    raise ProtocolError("conflicting duplicate EVENT_OPEN")
                return
            self.connection.execute(
                "UPDATE events SET trigger_type=?,opened_at=? WHERE event_id=?",
                (int(value.trigger_type), _now(), row["event_id"]),
            )

    def audio_ready(self, value: EventAudioReady, request_id: int) -> None:
        value = EventAudioReady.decode(value.encode())
        with self.connection:
            row = self._ensure_event(value.event_id, request_id)
            desired = (int(value.audio_status), str(value.audio_first_sample), str(value.audio_last_sample))
            if row["audio_terminal_received"]:
                actual = (row["audio_status"], row["audio_first_sample"], row["audio_last_sample"])
                if actual != desired:
                    raise ProtocolError("conflicting duplicate audio terminal")
                return
            if row["completed_at"] is not None:
                raise ProtocolError("stale audio terminal")
            self.connection.execute(
                "UPDATE events SET audio_status=?,audio_first_sample=?,audio_last_sample=?,"
                "audio_file=?,audio_terminal_received=1 WHERE event_id=?",
                (
                    *desired,
                    f"event_{row['event_id']}.wav" if value.audio_status is AudioStatus.READY else None,
                    row["event_id"],
                ),
            )

    def face_batch_end(self, value: FaceBatchEnd, request_id: int) -> None:
        value = FaceBatchEnd.decode(value.encode())
        if value.request_id != request_id:
            raise ProtocolError("FACE_BATCH_END request payload/header mismatch")
        with self.connection:
            row = self._ensure_event(value.event_id, request_id)
            if (row["request_id"] or 0) != request_id:
                raise ProtocolError("face terminal request disagrees with event ownership")
            columns = (
                "face_count",
                "completed_face_count",
                "recognized_count",
                "unknown_count",
                "failed_count",
                "face_status",
            )
            desired = (
                value.expected_face_count,
                value.completed_face_count,
                value.recognized_count,
                value.unknown_count,
                value.failed_count,
                int(value.face_status),
            )
            if row["face_terminal_received"]:
                if tuple(row[column] for column in columns) != desired:
                    raise ProtocolError("conflicting duplicate face terminal")
                return
            if row["completed_at"] is not None:
                raise ProtocolError("stale face terminal")
            faces = self.faces(row["event_id"])
            if faces and any(face["face_count"] != value.expected_face_count for face in faces):
                raise ProtocolError("terminal face_count disagrees with images")
            if len(faces) > value.expected_face_count:
                raise ProtocolError("terminal face_count smaller than received images")
            self.connection.execute(
                "UPDATE events SET face_count=?,completed_face_count=?,recognized_count=?,unknown_count=?,"
                "failed_count=?,face_status=?,face_terminal_received=1 WHERE event_id=?",
                (*desired, row["event_id"]),
            )

    def complete_event(self, value: EventComplete, request_id: int) -> None:
        value = EventComplete.decode(value.encode())
        with self.connection:
            row = self._ensure_event(value.event_id, request_id)
            if (row["request_id"] or 0) != request_id:
                raise ProtocolError("completion request disagrees with event ownership")
            if row["completed_at"] is not None:
                if (row["event_status"], row["audio_status"], row["face_status"]) != (
                    int(value.event_status),
                    int(value.audio_status),
                    int(value.face_status),
                ):
                    raise ProtocolError("conflicting duplicate EVENT_COMPLETE")
                return
            if not row["audio_terminal_received"] or not row["face_terminal_received"]:
                raise ProtocolError("EVENT_COMPLETE before audio and face terminals")
            if (row["audio_status"], row["face_status"]) != (value.audio_status, value.face_status):
                raise ProtocolError("completion disagrees with terminal branches")
            self.connection.execute(
                "UPDATE events SET event_status=?,completed_at=? WHERE event_id=?",
                (int(value.event_status), _now(), row["event_id"]),
            )

    def handle_packet(self, packet: Packet) -> bool:
        kind = packet.header.message_type
        if kind not in (
            MessageType.EVENT_OPEN,
            MessageType.EVENT_AUDIO_READY,
            MessageType.FACE_BATCH_END,
            MessageType.EVENT_COMPLETE,
        ):
            return False
        if (
            packet.header.image_id
            or packet.header.flags
            or packet.header.reserved
            or packet.header.chunk_index
            or packet.header.total_chunks
        ):
            raise ProtocolError("invalid event header metadata")
        if kind is MessageType.EVENT_OPEN:
            self.open_event(EventOpen.decode(packet.payload), packet.header.request_id)
        elif kind is MessageType.EVENT_AUDIO_READY:
            self.audio_ready(EventAudioReady.decode(packet.payload), packet.header.request_id)
        elif kind is MessageType.FACE_BATCH_END:
            self.face_batch_end(FaceBatchEnd.decode(packet.payload), packet.header.request_id)
        else:
            self.complete_event(EventComplete.decode(packet.payload), packet.header.request_id)
        return True

    def accept_face(
        self, *, event_id: int | str, request_id: int, image_id: int, face_index: int, face_count: int
    ) -> None:
        row = self.event(event_id)
        if not request_id or row["request_id"] != request_id:
            raise ProtocolError("face request does not belong to event")
        if row["opened_at"] is None:
            raise ProtocolError("face image arrived before EVENT_OPEN")
        if row["completed_at"] is not None or row["face_terminal_received"]:
            raise ProtocolError("stale image for terminal face batch")
        if not 0 < image_id <= 0xFFFFFFFF or not 0 < face_count <= 0xFFFF or not 0 <= face_index < face_count:
            raise ProtocolError("invalid face identifiers or sequence")
        faces = self.faces(row["event_id"])
        if face_index != len(faces):
            raise ProtocolError("face_index is missing, duplicated, or out of order")
        if faces and faces[0]["face_count"] != face_count:
            raise ProtocolError("face_count changed within event")
        with self.connection:
            try:
                self.connection.execute(
                    "INSERT INTO event_faces(event_id,boot_session,request_id,image_id,face_index,face_count,"
                    "received_at) VALUES(?,?,?,?,?,?,?)",
                    (
                        row["event_id"],
                        row["boot_session"],
                        request_id,
                        image_id,
                        face_index,
                        face_count,
                        _now(),
                    ),
                )
            except sqlite3.IntegrityError as exc:
                raise ProtocolError("image_id already belongs to another request or face") from exc

    def record_recognition(
        self,
        *,
        event_id: int | str,
        request_id: int,
        image_id: int,
        status: StatusCode,
        person_id: str,
        person_name: str,
        similarity: float,
        processing_time_ms: int,
    ) -> None:
        row = self.event(event_id)
        if row["request_id"] != request_id:
            raise ProtocolError("recognition request does not belong to event")
        face = self.connection.execute(
            "SELECT * FROM event_faces WHERE event_id=? AND request_id=? AND image_id=?",
            (row["event_id"], request_id, image_id),
        ).fetchone()
        if face is None:
            raise ProtocolError("recognition does not belong to an accepted image")
        if face["recognition_status"] is not None:
            raise ProtocolError("duplicate recognition result")
        # A local disconnect may precede completion of a running recognizer;
        # retain its result, while the device's authoritative terminal stays fixed.
        if row["completed_at"] is not None or row["face_terminal_received"]:
            raise ProtocolError("stale recognition for terminal event")
        if status not in (StatusCode.OK, StatusCode.UNKNOWN, StatusCode.FAILED):
            raise ProtocolError("invalid face recognition status")
        if not math.isfinite(similarity) or processing_time_ms < 0:
            raise ProtocolError("invalid recognition metrics")
        with self.connection:
            self.connection.execute(
                "UPDATE event_faces SET recognition_status=?,person_id=?,person_name=?,similarity=?,"
                "processing_time_ms=?,recognized_at=? WHERE event_id=? AND image_id=?",
                (
                    int(status),
                    person_id,
                    person_name,
                    similarity,
                    processing_time_ms,
                    _now(),
                    row["event_id"],
                    image_id,
                ),
            )

    def mark_result_sent(self, event_id: str, request_id: int, image_id: int) -> None:
        with self.connection:
            cursor = self.connection.execute(
                "UPDATE event_faces SET result_sent=1 WHERE event_id=? AND request_id=? AND image_id=? "
                "AND recognition_status IS NOT NULL",
                (event_id, request_id, image_id),
            )
            if cursor.rowcount != 1:
                raise ProtocolError("cannot mark unowned recognition as sent")

    def mark_disconnected(self, event_ids: set[str] | None = None) -> None:
        # This is an observation, not a replacement for a missing device terminal.
        # Pending audio remains pending because audio recording continues locally.
        with self.connection:
            if event_ids is None:
                rows = self.connection.execute("SELECT event_id FROM events WHERE completed_at IS NULL")
                event_ids = {str(row[0]) for row in rows}
            for key in event_ids:
                self.connection.execute(
                    "UPDATE events SET disconnect_count=disconnect_count+1,"
                    "face_status=CASE WHEN face_terminal_received=0 THEN ? ELSE face_status END "
                    "WHERE event_id=? AND completed_at IS NULL",
                    (int(FaceStatus.DISCONNECTED), key),
                )
