from __future__ import annotations

from pathlib import Path
from typing import Any

import cv2
import numpy as np
import numpy.typing as npt


class FaceCountError(ValueError):
    pass


class InsightFaceEngine:
    def __init__(self, model_name: str = "buffalo_l", providers: list[str] | None = None) -> None:
        self.model_name = model_name
        try:
            from insightface.app import FaceAnalysis  # type: ignore[import-untyped]
        except ImportError as exc:
            raise RuntimeError("InsightFace is not installed; run tools/create_python_venv.ps1") from exc
        self._app: Any = FaceAnalysis(name=model_name, providers=providers or ["CPUExecutionProvider"])
        self._app.prepare(ctx_id=-1, det_size=(640, 640))

    @staticmethod
    def decode(data: bytes) -> npt.NDArray[np.uint8]:
        image = cv2.imdecode(np.frombuffer(data, dtype=np.uint8), cv2.IMREAD_COLOR)
        if image is None:
            raise ValueError("OpenCV could not decode image")

        image = cv2.copyMakeBorder(image, 20, 20, 20, 20, cv2.BORDER_CONSTANT, value=[255, 255, 255])
        ###### me ######
        cv2.imwrite("output.png", image)
        ###### me ######

        return np.asarray(image, dtype=np.uint8)

    def embedding_from_bytes(self, data: bytes, *, allow_largest: bool = False) -> npt.NDArray[np.float32]:
        faces = self._app.get(self.decode(data))
        if not faces:
            raise FaceCountError("InsightFace found no face")
        if len(faces) != 1 and not allow_largest:
            raise FaceCountError(f"InsightFace found {len(faces)} faces; expected exactly one")
        face = max(
            faces, key=lambda item: float((item.bbox[2] - item.bbox[0]) * (item.bbox[3] - item.bbox[1]))
        )
        # FaceAnalysis performs landmark-based alignment before its recognition model.
        vector = np.asarray(face.normed_embedding, dtype=np.float32).reshape(-1)
        norm = float(np.linalg.norm(vector))
        if norm <= 0:
            raise ValueError("InsightFace returned an invalid embedding")
        return np.asarray(vector / np.float32(norm), dtype=np.float32)

    def embedding_from_file(
        self, path: str | Path, *, allow_largest: bool = False
    ) -> tuple[npt.NDArray[np.float32], bytes]:
        data = Path(path).read_bytes()
        return self.embedding_from_bytes(data, allow_largest=allow_largest), data
