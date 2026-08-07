# Troubleshooting

- `idf.py` not found: always call `tools/idf.ps1`; it activates `C:\Espressif\tools\Microsoft.v6.0.PowerShell_profile.ps1` inside the command process.
- PowerShell script policy: invoke tools with `powershell.exe -ExecutionPolicy Bypass -File ...`.
- Wrong boot mode: release BOOT/GPIO0 before reset. The application uses only external D1/GPIO2 after startup.
- Camera init failure: reseat the Sense camera flex cable, verify the board variant is OV3660, power-cycle, and compare pins only against `board_config.hpp`.
- PSRAM unavailable: confirm ESP32-S3 Sense hardware and octal PSRAM sdkconfig. This is fatal; do not continue with internal RAM.
- No face: use even frontal lighting and sufficient face size. This is a safe NO_FACE result, not face recognition failure.
- Detector load: verify lockfiles, 8 MB flash partition, and `CONFIG_FLASH_ESPDET_PICO_224_224_FACE=y`; fullclean and rebuild.
- JPEG encode/crop error: inspect bbox/crop dimensions and largest PSRAM block. The controller releases slots in recovery.
- BLE device absent: confirm Phase 5/6 is flashed, Bluetooth adapter is enabled, old connection is closed, then run `scan`.
- Notify not subscribed: the client must subscribe to Event and Image before START_CAPTURE.
- MTU error: the 32-byte header requires ATT MTU ≥36. Update the central/OS; do not hard-code a chunk size.
- Congestion: shorten range, remove interference, inspect retry/congestion counters, and lower connection load. Retries are bounded.
- Disconnect mid-transfer: partial image is intentionally discarded. Reconnect, resubscribe, use a fresh request ID.
- CRC/missing/out-of-order: keep the raw CSV/log; successful reassembly requires all chunks and whole-image CRC. Do not pass a corrupted image to InsightFace.
- Stale/wrong result: ensure both request_id and image_id from IMAGE_BEGIN are returned.
- Empty database: recognition returns UNKNOWN. Register at least one sample.
- Database model mismatch: use a separate DB or remove/re-register all samples with one model/dimension; embeddings are never mixed.
- InsightFace no/multiple faces: registration rejects by default. Use a clearer single-face photo or explicitly `--allow-largest-face` for registration only.
- Model download failure: permit network on first `buffalo_l` use or pre-provision the InsightFace model cache according to its license.
- BLE adapter unavailable: close other BLE software, enable Windows Bluetooth, verify permissions/drivers, and retry `scan`.
- Swift compiler absent: run consistency pytest on Windows; compile `BLEProtocol.swift` in a macOS/iOS target later.

