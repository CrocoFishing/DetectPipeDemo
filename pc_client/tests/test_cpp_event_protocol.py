import os
import shutil
import subprocess
from pathlib import Path

import pytest

from pc_client.protocol import (
    AudioStatus,
    ErrorCode,
    EventAudioReady,
    EventComplete,
    EventOpen,
    EventStatus,
    FaceBatchEnd,
    FaceStatus,
    MessageType,
    SdExportState,
    SdExportStatus,
    make_packet,
)

ROOT = Path(__file__).resolve().parents[2]


def test_cpp_packets_match_python_golden_wire(tmp_path: Path) -> None:
    component = ROOT / "firmware/components/application_protocol"
    source = Path(__file__).with_name("cpp_event_protocol.cpp")
    executable = tmp_path / ("event_protocol.exe" if os.name == "nt" else "event_protocol")
    vcvars = Path(r"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat")
    compiler = shutil.which("g++") or shutil.which("clang++")
    if os.name == "nt" and vcvars.is_file():
        command = (
            f'call "{vcvars}" >nul && cl /nologo /std:c++17 /EHsc /W4 /WX '
            f'/I"{component / "include"}" "{source}" "{component / "application_protocol.cpp"}" '
            f'/Fe:"{executable}"'
        )
        script = tmp_path / "compile.cmd"
        script.write_text("@echo off\n" + command + "\n", encoding="utf-8")
        result = subprocess.run(
            ["cmd", "/d", "/c", str(script)],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=60,
            cwd=tmp_path,
        )
    elif compiler:
        result = subprocess.run(
            [
                compiler,
                "-std=c++17",
                "-Wall",
                "-Wextra",
                "-Werror",
                f"-I{component / 'include'}",
                str(source),
                str(component / "application_protocol.cpp"),
                "-o",
                str(executable),
            ],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=60,
        )
    else:
        pytest.skip("host C++ compiler unavailable")
    assert result.returncode == 0, result.stdout + result.stderr
    result = subprocess.run(
        [str(executable)], capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=10
    )
    assert result.returncode == 0, result.stdout + result.stderr
    event_id, request = 0xFEDCBA9876543210, 0x80000012
    payloads = (
        (MessageType.EVENT_OPEN, EventOpen(event_id)),
        (
            MessageType.EVENT_AUDIO_READY,
            EventAudioReady(event_id, AudioStatus.READY, 0x1122334455667788, 0x8877665544332211),
        ),
        (MessageType.FACE_BATCH_END, FaceBatchEnd(event_id, request, 3, 3, 1, 1, 1, FaceStatus.FAILED)),
        (
            MessageType.EVENT_COMPLETE,
            EventComplete(event_id, EventStatus.COMPLETE_WITH_ERROR, AudioStatus.READY, FaceStatus.FAILED),
        ),
        (MessageType.SD_EXPORT_STATUS, SdExportStatus(event_id, SdExportState.FAILED, ErrorCode.SD_IO)),
    )
    assert result.stdout.splitlines() == [
        make_packet(
            kind, request_id=7 if kind is MessageType.SD_EXPORT_STATUS else request, payload=value.encode()
        ).hex().upper()
        for kind, value in payloads
    ]
