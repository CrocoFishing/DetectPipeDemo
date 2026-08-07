from __future__ import annotations

import time
from dataclasses import dataclass

import numpy as np
import numpy.typing as npt

from .database import FaceDatabase
from .protocol import StatusCode


@dataclass(frozen=True, slots=True)
class Recognition:
    status: StatusCode
    person_id: str
    person_name: str
    similarity: float
    processing_time_ms: int


def recognize_embedding(
    query: npt.NDArray[np.float32], database: FaceDatabase, model_name: str, threshold: float
) -> Recognition:
    started = time.perf_counter()
    vector = np.asarray(query, dtype=np.float32).reshape(-1)
    norm = float(np.linalg.norm(vector))
    if norm <= 0:
        raise ValueError("invalid query embedding")
    vector /= norm
    samples = database.embeddings(model_name, int(vector.size))
    if not samples:
        return Recognition(
            StatusCode.UNKNOWN, "", "UNKNOWN", 0.0, int((time.perf_counter() - started) * 1000)
        )
    scores = [(float(np.dot(vector, sample)), pid, name) for pid, name, sample in samples]
    score, person_id, name = max(scores, key=lambda item: item[0])
    elapsed = int((time.perf_counter() - started) * 1000)
    if score < threshold:
        return Recognition(StatusCode.UNKNOWN, "", "UNKNOWN", score, elapsed)
    return Recognition(StatusCode.OK, person_id, name, score, elapsed)
