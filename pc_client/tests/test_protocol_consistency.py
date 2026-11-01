import re
import struct
from pathlib import Path

import yaml

from pc_client import protocol

ROOT = Path(__file__).resolve().parents[2]


def test_yaml_matches_python() -> None:
    spec = yaml.safe_load((ROOT / "protocol/ble_protocol.yaml").read_text(encoding="utf-8"))
    assert spec["protocol"]["version"] == protocol.VERSION
    assert spec["protocol"]["magic"] == protocol.MAGIC
    assert spec["protocol"]["header_length"] == protocol.HEADER_SIZE
    actual_uuids = {
        "service": protocol.SERVICE_UUID,
        "control": protocol.CONTROL_UUID,
        "event": protocol.EVENT_UUID,
        "image_data": protocol.IMAGE_UUID,
        "recognition_result": protocol.RESULT_UUID,
        "device_information": protocol.DEVICE_INFO_UUID,
        "sd_export_info": protocol.SD_EXPORT_INFO_UUID,
    }
    assert spec["uuids"] == actual_uuids
    assert spec["message_types"] == {item.name: item.value for item in protocol.MessageType}
    assert spec["message_flags"] == {item.name: item.value for item in protocol.MessageFlag}
    assert spec["feature_flags"] == {item.name: item.value for item in protocol.FeatureFlag}
    assert spec["error_codes"] == {item.name: item.value for item in protocol.ErrorCode}
    for section, enum in (
        ("trigger_sources", protocol.TriggerSource),
        ("audio_statuses", protocol.AudioStatus),
        ("face_statuses", protocol.FaceStatus),
        ("event_statuses", protocol.EventStatus),
        ("sd_export_states", protocol.SdExportState),
    ):
        assert spec[section] == {item.name: item.value for item in enum}
    for name, format_ in (
        ("EVENT_OPEN", protocol.EVENT_OPEN_FORMAT),
        ("EVENT_AUDIO_READY", protocol.EVENT_AUDIO_READY_FORMAT),
        ("FACE_BATCH_END", protocol.FACE_BATCH_END_FORMAT),
        ("EVENT_COMPLETE", protocol.EVENT_COMPLETE_FORMAT),
        ("SD_EXPORT_START", ""),
        ("SD_EXPORT_CANCEL", protocol.SD_EXPORT_SESSION_FORMAT),
        ("SD_EXPORT_GET_STATUS", protocol.SD_EXPORT_SESSION_FORMAT),
        ("SD_EXPORT_STATUS", protocol.SD_EXPORT_STATUS_FORMAT),
    ):
        payload = spec["payloads"][name]
        assert payload["python_struct"] == format_
        assert payload["length"] == struct.calcsize(format_)
        assert sum(field["length"] for field in payload["fields"]) == payload["length"]
        for field in payload["fields"]:
            assert field["offset"] + field["length"] <= payload["length"]


def test_cpp_and_swift_contain_protocol_constants() -> None:
    spec = yaml.safe_load((ROOT / "protocol/ble_protocol.yaml").read_text(encoding="utf-8"))
    cpp = (
        (ROOT / "firmware/components/ble_transport/include/ble_transport.hpp")
        .read_text(encoding="utf-8")
        .lower()
    )
    swift = (ROOT / "clients/ios/BLEProtocol.swift").read_text(encoding="utf-8").lower()
    for value in spec["uuids"].values():
        assert value.lower() in cpp
        assert value.lower() in swift
    app_header = (
        ROOT / "firmware/components/application_protocol/include/application_protocol.hpp"
    ).read_text(encoding="utf-8")
    for value in spec["message_types"].values():
        assert f"= {value}" in app_header
    for value in spec["error_codes"].values():
        assert f"= {value}" in app_header
    assert "FaceSequence" in app_header
    assert "facesequence" in swift
    assert f"VERSION = {protocol.VERSION};" in app_header
    assert f"version: uint8 = {protocol.VERSION}" in swift
    cpp_names = {
        "EVENT_OPEN": "EventOpen",
        "EVENT_AUDIO_READY": "EventAudioReady",
        "FACE_BATCH_END": "FaceBatchEnd",
        "EVENT_COMPLETE": "EventComplete",
        "SD_EXPORT_START": "SdExportStart",
        "SD_EXPORT_CANCEL": "SdExportCancel",
        "SD_EXPORT_GET_STATUS": "SdExportGetStatus",
        "SD_EXPORT_STATUS": "SdExportStatus",
    }
    swift_names = {name: value[0].lower() + value[1:] for name, value in cpp_names.items()}
    for name in cpp_names:
        assert re.search(rf"\b{cpp_names[name]}\s*=\s*{spec['message_types'][name]}\b", app_header)
        assert re.search(rf"\b{swift_names[name].lower()}\s*=\s*{spec['message_types'][name]}\b", swift)
    for name in ("EventOpen", "EventAudioReady", "FaceBatchEnd", "EventComplete"):
        constant = re.sub(r"(?<!^)(?=[A-Z])", "_", name).upper() + "_PAYLOAD_SIZE"
        payload_name = re.sub(r"(?<!^)(?=[A-Z])", "_", name).upper()
        assert f"{constant} = {spec['payloads'][payload_name]['length']}" in app_header


def test_sd_export_enums_and_mtu_limits_match_all_clients() -> None:
    spec = yaml.safe_load((ROOT / "protocol/ble_protocol.yaml").read_text(encoding="utf-8"))
    cpp = (ROOT / "firmware/components/application_protocol/include/application_protocol.hpp").read_text()
    swift = (ROOT / "clients/ios/BLEProtocol.swift").read_text()
    for section in ("sd_export_states", "error_codes"):
        names = spec[section]
        if section == "error_codes":
            names = {name: value for name, value in names.items() if value >= 23}
        for name, value in names.items():
            cpp_name = "".join(word.capitalize() for word in name.split("_"))
            swift_name = cpp_name[0].lower() + cpp_name[1:]
            assert re.search(rf"\b{cpp_name}\s*=\s*{value}\b", cpp)
            assert re.search(rf"\b{swift_name}\s*=\s*{value}\b", swift)
    assert "WifiSdExport = 1U << 6" in cpp
    assert "wifiSDExportFeature: UInt32 = 0x00000040" in swift
    assert "SD_EXPORT_STATUS_PAYLOAD_SIZE = 16" in cpp
    assert "SD_EXPORT_SESSION_PAYLOAD_SIZE = 8" in cpp
    assert spec["protocol"]["header_length"] + struct.calcsize(protocol.SD_EXPORT_STATUS_FORMAT) + 3 <= 67
    assert spec["characteristics"]["sd_export_info"]["maximum_bytes"] == 512
