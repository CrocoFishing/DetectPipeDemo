# Local Verification Record

Date: 2026-08-01  
Host: Windows PowerShell, no ESP32-S3 board connected

## Environment

| Check | Actual result |
|---|---|
| direct `idf.py --version` before profile | command not present in the original shell |
| `tools/idf.ps1 ... --version` | ESP-IDF v6.0 |
| ESP-IDF Git commit | `662a3be354759d9487bf4b1a629fadb766cb1800` (`v6.0-dirty`) |
| `python --version` | CPython 3.13.7 |
| `git --version` | 2.49.0.windows.1 |
| Swift compiler | not found; Swift syntax compilation not run |

## Firmware builds

All apps were configured and built with the repository wrapper and their locked dependencies. These are compile/link results, not flash or hardware-test results.

| App | Result | Binary bytes |
|---|---:|---:|
| phase01_camera | PASS | 253424 |
| phase02_trigger | PASS | 157184 |
| phase03_face_pipeline | PASS | 2596032 |
| phase04_protocol | PASS | 148976 |
| phase05_ble | PASS | 531600 |
| phase06_end_to_end | PASS | 2916640 |

Phase 5 and Phase 6 were also rebuilt after the final BLE security, timeout, and Device Information changes.

## Host validation

| Command/check | Result |
|---|---|
| editable PC-client installation | PASS |
| `ruff format --check pc_client` | PASS, 12 files already formatted |
| `ruff check pc_client` | PASS |
| `ruff check tests/phase03/extract_jpeg.py` | PASS |
| `mypy pc_client/src` | PASS, 8 source files |
| `pytest pc_client -q` | PASS, 18 tests |
| YAML/Python/C++/Swift protocol consistency subset | PASS, 2 tests |
| `python -m pc_client --help` | PASS; all eight required subcommands present |
| `tests/phase03/extract_jpeg.py --help` | PASS; device-JPEG restore utility loads |
| Swift syntax compilation | NOT RUN; compiler unavailable |

## Explicitly not run

- Flash/monitor, PSRAM electrical validation, OV3660 initialization/capture, and camera frame-return soak test.
- GPIO2 wiring, falling-edge ISR, debounce, release gate, long-press, BOOT/GPIO0 behavior, and simultaneous-trigger tests on a board.
- ESPDet load/inference, real face/no-face/multiple-face behavior, crop, and device-produced JPEG decode.
- BLE advertising, pairing/encryption, real negotiated MTU, notifications, congestion, disconnect/reconnect, RSSI, throughput, and the Phase 5 distance/environment matrix.
- InsightFace first-use model download/license acceptance, real-photo registration/recognition, and both Phase 6 end-to-end paths.

Use the phase result templates for these manual runs; do not replace this record with assumed values.
