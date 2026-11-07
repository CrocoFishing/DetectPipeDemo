import json
import runpy
from pathlib import Path
from typing import Any
from unittest.mock import AsyncMock

import pytest

from pc_client.protocol import (
    ErrorCode,
    MessageType,
    SdExportState,
    SdExportStatus,
    make_packet,
)

HELPERS = runpy.run_path(str(Path(__file__).resolve().parents[2] / "tools/smoke_sd_export_ble.py"))
SESSION = 0x1122334455667788


def info(**changes: Any) -> bytes:
    value = {
        "format_version": 1,
        "session_id": f"{SESSION:016X}",
        "ssid": "secret-network-name",
        "passphrase": "secret-wifi-password",
        "base_url": "http://192.168.4.1",
        "token": "secret-token-should-never-be-logged-1234567890",
    }
    value.update(changes)
    return json.dumps(value).encode()


def test_smoke_info_validator_returns_only_byte_count() -> None:
    assert HELPERS["validate_connection_info"](info(), SESSION) == len(info())


@pytest.mark.parametrize(
    "changes",
    [
        {"session_id": "0000000000000000"},
        {"format_version": True},
        {"base_url": "http://192.168.4.1@attacker.example"},
        {"base_url": "http://192.168.4.1/path"},
        {"passphrase": "short"},
        {"token": "short"},
    ],
)
def test_smoke_info_rejects_invalid_fields_without_echoing_secrets(changes: dict[str, Any]) -> None:
    with pytest.raises(HELPERS["SmokeFailure"]) as caught:
        HELPERS["validate_connection_info"](info(**changes), SESSION)
    assert "secret" not in str(caught.value)


def test_smoke_notification_ignores_audio_events_and_validates_crc(
    capsys: pytest.CaptureFixture[str],
) -> None:
    report: dict[str, Any] = {"states": [], "invalid_notifications": 0}
    smoke = HELPERS["ExportSmoke"](None, 45, report)
    request_id = smoke.request_id()
    smoke.start_request = request_id
    audio = make_packet(MessageType.EVENT_AUDIO_READY, request_id=request_id, payload=bytes(32))
    smoke.notification(None, bytearray(audio))
    assert not report["states"]
    wire = make_packet(
        MessageType.SD_EXPORT_STATUS,
        request_id=request_id,
        payload=SdExportStatus(SESSION, SdExportState.PREPARING).encode(),
    )
    smoke.notification(None, bytearray(wire))
    assert smoke.owned_session == SESSION
    assert report["states"][0]["state"] == "PREPARING"
    damaged = bytearray(wire)
    damaged[-1] ^= 1
    smoke.notification(None, damaged)
    assert report["invalid_notifications"] == 1
    assert "secret" not in capsys.readouterr().out


@pytest.mark.parametrize("error", [ErrorCode.WIFI_START_FAILED, ErrorCode.QUIESCE_TIMEOUT])
def test_cleanup_accepts_restored_failure_without_changing_failed_outcome(error: ErrorCode) -> None:
    report: dict[str, Any] = {"states": [], "invalid_notifications": 0, "outcome": "failed"}
    smoke = HELPERS["ExportSmoke"](None, 45, report)
    smoke.owned_session = SESSION
    smoke.query = AsyncMock(return_value=SdExportStatus(SESSION, SdExportState.FAILED, error))
    smoke.cancel_owned = AsyncMock()
    with pytest.raises(StopIteration) as finished:
        smoke.cleanup_owned().send(None)
    assert finished.value.value == "already_terminal_and_restored"
    assert smoke.owned_session == 0
    smoke.cancel_owned.assert_not_awaited()
    assert report["outcome"] == "failed"


def test_cleanup_retries_cancel_when_audio_restoration_failed() -> None:
    report: dict[str, Any] = {"states": [], "invalid_notifications": 0, "outcome": "failed"}
    smoke = HELPERS["ExportSmoke"](None, 45, report)
    smoke.owned_session = SESSION
    smoke.query = AsyncMock(return_value=SdExportStatus(
        SESSION, SdExportState.FAILED, ErrorCode.AUDIO_RESUME_FAILED
    ))
    smoke.cancel_owned = AsyncMock()
    with pytest.raises(StopIteration) as finished:
        smoke.cleanup_owned().send(None)
    assert finished.value.value == "cancelled_and_restored"
    smoke.cancel_owned.assert_awaited_once()
    assert report["outcome"] == "failed"
