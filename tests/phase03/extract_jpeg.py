"""Restore and decode the Phase 3 device JPEG from a captured serial log."""

from __future__ import annotations

import argparse
import base64
import json
import re
import zlib
from pathlib import Path

import cv2
import numpy as np

CHUNK = re.compile(r"\[JPEG_B64\]\s+index=(\d+)\s+data=([A-Za-z0-9+/=]+)")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("log", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    chunks: dict[int, bytes] = {}
    for index, encoded in CHUNK.findall(args.log.read_text(encoding="utf-8", errors="replace")):
        chunks[int(index)] = base64.b64decode(encoded, validate=True)
    if not chunks or sorted(chunks) != list(range(len(chunks))):
        raise SystemExit("missing or non-contiguous JPEG_B64 chunks")

    jpeg = b"".join(chunks[index] for index in range(len(chunks)))
    decoded = cv2.imdecode(np.frombuffer(jpeg, dtype=np.uint8), cv2.IMREAD_COLOR)
    if decoded is None or decoded.shape[0] <= 0 or decoded.shape[1] <= 0:
        raise SystemExit("OpenCV could not decode the restored JPEG")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(jpeg)
    print(
        json.dumps(
            {
                "result": "PASS",
                "jpeg_bytes": len(jpeg),
                "crc32": f"{zlib.crc32(jpeg) & 0xFFFFFFFF:08x}",
                "width": int(decoded.shape[1]),
                "height": int(decoded.shape[0]),
                "output": str(args.output),
            }
        )
    )


if __name__ == "__main__":
    main()
