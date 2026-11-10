from types import SimpleNamespace

import numpy as np
import pytest

from pc_client.face_engine import FaceCountError, InsightFaceEngine, NoFaceError


def test_zero_faces_has_a_distinct_retry_signal(monkeypatch: pytest.MonkeyPatch) -> None:
    instance = InsightFaceEngine.__new__(InsightFaceEngine)
    monkeypatch.setattr(
        InsightFaceEngine, "decode", staticmethod(lambda _: np.zeros((1, 1, 3), dtype=np.uint8))
    )
    instance._app = SimpleNamespace(get=lambda _: [])
    with pytest.raises(NoFaceError):
        instance.embedding_from_bytes(b"valid-jpeg-placeholder")
    instance._app = SimpleNamespace(get=lambda _: [object(), object()])
    with pytest.raises(FaceCountError) as result:
        instance.embedding_from_bytes(b"valid-jpeg-placeholder")
    assert not isinstance(result.value, NoFaceError)  # Multiple faces are not a zero-detection signal.
