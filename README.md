# ESP32-S3 Face Detection / PC Recognition Demo

This repository is a six-phase ESP-IDF 6.0 demo for Seeed Studio XIAO ESP32-S3 Sense (OV3660). The ESP32-S3 performs **face detection only** with the fixed `espdet_pico_224_224_face_s8_s3` configuration. The PC performs **face recognition** with InsightFace.

No display, microphone, audio playback, SD card, or extra UART module is required. The application trigger is an external momentary button between **D1/GPIO2 and GND**. GPIO0 remains the BOOT button and is never configured by the application.

## Repository map

- `firmware/components/`: reusable board, camera, trigger, detection, crop/JPEG, protocol, NimBLE, metrics, and controller modules.
- `firmware/apps/phase01_*` … `phase06_*`: independently buildable thin ESP-IDF apps.
- `pc_client/`: Bleak + InsightFace + SQLite CLI and host tests.
- `protocol/ble_protocol.yaml`: protocol single source of truth.
- `clients/ios/BLEProtocol.swift`: Foundation/CoreBluetooth protocol implementation.
- `tests/phase01` … `tests/phase06`: manual plans and result templates.
- `results/`: intentionally empty evidence destinations; hardware data is not fabricated.

## Wiring

```text
XIAO D1 / ESP32-S3 GPIO2 ─── momentary normally-open button ─── GND
```

GPIO2 is configured only after system initialization as input/pull-up/falling-edge. The ISR only calls `vTaskNotifyGiveFromISR`; a task waits 40 ms, re-reads low, emits one request, and waits for release. Do not use BOOT/GPIO0 as an application trigger.

## Build and test in order

Every command below is run from the repository root. `tools/idf.ps1` activates the ESP-IDF v6.0 profile in the same PowerShell process, so activation persistence is not assumed.

```powershell
# Phase 1 first
powershell.exe -ExecutionPolicy Bypass -File .\tools\idf.ps1 -App phase01_camera build
powershell.exe -ExecutionPolicy Bypass -File .\tools\idf.ps1 -App phase01_camera -p COM5 flash monitor

# Continue only after recording Phase 1 hardware evidence
powershell.exe -ExecutionPolicy Bypass -File .\tools\idf.ps1 -App phase02_trigger build
powershell.exe -ExecutionPolicy Bypass -File .\tools\idf.ps1 -App phase03_face_pipeline build
powershell.exe -ExecutionPolicy Bypass -File .\tools\idf.ps1 -App phase04_protocol build
powershell.exe -ExecutionPolicy Bypass -File .\tools\idf.ps1 -App phase05_ble build
powershell.exe -ExecutionPolicy Bypass -File .\tools\idf.ps1 -App phase06_end_to_end build
```

Replace `COM5`. Exit monitor with `Ctrl+]`. Build all apps with:

```powershell
powershell.exe -ExecutionPolicy Bypass -File .\tools\build_all.ps1
```

Set up and test the PC client without activating the venv:

```powershell
powershell.exe -ExecutionPolicy Bypass -File .\tools\create_python_venv.ps1
.\pc_client\.venv\Scripts\python.exe -m pip install -e .\pc_client
.\pc_client\.venv\Scripts\python.exe -m pytest .\pc_client -q
.\pc_client\.venv\Scripts\python.exe -m ruff check .\pc_client
.\pc_client\.venv\Scripts\python.exe -m mypy .\pc_client\src
```

## PC CLI

After the editable install:

```powershell
.\pc_client\.venv\Scripts\python.exe -m pc_client register --person-id P001 --name "Alice" --image alice1.jpg
.\pc_client\.venv\Scripts\python.exe -m pc_client register --person-id P001 --name "Alice" --image alice2.jpg
.\pc_client\.venv\Scripts\python.exe -m pc_client list
.\pc_client\.venv\Scripts\python.exe -m pc_client remove --person-id P001
.\pc_client\.venv\Scripts\python.exe -m pc_client recognize-file --image test.jpg
.\pc_client\.venv\Scripts\python.exe -m pc_client scan
.\pc_client\.venv\Scripts\python.exe -m pc_client run --device-name ESP32S3-FACE-DEMO
.\pc_client\.venv\Scripts\python.exe -m pc_client ble-test --device-name ESP32S3-FACE-DEMO
```

Registration rejects zero or multiple faces by default; add `--allow-largest-face` only when explicitly desired. Original registration images are not stored unless `--save-original` is passed. The first InsightFace use downloads the selected model pack and therefore needs network access and acceptance of its license terms.

## Phase sequence

1. Phase 1: verify PSRAM, OV3660 initialization, capture metadata, frame return, and heap stability.
2. Phase 2: verify GPIO2 debounce/release behavior, mock BLE requests, BUSY, duplicate IDs, and state logs.
3. Phase 3: manually test one face, no face, multiple faces/largest selection, 25% total margin, and JPEG output.
4. Phase 4: run host protocol tests; physical UART is neither required nor an acceptance condition.
5. Phase 5: flash the NimBLE synthetic transfer app, then run the 0.5/2/5 m quality matrix and save CSV.
6. Phase 6: register people, flash the end-to-end app, test both external-button and PC START_CAPTURE paths.

Detailed commands, expected output, pass/fail rules, recovery, and evidence locations are in each `tests/phaseNN/TEST_PLAN.md` and [docs/TESTING.md](docs/TESTING.md).

## Verification status

All six firmware apps were configured and built locally with ESP-IDF v6.0. Python lint, strict type checking, protocol consistency, and unit tests were run. No board was connected during repository creation, so flash, OV3660 operation, button electrical behavior, BLE/RSSI/throughput, InsightFace model download/inference on real photos, and end-to-end hardware behavior are explicitly **not yet tested**.

