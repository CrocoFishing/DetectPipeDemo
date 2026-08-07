# Phase 3 Test Plan

- Purpose: verify model load, legal detector results, largest face, total 25% expansion, clamp, positive crop, JPEG encode, PC JPEG decode, no-face recovery, and buffer return.
- Preconditions: Phase 1 passed; face/no-face/multi-face scenes available; PC OpenCV environment installed.
- Hardware: board/OV3660/USB; no BLE required.
- Firmware: `firmware/apps/phase03_face_pipeline`.
- Build/flash: wrapper with `-App phase03_face_pipeline build`, then `-p COM5 flash monitor`.
- Steps: run ten single-face captures, ten no-face scenes, ten multi-face scenes with visibly different face sizes, and edge faces; save logs; verify selected area is maximum; independently calculate 12.5% per side and clamp. For each face case, restore the test app's `[JPEG_B64]` chunks and decode the actual device JPEG with `tests/phase03/extract_jpeg.py` as shown below.
- Data: serial log/JSONL with raw boxes, chosen/expanded box, crop dimensions, JPEG bytes/CRC, heap before/after; decoded JPEG dimensions.
- Expected: model-loaded line, legal boxes, `JPEG_READY`, or safe `NO_FACE`; no recognition identity is produced on ESP32.
- Pass: required smoke checks pass, PC decodes each obtained JPEG, all buffers return, no-face reaches safe idle/reset state.
- Fail: alternate detector, confidence violation, wrong largest face, 25% each-side expansion, out-of-range/zero crop, encode/decode failure, leak/panic.
- Recovery: power-cycle; inspect PSRAM largest block; check camera cable/model lock/sdkconfig; preserve failing frame metadata without claiming accuracy.
- Results: `results/phase03/actual/`.

```powershell
.\pc_client\.venv\Scripts\python.exe .\tests\phase03\extract_jpeg.py `
  .\results\phase03\actual\serial.log `
  --output .\results\phase03\actual\device_crop.jpg
```

The Phase 3 serial log contains the cropped face JPEG in Base64. Treat it as sensitive test data and do not publish it.
