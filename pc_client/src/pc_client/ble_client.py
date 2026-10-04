from __future__ import annotations

import asyncio
import csv
import logging
import time
from collections.abc import Callable
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any

from .assembler import ImageAssembler
from .event_store import EventStore
from .protocol import (
    CONTROL_UUID,
    EVENT_UUID,
    IMAGE_UUID,
    RESULT_UUID,
    EventOpen,
    MessageType,
    Packet,
    ProtocolError,
    StatusCode,
    decode_packet,
    event_id_hex,
    recognition_result_packet,
    start_capture_packet,
)

LOG = logging.getLogger(__name__)


class BleAdapterUnavailable(RuntimeError):
    pass


@dataclass(slots=True)
class TransferMetrics:
    timestamp: str
    device_name: str
    request_id: int
    image_id: int
    image_bytes: int
    chunk_count: int
    transfer_duration_ms: float
    application_throughput_kbps: float
    rssi_current_dbm: int | None
    rssi_min_dbm: int | None
    rssi_max_dbm: int | None
    rssi_mean_dbm: float | None
    rssi_stddev_dbm: float | None
    negotiated_mtu: int | None
    missing_sequence_count: int
    duplicated_sequence_count: int
    crc_failure_count: int
    disconnect_count: int
    reconnect_count: int
    transfer_success: bool
    retry_count: int
    queue_congestion_count: int


@dataclass(frozen=True, slots=True)
class ReceivedFaceImage:
    request_id: int
    image_id: int
    face_index: int
    face_count: int
    jpeg: bytes
    event_id: str | None = None


@dataclass(frozen=True, slots=True)
class RecognitionBatchSummary:
    request_id: int
    face_count: int
    ok_count: int
    unknown_count: int
    failed_count: int


@dataclass(slots=True)
class _FaceBatchProgress:
    face_count: int
    next_index: int = 0
    image_ids: set[int] = field(default_factory=set)
    completed_image_ids: set[int] = field(default_factory=set)
    ok_count: int = 0
    unknown_count: int = 0
    failed_count: int = 0


class FaceBatchTracker:
    def __init__(self) -> None:
        self._batches: dict[int, _FaceBatchProgress] = {}

    def accept(self, image: ReceivedFaceImage) -> None:
        if image.face_count <= 0 or not 0 <= image.face_index < image.face_count:
            raise ProtocolError("invalid face sequence metadata")
        progress = self._batches.get(image.request_id)
        if progress is None:
            if image.face_index != 0:
                raise ProtocolError("face batch must begin at index 0")
            progress = _FaceBatchProgress(face_count=image.face_count)
            self._batches[image.request_id] = progress
        if image.face_count != progress.face_count:
            raise ProtocolError("face_count changed within request")
        if image.face_index != progress.next_index:
            raise ProtocolError("face_index is missing, duplicated, or out of order")
        if image.image_id in progress.image_ids:
            raise ProtocolError("duplicate image_id within face batch")
        progress.image_ids.add(image.image_id)
        progress.next_index += 1

    def complete(self, image: ReceivedFaceImage, status: StatusCode) -> RecognitionBatchSummary | None:
        progress = self._batches.get(image.request_id)
        if progress is None or image.image_id not in progress.image_ids:
            raise ProtocolError("result does not belong to an accepted face image")
        if image.image_id in progress.completed_image_ids:
            raise ProtocolError("duplicate face recognition result")
        progress.completed_image_ids.add(image.image_id)
        if status == StatusCode.OK:
            progress.ok_count += 1
        elif status == StatusCode.UNKNOWN:
            progress.unknown_count += 1
        else:
            progress.failed_count += 1
        if len(progress.completed_image_ids) != progress.face_count:
            return None
        if progress.next_index != progress.face_count:
            raise ProtocolError("face batch completed before all images arrived")
        summary = RecognitionBatchSummary(
            request_id=image.request_id,
            face_count=progress.face_count,
            ok_count=progress.ok_count,
            unknown_count=progress.unknown_count,
            failed_count=progress.failed_count,
        )
        del self._batches[image.request_id]
        return summary

    def abort(self, request_id: int) -> None:
        self._batches.pop(request_id, None)

    def clear(self) -> None:
        self._batches.clear()


async def find_device(name: str, timeout: float = 10.0) -> tuple[Any, int | None]:
    try:
        from bleak import BleakScanner

        discovered = await BleakScanner.discover(timeout=timeout, return_adv=True)
    except Exception as exc:
        raise BleAdapterUnavailable(f"BLE scan failed: {exc}") from exc
    for device, advertisement in discovered.values():
        if device.name == name or advertisement.local_name == name:
            return device, getattr(advertisement, "rssi", None)
    raise BleAdapterUnavailable(f"BLE device not found: {name}")


async def scan_devices(timeout: float = 10.0) -> list[dict[str, object]]:
    try:
        from bleak import BleakScanner

        discovered = await BleakScanner.discover(timeout=timeout, return_adv=True)
    except Exception as exc:
        raise BleAdapterUnavailable(f"BLE adapter unavailable: {exc}") from exc
    return [
        {
            "name": device.name or adv.local_name or "",
            "address": device.address,
            "rssi": getattr(adv, "rssi", None),
        }
        for device, adv in discovered.values()
    ]


class BleFaceSession:
    def __init__(
        self,
        device_name: str,
        on_image: Callable[[ReceivedFaceImage], Any] | None = None,
        *,
        event_store: EventStore | None = None,
        event_database: str | Path = "event_store.db",
    ) -> None:
        self.device_name = device_name
        self.on_image = on_image
        self.assembler = ImageAssembler()
        self.client: Any = None
        self.initial_rssi: int | None = None
        self.image_complete = asyncio.Event()
        self.last_image: bytes | None = None
        self.last_ids = (0, 0)
        self.started = 0.0
        self.chunk_count = 0
        self.crc_failures = 0
        self.disconnect_count = 0
        self.reconnect_count = 0
        self.batch_tracker = FaceBatchTracker()
        self.event_store = event_store if event_store is not None else EventStore(event_database)
        self._owns_event_store = event_store is None
        self._event_requests: dict[int, str] = {}
        self._owned_images: dict[tuple[int, int], str] = {}
        self._open_events: set[str] = set()
        self._boot_session: str | None = None

    def close(self) -> None:
        if self._owns_event_store:
            self.event_store.close()
            self._owns_event_store = False

    async def connect(self, timeout: float = 10.0) -> None:
        from bleak import BleakClient

        # Counters can restart while disconnected. Until this connection receives
        # an Event message, no prior boot's durable request mapping is usable.
        self.event_store.mark_disconnected(self._open_events)
        self._clear_active_ownership()
        device, self.initial_rssi = await find_device(self.device_name, timeout)
        self.client = BleakClient(device, disconnected_callback=self._disconnected)
        try:
            await self.client.connect()
            self.reconnect_count += 1
            await self.client.start_notify(EVENT_UUID, self._notification)
            await self.client.start_notify(IMAGE_UUID, self._notification)
        except Exception as exc:
            raise BleAdapterUnavailable(f"BLE connection/subscription failed: {exc}") from exc

    async def disconnect(self) -> None:
        if self.client and self.client.is_connected:
            await self.client.disconnect()

    def _disconnected(self, _: Any) -> None:
        self.disconnect_count += 1
        self.event_store.mark_disconnected(self._open_events)
        self._clear_active_ownership()

    def _clear_active_ownership(self) -> None:
        self._event_requests.clear()
        self._owned_images.clear()
        self._open_events.clear()
        self._boot_session = None
        self.batch_tracker.clear()
        self.assembler = ImageAssembler()

    def _notification(self, _: Any, data: bytearray) -> None:
        try:
            packet = decode_packet(bytes(data))
            if self.event_store.handle_packet(packet):
                self._associate_event(packet)
                self._log_event(packet)
            elif packet.header.message_type is MessageType.IMAGE_BEGIN:
                event_id = self._event_for_request(packet.header.request_id)
                if event_id is not None:
                    row = self.event_store.event(event_id)
                    if row["opened_at"] is None or row["face_terminal_received"] or row["completed_at"]:
                        raise ProtocolError("image began before OPEN or after face terminal")
                self.assembler.begin(packet)
                self.started = time.perf_counter()
                self.chunk_count = 0
                self.image_complete.clear()
            elif packet.header.message_type is MessageType.IMAGE_CHUNK:
                self.assembler.add(packet)
                self.chunk_count += 1
            elif packet.header.message_type is MessageType.IMAGE_END:
                jpeg = self.assembler.finish(packet)
                image = ReceivedFaceImage(
                    request_id=packet.header.request_id,
                    image_id=packet.header.image_id,
                    face_index=self.assembler.face_index,
                    face_count=self.assembler.face_count,
                    jpeg=jpeg,
                    event_id=self._event_for_request(packet.header.request_id),
                )
                if image.event_id is not None:
                    self.event_store.accept_face(
                        event_id=image.event_id,
                        request_id=image.request_id,
                        image_id=image.image_id,
                        face_index=image.face_index,
                        face_count=image.face_count,
                    )
                    self._owned_images[(image.request_id, image.image_id)] = image.event_id
                if self.on_image:
                    self.batch_tracker.accept(image)
                self.last_image = jpeg
                self.last_ids = (packet.header.request_id, packet.header.image_id)
                self.image_complete.set()
                if self.on_image:
                    result = self.on_image(image)
                    if asyncio.iscoroutine(result):
                        asyncio.create_task(result)
            else:
                if packet.header.message_type in (MessageType.ERROR, MessageType.NO_FACE):
                    self.batch_tracker.abort(packet.header.request_id)
                self._log_event(packet)
        except ProtocolError as exc:
            self.crc_failures += int("CRC" in str(exc))
            LOG.error("invalid notification: %s", exc)

    def _associate_event(self, packet: Packet) -> None:
        event_id = event_id_hex(int.from_bytes(packet.payload[:8], "little"))
        boot_session = event_id[:8]
        if self._boot_session is None or boot_session > self._boot_session:
            if self._boot_session is not None:
                self.event_store.mark_disconnected(self._open_events)
                self.batch_tracker.clear()
                self.assembler = ImageAssembler()
            self._boot_session = boot_session
            self._event_requests.clear()
            self._owned_images.clear()
            self._open_events.clear()
        if boot_session == self._boot_session:
            row = self.event_store.event(event_id)
            if row["completed_at"] is None:
                self._open_events.add(event_id)
                if packet.header.request_id:
                    self._event_requests[packet.header.request_id] = event_id
            else:
                self._open_events.discard(event_id)
                if row["request_id"]:
                    self._event_requests.pop(row["request_id"], None)
                    self._owned_images = {
                        key: owner for key, owner in self._owned_images.items() if owner != event_id
                    }
        if packet.header.message_type is MessageType.EVENT_OPEN:
            value = EventOpen.decode(packet.payload)
            LOG.info(
                "EVENT %s request_id=%d trigger=%s",
                event_id,
                packet.header.request_id,
                value.trigger_type.name,
            )
        elif packet.header.message_type is MessageType.EVENT_COMPLETE:
            row = self.event_store.event(event_id)
            LOG.info(
                "EVENT %s COMPLETE audio=%d face=%d faces=%d",
                event_id,
                row["audio_status"],
                row["face_status"],
                row["face_count"] or 0,
            )

    def _event_for_request(self, request_id: int) -> str | None:
        owner = self._event_requests.get(request_id)
        if owner is None and self._boot_session is not None:
            owner = self.event_store.event_for_request(request_id, self._boot_session)
        return owner

    @staticmethod
    def _log_event(packet: Packet) -> None:
        LOG.info(
            "event=%s request_id=%d image_id=%d reserved=%d",
            packet.header.message_type.name,
            packet.header.request_id,
            packet.header.image_id,
            packet.header.reserved,
        )

    async def start_capture(self, request_id: int, desired_test_bytes: int = 0) -> None:
        if not self.client or not self.client.is_connected:
            raise BleAdapterUnavailable("BLE is not connected")
        # Clear before writing so a completed prior request cannot satisfy the
        # next request's wait before IMAGE_BEGIN is delivered.
        self.image_complete.clear()
        self.last_image = None
        packet = start_capture_packet(request_id, int(time.time()), desired_test_bytes)
        await self.client.write_gatt_char(CONTROL_UUID, packet, response=True)

    async def send_recognition_result(
        self,
        request_id: int,
        image_id: int,
        status: StatusCode,
        person_id: str,
        person_name: str,
        similarity: float,
        processing_time_ms: int,
        *,
        event_id: str | None = None,
    ) -> None:
        owner = event_id or self._owned_images.get((request_id, image_id))
        if owner is not None:
            self.event_store.record_recognition(
                event_id=owner,
                request_id=request_id,
                image_id=image_id,
                status=status,
                person_id=person_id,
                person_name=person_name,
                similarity=similarity,
                processing_time_ms=processing_time_ms,
            )
        if not self.client or not self.client.is_connected:
            raise BleAdapterUnavailable("BLE disconnected; recognition retained locally")
        if owner is not None and self._event_for_request(request_id) != owner:
            raise ProtocolError("stale result belongs to a previous boot session")
        packet = recognition_result_packet(
            request_id=request_id,
            image_id=image_id,
            status=status,
            person_id=person_id,
            person_name=person_name,
            similarity=similarity,
            processing_time_ms=processing_time_ms,
        )
        await self.client.write_gatt_char(RESULT_UUID, packet, response=True)
        if owner is not None:
            self.event_store.mark_result_sent(owner, request_id, image_id)
            self._owned_images.pop((request_id, image_id), None)

    def complete_face(self, image: ReceivedFaceImage, status: StatusCode) -> RecognitionBatchSummary | None:
        return self.batch_tracker.complete(image, status)

    def metrics(self) -> TransferMetrics:
        duration = max(0.0, (time.perf_counter() - self.started) * 1000)
        image_bytes = len(self.last_image or b"")
        mtu = getattr(self.client, "mtu_size", None) if self.client else None
        return TransferMetrics(
            time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            self.device_name,
            self.last_ids[0],
            self.last_ids[1],
            image_bytes,
            self.chunk_count,
            duration,
            image_bytes * 8 / duration if duration else 0.0,
            self.initial_rssi,
            self.initial_rssi,
            self.initial_rssi,
            float(self.initial_rssi) if self.initial_rssi is not None else None,
            0.0 if self.initial_rssi is not None else None,
            mtu,
            len(self.assembler.missing_sequences()),
            self.assembler.duplicated_sequence_count,
            self.crc_failures,
            self.disconnect_count,
            self.reconnect_count,
            self.last_image is not None,
            0,
            0,
        )


def append_metrics_csv(path: str | Path, metrics: TransferMetrics) -> None:
    output = Path(path)
    output.parent.mkdir(parents=True, exist_ok=True)
    exists = output.exists()
    with output.open("a", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(asdict(metrics)))
        if not exists:
            writer.writeheader()
        writer.writerow(asdict(metrics))
