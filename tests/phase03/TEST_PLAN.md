# Phase 3 Test Plan

- Purpose: verify model load, bounded no-face recaptures, final-frame 180-degree fallback, legal detector results, stable area-descending face order, every-face crop/JPEG output, configured expansion, clamp, no-face recovery, and buffer return.
- Preconditions: Phase 1 passed; face/no-face/multi-face scenes available; PC OpenCV environment installed.
- Hardware: board/OV3660/USB; no BLE required.
- Firmware: `firmware/apps/phase03_face_pipeline`.
- Build/flash: wrapper with `-App phase03_face_pipeline build`, then `-p COM5 flash monitor`.
- Steps: run ten single-face captures, ten no-face scenes, ten multi-face scenes with visibly different face sizes, equal-size faces, and edge faces. Verify every valid box appears once in descending raw-area order and equal-area boxes preserve detector order. Confirm early success stops further captures. With the default settings, confirm a persistent no-face scene produces three normal attempts and one rotated attempt of capture 3, then exactly one `NO_FACE`. Exercise an upside-down face that reaches the rotated fallback and verify its decoded crop orientation. Rebuild once with rotation disabled and once with zero recaptures to verify both configuration boundaries. Restore each device JPEG with `extract_jpeg.py --face <one-based-rank>` and decode it independently.
- Data: serial log/JSONL with all raw and expanded boxes, rank/count, crop dimensions, per-face JPEG bytes/CRC, heap before/after, and decoded JPEG dimensions.
- Expected: model-loaded and attempt lines, at most three captures/four detector calls with defaults, one ordered `JPEG_READY` per detected face, or one safe `NO_FACE`; no recognition identity is produced on ESP32.
- Pass: ordering and output count match detection, fallback counts and orientations match configuration, PC decodes every obtained JPEG, buffers are reused and returned, and no-face reaches safe idle/reset state.
- Fail: alternate detector, confidence violation, missing/duplicate/out-of-order face, incorrect margin, out-of-range/zero crop, encode/decode failure, leak/panic.
- Recovery: power-cycle; inspect PSRAM largest block; check camera cable/model lock/sdkconfig; preserve failing frame metadata without claiming accuracy.
- Results: `results/phase03/actual/`.

```powershell
.\pc_client\.venv\Scripts\python.exe .\tests\phase03\extract_jpeg.py `
  .\results\phase03\actual\serial.log `
  --face 1 `
  --output .\results\phase03\actual\device_crop.jpg
```

The Phase 3 serial log contains the cropped face JPEG in Base64. Treat it as sensitive test data and do not publish it.
