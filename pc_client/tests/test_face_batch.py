import pytest

from pc_client.ble_client import FaceBatchTracker, ReceivedFaceImage
from pc_client.protocol import ProtocolError, StatusCode


def face(index: int, count: int, image_id: int = 10) -> ReceivedFaceImage:
    return ReceivedFaceImage(7, image_id, index, count, b"jpeg")


def test_face_batch_tracks_order_and_result_summary() -> None:
    tracker = FaceBatchTracker()
    first = face(0, 2, 10)
    second = face(1, 2, 11)

    tracker.accept(first)
    assert tracker.complete(first, StatusCode.OK) is None
    tracker.accept(second)
    summary = tracker.complete(second, StatusCode.UNKNOWN)

    assert summary is not None
    assert summary.request_id == 7
    assert summary.face_count == 2
    assert (summary.ok_count, summary.unknown_count, summary.failed_count) == (1, 1, 0)


def test_face_batch_counts_failed_and_rejects_invalid_sequence() -> None:
    tracker = FaceBatchTracker()
    with pytest.raises(ProtocolError, match="index 0"):
        tracker.accept(face(1, 2))

    first = face(0, 2, 10)
    tracker.accept(first)
    with pytest.raises(ProtocolError, match="face_count changed"):
        tracker.accept(face(1, 3, 11))
    with pytest.raises(ProtocolError, match="out of order"):
        tracker.accept(face(0, 2, 12))

    assert tracker.complete(first, StatusCode.FAILED) is None
    second = face(1, 2, 11)
    tracker.accept(second)
    summary = tracker.complete(second, StatusCode.FAILED)
    assert summary is not None
    assert summary.failed_count == 2


def test_face_batch_rejects_duplicate_image_id() -> None:
    tracker = FaceBatchTracker()
    tracker.accept(face(0, 2, 10))
    with pytest.raises(ProtocolError, match="duplicate image_id"):
        tracker.accept(face(1, 2, 10))
