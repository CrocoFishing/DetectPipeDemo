import cv2
import numpy as np


def test_pc_can_decode_jpeg() -> None:
    source = np.zeros((32, 24, 3), dtype=np.uint8)
    source[:, :, 1] = 200
    ok, encoded = cv2.imencode(".jpg", source, [cv2.IMWRITE_JPEG_QUALITY, 85])
    assert ok
    decoded = cv2.imdecode(encoded, cv2.IMREAD_COLOR)
    assert decoded is not None and decoded.shape == source.shape
