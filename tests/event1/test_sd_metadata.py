from __future__ import annotations

import csv
import runpy
import wave
from pathlib import Path

import pytest

reconstruct = runpy.run_path(
    str(Path(__file__).parents[2] / "tools" / "inspect_event_sd.py")
)["reconstruct"]
KEY = "0000002500000068"


def write_csv(directory: Path, name: str, rows: list[dict[str, str | int]]) -> None:
    with (directory / name).open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def fixture(directory: Path) -> None:
    write_csv(
        directory,
        "audio_events.csv",
        [
            {
                "event_id": KEY,
                "audio_file": f"event_{KEY}.wav",
                "audio_source": "RAW",
                "sample_rate": 16000,
                "channels": 1,
                "bits_per_sample": 16,
                "audio_first_sample": 1000,
                "vad_start_sample": 17000,
                "vad_end_sample": 27000,
                "audio_last_sample": 33000,
                "vad_start_timestamp_us": 1000000,
                "vad_end_timestamp_us": 1625000,
                "audio_status": "READY",
            }
        ],
    )
    write_csv(
        directory,
        "event_lifecycle.csv",
        [
            {"event_id": KEY, "state": "OPEN", "timestamp_us": 1000000, "status": "OK"},
            {
                "event_id": KEY,
                "state": "FACE_BATCH_END",
                "timestamp_us": 1500000,
                "status": "COMPLETED",
            },
            {
                "event_id": KEY,
                "state": "AUDIO_READY",
                "timestamp_us": 1600000,
                "status": "READY",
            },
            {
                "event_id": KEY,
                "state": "COMPLETE",
                "timestamp_us": 1600001,
                "status": "COMPLETE",
            },
        ],
    )
    write_csv(
        directory,
        "face_associations.csv",
        [
            {
                "event_id": KEY,
                "request_id": 81,
                "image_id": 220,
                "face_index": 0,
                "face_count": 2,
                "status": "RECOGNIZED",
                "person_id": 'P,"01',
                "similarity": 0.71,
                "processing_time_ms": 52,
            },
            {
                "event_id": KEY,
                "request_id": 81,
                "image_id": 221,
                "face_index": 1,
                "face_count": 2,
                "status": "UNKNOWN",
                "person_id": "",
                "similarity": 0.31,
                "processing_time_ms": 48,
            },
            {
                "event_id": KEY,
                "request_id": 81,
                "image_id": 0,
                "face_index": -1,
                "face_count": 2,
                "status": "COMPLETED",
                "person_id": "",
                "similarity": "",
                "processing_time_ms": 0,
            },
        ],
    )
    with wave.open(str(directory / f"event_{KEY}.wav"), "wb") as wav:
        wav.setparams((1, 2, 16000, 0, "NONE", "not compressed"))
        wav.writeframes(b"\0\0" * 32000)


def test_reconstruct_raw_preroll_all_faces(tmp_path: Path) -> None:
    fixture(tmp_path)
    event = reconstruct(tmp_path)[0]
    assert event["event_id"] == KEY and event["request_id"] == 81
    assert event["audio"]["has_preroll"] == "True"
    assert [f["status"] for f in event["faces"]] == ["RECOGNIZED", "UNKNOWN"]
    assert event["faces"][0]["person_id"] == 'P,"01'
    assert event["status"] == "COMPLETE"


def test_wav_sample_mismatch_rejected(tmp_path: Path) -> None:
    fixture(tmp_path)
    with wave.open(str(tmp_path / f"event_{KEY}.wav"), "wb") as wav:
        wav.setparams((1, 2, 16000, 0, "NONE", "not compressed"))
        wav.writeframes(b"\0\0" * 100)
    with pytest.raises(ValueError, match="discontinuity"):
        reconstruct(tmp_path)


@pytest.mark.parametrize("missing", ["OPEN", "AUDIO_READY", "FACE_BATCH_END"])
def test_missing_lifecycle_evidence_retains_artifacts_as_incomplete(
    tmp_path: Path, missing: str
) -> None:
    fixture(tmp_path)
    path = tmp_path / "event_lifecycle.csv"
    with path.open(encoding="utf-8", newline="") as handle:
        rows = [row for row in csv.DictReader(handle) if row["state"] != missing]
    write_csv(tmp_path, "event_lifecycle.csv", rows)
    event = reconstruct(tmp_path)[0]
    assert event["status"] == "INCOMPLETE" and event["declared_status"] == "COMPLETE"
    assert any(missing in error for error in event["validation_errors"])
    assert event["audio"]["audio_status"] == "READY" and len(event["faces"]) == 2


def test_partial_zero_results_keeps_request_and_expected_count(tmp_path: Path) -> None:
    write_csv(
        tmp_path,
        "event_lifecycle.csv",
        [
            {"event_id": KEY, "state": "OPEN", "timestamp_us": 1, "status": "OK"},
            {
                "event_id": KEY,
                "state": "FACE_BATCH_END",
                "timestamp_us": 2,
                "status": "DISCONNECTED",
            },
        ],
    )
    write_csv(
        tmp_path,
        "face_associations.csv",
        [
            {
                "event_id": KEY,
                "request_id": 81,
                "image_id": 0,
                "face_index": -1,
                "face_count": 3,
                "status": "DISCONNECTED",
                "person_id": "",
                "similarity": "",
                "processing_time_ms": 0,
            },
        ],
    )
    event = reconstruct(tmp_path)[0]
    assert event["request_id"] == 81 and event["expected_face_count"] == 3
    assert event["face_status"] == "DISCONNECTED" and event["status"] == "INCOMPLETE"


def test_cross_event_request_rejected(tmp_path: Path) -> None:
    fixture(tmp_path)
    with (tmp_path / "face_associations.csv").open(
        "a", encoding="utf-8", newline=""
    ) as handle:
        csv.writer(handle).writerow(
            ["0000002500000069", 81, 0, -1, 0, "NO_FACE", "", "", 0]
        )
    with pytest.raises(ValueError, match="crossed Event"):
        reconstruct(tmp_path)
