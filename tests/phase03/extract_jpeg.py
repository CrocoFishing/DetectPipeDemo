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

FACE_CHUNK = re.compile(
    r"\[JPEG_B64\]\s+face=(\d+)/(\d+)\s+index=(\d+)\s+data=([A-Za-z0-9+/=]+)"
)
LEGACY_CHUNK = re.compile(r"\[JPEG_B64\]\s+index=(\d+)\s+data=([A-Za-z0-9+/=]+)")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("log", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument(
        "--face", type=int, default=1, help="one-based face rank to extract"
    )
    args = parser.parse_args()

    if args.face <= 0:
        raise SystemExit("--face must be at least 1")
    text = args.log.read_text(encoding="utf-8", errors="replace")
    chunks: dict[int, bytes] = {}
    face_count = 1
    matches = FACE_CHUNK.findall(text)
    if matches:
        for face, total, index, encoded in matches:
            if int(face) == args.face:
                face_count = int(total)
                chunks[int(index)] = base64.b64decode(encoded, validate=True)
    elif args.face == 1:
        for index, encoded in LEGACY_CHUNK.findall(text):
            chunks[int(index)] = base64.b64decode(encoded, validate=True)
    if not chunks or sorted(chunks) != list(range(len(chunks))):
        raise SystemExit(
            f"missing or non-contiguous JPEG_B64 chunks for face {args.face}"
        )

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
                "face": args.face,
                "face_count": face_count,
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
