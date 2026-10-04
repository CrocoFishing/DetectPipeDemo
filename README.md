# EventPipeDemo — Event 1 Audio / Multi-face Association

This independent integration repository combines DetectPipeDemo's XGA multiface pipeline with AudioVal's always-on PDM / ESP-SR VAD / RAW pre-roll recording on Seeed Studio XIAO ESP32-S3 Sense (OV3660). Each VAD utterance owns a persistent-session 64-bit Event ID, one local WAV, and at most one camera request containing all detected faces. InsightFace recognition runs on the PC. Both original repositories remain untouched.

Build the integrated app with `powershell.exe -ExecutionPolicy Bypass -File .\tools\idf.ps1 -App event1_audio_face build`. Run the PC client with `python -m pc_client --event-database event_store.db run --wait-only` from an environment with `pc_client` installed. GPIO/BLE manual captures remain diagnostic captures without an audio Event.

See [Event 1 architecture and operation](docs/EVENT1.md), [current verification](results/EVENT1_VERIFICATION.md), and [hardware acceptance matrix](tests/event1/HARDWARE_TEST_PLAN.md). Compilation and host tests do not establish audio continuity on hardware; that acceptance remains pending.

Face detection uses one initial capture plus the bounded recapture count configured by `FACE_DETECTION_MAX_RECAPTURES`. When `FACE_DETECTION_ROTATE_FINAL_FRAME_180` is enabled and all normal attempts miss, the firmware rotates the final RGB565 frame 180 degrees in-place and detects it once more without taking another photo. Both compile-time settings live beside the other detector settings in `board_config.hpp`.

When a frame contains multiple faces, the firmware stably sorts every valid detector box by original area, largest first. It then reuses one crop/JPEG buffer pair to transfer and recognize each face sequentially under the same request; each face has its own image ID and zero-based sequence metadata in the unchanged 32-byte header. Protocol v2 adds Event association messages and requires ATT MTU 67 for those messages.

The integrated app uses PDM DATA GPIO41 / CLK GPIO42 and an SD card on SPI SCLK7 / MISO8 / MOSI9 / CS3. Event WAVs contain 16 kHz mono PCM16 RAW capture plus application pre-roll. USB UAC, continuous recording, audio BLE upload and STT are disabled/absent. The optional diagnostic trigger is a button between **D1/GPIO2 and GND**. GPIO0 remains reserved for BOOT. The six original phase apps below remain available for subsystem validation.

## Repository map

- `firmware/components/`: camera/BLE pipeline, imported audio components, event-only recorder and bounded EventManager.
- `firmware/apps/event1_audio_face`: combined Event 1 app, SR model partition and integration defaults.
- `firmware/apps/phase01_*` … `phase06_*`: independently buildable thin ESP-IDF apps.
- `pc_client/`: Bleak + InsightFace + SQLite identity database and durable EventStore.
- `protocol/ble_protocol.yaml`: protocol single source of truth.
- `clients/ios/BLEProtocol.swift`: Foundation/CoreBluetooth protocol implementation.
- `tests/phase01` … `tests/phase06`: manual plans and result templates.
- `results/`: local raw evidence destinations plus reviewable verification summaries; raw files under `results/**/actual/` remain ignored by Git.

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
3. Phase 3: manually test one face, no face, all detected faces in size order, configured margin, and JPEG output.
4. Phase 4: run host protocol tests; physical UART is neither required nor an acceptance condition.
5. Phase 5: flash the NimBLE synthetic transfer app, then run the 0.5/2/5 m quality matrix and save CSV.
6. Phase 6: register people, flash the end-to-end app, test both external-button and PC START_CAPTURE paths.

Detailed commands, expected output, pass/fail rules, recovery, and evidence locations are in each `tests/phaseNN/TEST_PLAN.md` and [docs/TESTING.md](docs/TESTING.md).

## Verification status

The GitHub baseline records successful ESP-IDF v6.0 builds for all six firmware apps plus passing Python lint, strict type checking, protocol consistency, and unit tests. No board was connected for that original baseline verification.

Later local-only evidence now covers limited OV3660 capture, GPIO trigger/state-machine behavior, face detection/JPEG generation, protocol smoke tests, BLE transfers, and four end-to-end recognition flows. This evidence is incomplete and does **not** establish full hardware acceptance: required repetition counts, several negative/recovery scenarios, environment metadata, and signed result fields are still missing.

The Unreleased XGA, multi-face, recapture, and rotation-fallback implementation is newer than those raw logs. Its host suite passes, but it still requires fresh Phase 1, 3, and 6 hardware evidence before acceptance.

See [CHANGELOG.md](CHANGELOG.md) for the exact differences from GitHub commit `346a1ac`, and [results/TEST_DATA_SUMMARY.md](results/TEST_DATA_SUMMARY.md) for the detailed measurements and remaining gaps. Raw logs and CSV files stay local under the ignored `results/**/actual/` paths.
