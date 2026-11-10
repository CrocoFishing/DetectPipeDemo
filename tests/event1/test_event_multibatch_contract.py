"""Independent SD evidence for periodic batches and receiver-triggered retries."""

import runpy
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
reconstruct = runpy.run_path(str(ROOT / "tools/inspect_event_sd.py"))["reconstruct"]
EID = "000000250000006B"


def evidence(path: Path) -> None:
    (path / "audio_events.csv").write_text(
        "event_id,audio_file,audio_source,sample_rate,channels,bits_per_sample,audio_first_sample,"
        "vad_start_sample,vad_end_sample,audio_last_sample,vad_start_timestamp_us,vad_end_timestamp_us,audio_status\n"
        f"{EID},event_{EID}.wav,RAW,16000,1,16,0,0,4800000,4800000,0,300000000,READY\n",
        encoding="utf-8",
    )
    (path / "face_associations.csv").write_text(
        "event_id,request_id,image_id,face_index,face_count,status,person_id,similarity,processing_time_ms\n"
        f'{EID},100,1,0,2,RECOGNIZED,"P1",0.9,10\n'
        f'{EID},100,2,1,2,CLIENT_NO_FACE,"",0,10\n'
        f"{EID},100,0,-1,2,PARTIAL,,,0\n"
        f'{EID},101,3,0,1,UNKNOWN,"",0.2,10\n'
        f"{EID},101,0,-1,1,COMPLETED,,,0\n"
        f'{EID},102,4,0,1,RECOGNIZED,"P2",0.8,10\n'
        f"{EID},102,0,-1,1,COMPLETED,,,0\n",
        encoding="utf-8",
    )
    (path / "event_lifecycle.csv").write_text(
        "event_id,state,timestamp_us,status\n"
        f"{EID},OPEN,0,OK\n"
        f"{EID},FACE_BATCH_END,100000,PARTIAL\n"
        f"{EID},FACE_BATCH_OPEN,200000,CLIENT_NO_FACE_RETRY\n"
        f"{EID},FACE_BATCH_END,300000,COMPLETED\n"
        f"{EID},FACE_BATCH_OPEN,30000000,OK\n"
        f"{EID},FACE_BATCH_END,30100000,COMPLETED\n"
        f"{EID},AUDIO_READY,300000000,READY\n"
        f"{EID},COMPLETE,300000100,COMPLETE_WITH_ERROR\n",
        encoding="utf-8",
    )


def test_all_batches_and_pre_retry_successes_survive(tmp_path: Path) -> None:
    evidence(tmp_path)
    event = reconstruct(tmp_path, verify_wav=False)[0]
    assert event["status"] == "COMPLETE_WITH_ERROR"
    assert [batch["request_id"] for batch in event["face_batches"]] == [100, 101, 102]
    assert [face["face_index"] for face in event["faces"]] == ["0", "1", "0", "0"]
    assert event["faces"][0]["person_id"] == "P1"
    assert len(event["face_batches"]) == 3


def test_duplicate_terminal_is_scoped_to_request(tmp_path: Path) -> None:
    evidence(tmp_path)
    with (tmp_path / "face_associations.csv").open("a", encoding="utf-8") as stream:
        stream.write(f"{EID},101,0,-1,1,COMPLETED,,,0\n")
    with pytest.raises(ValueError, match="duplicate batch"):
        reconstruct(tmp_path, verify_wav=False)


def test_earlier_failed_batch_cannot_be_declared_success(tmp_path: Path) -> None:
    evidence(tmp_path)
    path = tmp_path / "event_lifecycle.csv"
    path.write_text(
        path.read_text(encoding="utf-8").replace("COMPLETE_WITH_ERROR", "COMPLETE"),
        encoding="utf-8",
    )
    with pytest.raises(ValueError, match="failed batch"):
        reconstruct(tmp_path, verify_wav=False)
