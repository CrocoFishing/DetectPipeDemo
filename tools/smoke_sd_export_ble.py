"""Exercise BLE SD-export preparation/restoration without joining the device Wi-Fi.

Run from the repository's Python environment. Credentials are validated in RAM,
never printed or included in the optional evidence report.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import re
import sys
import time
from datetime import UTC, datetime
from pathlib import Path
from typing import Any
from urllib.parse import urlsplit

from bleak import BleakClient, BleakScanner

# Also support direct invocation without installing the repository package.
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "pc_client" / "src"))

from pc_client.protocol import (  # noqa: E402
    CONTROL_UUID,
    DEVICE_INFO_UUID,
    EVENT_UUID,
    SD_EXPORT_INFO_UUID,
    ErrorCode,
    FeatureFlag,
    MessageType,
    ProtocolError,
    SdExportState,
    SdExportStatus,
    decode_packet,
    sd_export_cancel_packet,
    sd_export_get_status_packet,
    sd_export_start_packet,
)

DEFAULT_ADDRESS = "E0:72:A1:FB:FF:3E"
ACTIVE_STATES = {
    SdExportState.PREPARING,
    SdExportState.READY,
    SdExportState.TRANSFERRING,
    SdExportState.RESTORING,
}


class SmokeFailure(RuntimeError):
    """A deliberately non-secret diagnostic failure reason."""


def validate_connection_info(data: bytes, session_id: int) -> int:
    """Validate explicit session credentials; return only their total byte count."""
    if not data or len(data) > 512:
        raise SmokeFailure("invalid_export_info_length")
    try:
        value = json.loads(data)
        if not isinstance(value, dict) or type(value.get("format_version")) is not int:
            raise ValueError
        if value["format_version"] != 1 or value.get("session_id") != f"{session_id:016X}":
            raise ValueError
        if not session_id or any(
            not isinstance(value.get(key), str) for key in ("ssid", "passphrase", "base_url", "token")
        ):
            raise ValueError
        if not 1 <= len(value["ssid"].encode("utf-8")) <= 32:
            raise ValueError
        password = value["passphrase"].encode("ascii")
        token = value["token"].encode("ascii")
        if not 8 <= len(password) <= 63 or any(byte < 32 or byte > 126 for byte in password):
            raise ValueError
        if not 32 <= len(token) <= 128 or any(byte < 33 or byte > 126 for byte in token):
            raise ValueError
        url = urlsplit(value["base_url"])
        if (
            url.scheme != "http"
            or url.hostname != "192.168.4.1"
            or url.port not in (None, 80)
            or url.username is not None
            or url.password is not None
            or url.path not in ("", "/")
            or url.query
            or url.fragment
        ):
            raise ValueError
    except (UnicodeError, ValueError, KeyError, TypeError) as exc:
        # JSON/Unicode exceptions can retain the source credentials. Keep the
        # public error and evidence report independent of their message text.
        raise SmokeFailure("invalid_export_info") from exc
    return len(data)


class ExportSmoke:
    def __init__(self, client: BleakClient, timeout: float, report: dict[str, Any]) -> None:
        self.client = client
        self.timeout = timeout
        self.report = report
        self.started = time.monotonic()
        self.cycle = 0
        self.owned_session = 0
        self.previous_session = 0
        self.start_request = 0
        self.next_request = 1
        self.requests: set[int] = set()
        self.responses: dict[int, SdExportStatus | ErrorCode] = {}
        self.changed = asyncio.Event()
        self.last_printed: tuple[int, SdExportState, ErrorCode] | None = None

    def request_id(self) -> int:
        result = self.next_request
        self.next_request += 1
        self.requests.add(result)
        return result

    def notification(self, _: Any, data: bytearray) -> None:
        try:
            packet = decode_packet(bytes(data))
            request_id = packet.header.request_id
            if request_id not in self.requests:
                return
            if packet.header.message_type is MessageType.ERROR:
                if len(packet.payload) >= 2:
                    self.responses[request_id] = ErrorCode(int.from_bytes(packet.payload[:2], "little"))
                    self.changed.set()
                return
            if packet.header.message_type is not MessageType.SD_EXPORT_STATUS:
                return  # Existing audio/Event/image notifications are never acted on.
            status = SdExportStatus.from_packet(packet)
        except (ProtocolError, ValueError):
            self.report["invalid_notifications"] += 1
            return
        self.responses[request_id] = status
        if (
            request_id == self.start_request
            and status.error is ErrorCode.NONE
            and status.session_id
            and status.session_id != self.previous_session
            and status.state in ACTIVE_STATES
        ):
            self.owned_session = status.session_id
        key = (status.session_id, status.state, status.error)
        if key != self.last_printed:
            record = {
                "cycle": self.cycle,
                "elapsed_s": round(time.monotonic() - self.started, 3),
                "session_id": f"{status.session_id:016X}",
                "state": status.state.name,
                "state_code": int(status.state),
                "error": status.error.name,
                "error_code": int(status.error),
            }
            self.report["states"].append(record)
            print(json.dumps(record), flush=True)
            self.last_printed = key
        self.changed.set()

    async def send(self, request_id: int, packet: bytes) -> None:
        self.responses.pop(request_id, None)
        await asyncio.wait_for(self.client.write_gatt_char(CONTROL_UUID, packet, response=True), 5)

    async def query(self, session_id: int, deadline: float) -> SdExportStatus | None:
        request_id = self.request_id()
        await self.send(request_id, sd_export_get_status_packet(request_id, session_id))
        reply_deadline = min(deadline, time.monotonic() + 3)
        while time.monotonic() < reply_deadline:
            self.changed.clear()
            reply = self.responses.pop(request_id, None)
            if isinstance(reply, ErrorCode):
                raise SmokeFailure(f"device_error_{reply.name}")
            if reply is not None:
                return reply
            try:
                await asyncio.wait_for(self.changed.wait(), max(0.01, reply_deadline - time.monotonic()))
            except TimeoutError:
                break
        return None

    async def wait_state(self, target: SdExportState, *, preparing: bool = False) -> SdExportStatus:
        deadline = time.monotonic() + self.timeout
        while time.monotonic() < deadline:
            start_reply = self.responses.get(self.start_request)
            if preparing and isinstance(start_reply, ErrorCode):
                raise SmokeFailure(f"device_error_{start_reply.name}")
            if preparing and isinstance(start_reply, SdExportStatus) and start_reply.error is not ErrorCode.NONE:
                raise SmokeFailure(f"device_error_{start_reply.error.name}")
            status = await self.query(self.owned_session, deadline)
            if status is not None:
                if status.error is not ErrorCode.NONE:
                    raise SmokeFailure(f"device_error_{status.error.name}")
                if preparing and status.session_id and status.session_id != self.previous_session:
                    self.owned_session = status.session_id
                if status.state is SdExportState.FAILED:
                    raise SmokeFailure("device_failed_restore_or_prepare")
                if status.state is target and status.session_id == self.owned_session and self.owned_session:
                    return status
                if preparing and status.state in (SdExportState.CANCELLED, SdExportState.COMPLETED):
                    if status.session_id == self.owned_session and self.owned_session:
                        raise SmokeFailure("unexpected_terminal_state_before_ready")
            await asyncio.sleep(min(0.5, max(0, deadline - time.monotonic())))
        raise SmokeFailure(f"timeout_waiting_for_{target.name}")

    async def cancel_owned(self) -> None:
        if not self.owned_session or not self.client.is_connected:
            return
        request_id = self.request_id()
        await self.send(request_id, sd_export_cancel_packet(request_id, self.owned_session))
        await self.wait_state(SdExportState.CANCELLED)
        self.owned_session = 0

    async def cleanup_owned(self) -> str:
        # FAILED retains the original preparation/network error after successful
        # restoration. Only AUDIO_RESUME_FAILED means restoration is still pending.
        # Observing this terminal does not change the diagnostic's failed outcome.
        try:
            status = await self.query(self.owned_session, time.monotonic() + min(5, self.timeout))
        except Exception:
            status = None
        if (
            status is not None
            and status.session_id == self.owned_session
            and status.state in (SdExportState.FAILED, SdExportState.COMPLETED, SdExportState.CANCELLED)
            and status.error is not ErrorCode.AUDIO_RESUME_FAILED
        ):
            self.owned_session = 0
            return "already_terminal_and_restored"
        await self.cancel_owned()
        return "cancelled_and_restored"

    async def run_cycle(self, number: int) -> None:
        self.cycle = number
        self.last_printed = None
        baseline = await self.query(0, time.monotonic() + min(5, self.timeout))
        if baseline is None:
            raise SmokeFailure("baseline_status_timeout")
        if baseline.state in ACTIVE_STATES or baseline.error is not ErrorCode.NONE:
            raise SmokeFailure("device_has_existing_export_or_restore_error")
        self.previous_session = baseline.session_id
        self.start_request = self.request_id()
        cycle_start = time.monotonic()
        await self.send(self.start_request, sd_export_start_packet(self.start_request))
        ready = await self.wait_state(SdExportState.READY, preparing=True)
        data = bytes(await asyncio.wait_for(self.client.read_gatt_char(SD_EXPORT_INFO_UUID), 10))
        info_size = validate_connection_info(data, ready.session_id)
        del data
        print(json.dumps({"session_id": f"{ready.session_id:016X}", "export_info_bytes": info_size}), flush=True)
        ready_after = time.monotonic() - cycle_start
        await self.cancel_owned()
        self.report["cycles"].append({
            "cycle": number,
            "session_id": f"{ready.session_id:016X}",
            "ready_after_s": round(ready_after, 3),
            "duration_s": round(time.monotonic() - cycle_start, 3),
            "export_info_bytes": info_size,
            "credentials_validated": True,
            "restored": True,
            "outcome": "passed",
        })


async def diagnose(args: argparse.Namespace, report: dict[str, Any]) -> None:
    device = await BleakScanner.find_device_by_address(args.address, timeout=args.timeout)
    if device is None or device.address.casefold() != args.address.casefold():
        raise SmokeFailure("exact_ble_address_not_found")
    # Firmware adds a characteristic; refresh Windows GATT services after flash.
    client = BleakClient(device, timeout=args.timeout, winrt={"use_cached_services": False})
    smoke = ExportSmoke(client, args.timeout, report)
    subscribed = False
    try:
        await client.connect()
        raw_info = bytes(await asyncio.wait_for(client.read_gatt_char(DEVICE_INFO_UUID), 10))
        match = re.search(r"(?:^|;)features=(0x[0-9a-fA-F]+|[0-9]+)(?:;|$)", raw_info.decode("utf-8"))
        if not match or not int(match[1], 0) & FeatureFlag.WIFI_SD_EXPORT:
            raise SmokeFailure("wifi_sd_export_feature_missing")
        await client.start_notify(EVENT_UUID, smoke.notification)
        subscribed = True
        mtu = client.mtu_size
        report["att_mtu"] = mtu
        if mtu and mtu < 67:
            raise SmokeFailure("att_mtu_below_67")
        for number in range(1, args.cycles + 1):
            await smoke.run_cycle(number)
    finally:
        if smoke.owned_session and client.is_connected:
            try:
                report["cleanup"] = await smoke.cleanup_owned()
            except Exception as exc:
                report["cleanup"] = f"failed_{type(exc).__name__}"
        if subscribed and client.is_connected:
            try:
                await client.stop_notify(EVENT_UUID)
            except Exception:
                pass
        if client.is_connected:
            await client.disconnect()


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--address", default=DEFAULT_ADDRESS, help="exact BLE MAC address")
    parser.add_argument("--cycles", type=int, default=2, help="START/READY/CANCEL/restoration cycles")
    parser.add_argument("--timeout", type=float, default=45, help="scan/connect and per-state timeout seconds")
    parser.add_argument("--report", type=Path, help="optional non-secret JSON evidence path")
    args = parser.parse_args()
    if not re.fullmatch(r"(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}", args.address):
        parser.error("--address must be an exact colon-separated BLE MAC address")
    if args.cycles < 1 or args.timeout <= 0:
        parser.error("--cycles and --timeout must be positive")
    return args


def main() -> int:
    args = arguments()
    report: dict[str, Any] = {
        "format_version": 1,
        "started_at_utc": datetime.now(UTC).isoformat(),
        "address": args.address.upper(),
        "requested_cycles": args.cycles,
        "timeout_s": args.timeout,
        "states": [],
        "cycles": [],
        "invalid_notifications": 0,
        "cleanup": "not_needed",
        "outcome": "failed",
    }
    try:
        asyncio.run(diagnose(args, report))
        report["outcome"] = "passed"
    except KeyboardInterrupt:
        report["reason"] = "interrupted"
    except SmokeFailure as exc:
        report["reason"] = str(exc)  # Only fixed, non-secret reasons enter this exception type.
    except Exception as exc:
        report["reason"] = f"diagnostic_error_{type(exc).__name__}"
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: report[key] for key in ("outcome", "cleanup", "reason") if key in report}))
    return 0 if report["outcome"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
