from __future__ import annotations

import asyncio
import csv
import logging
import time
from collections.abc import Callable
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any

from .assembler import ImageAssembler
from .protocol import (
    CONTROL_UUID,
    EVENT_UUID,
    IMAGE_UUID,
    RESULT_UUID,
    MessageType,
    Packet,
    ProtocolError,
    StatusCode,
    decode_packet,
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
    def __init__(self, device_name: str, on_image: Callable[[int, int, bytes], Any] | None = None) -> None:
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

    async def connect(self, timeout: float = 10.0) -> None:
        from bleak import BleakClient

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

    def _notification(self, _: Any, data: bytearray) -> None:
        try:
            packet = decode_packet(bytes(data))
            if packet.header.message_type is MessageType.IMAGE_BEGIN:
                self.assembler.begin(packet)
                self.started = time.perf_counter()
                self.chunk_count = 0
                self.image_complete.clear()
            elif packet.header.message_type is MessageType.IMAGE_CHUNK:
                self.assembler.add(packet)
                self.chunk_count += 1
            elif packet.header.message_type is MessageType.IMAGE_END:
                image = self.assembler.finish(packet)
                self.last_image = image
                self.last_ids = (packet.header.request_id, packet.header.image_id)
                self.image_complete.set()
                if self.on_image:
                    result = self.on_image(*self.last_ids, image)
                    if asyncio.iscoroutine(result):
                        asyncio.create_task(result)
            else:
                self._log_event(packet)
        except ProtocolError as exc:
            self.crc_failures += int("CRC" in str(exc))
            LOG.error("invalid notification: %s", exc)

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
    ) -> None:
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
