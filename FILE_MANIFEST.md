# Source File Manifest

Generated source and documentation files are grouped below. Generated ESP-IDF `build/`, managed-component caches, Python `.venv/`, and interpreter caches are intentionally excluded.

## Root

- `.gitignore`
- `CMakeLists.txt`
- `README.md`
- `ARCHITECTURE.md`
- `DEPENDENCIES.md`
- `DECISIONS.md`
- `FILE_MANIFEST.md`

## Firmware

- `firmware/sdkconfig.defaults`
- `firmware/sdkconfig.ble.defaults`
- `firmware/partitions.csv`
- `firmware/apps/phase01_camera/` through `firmware/apps/phase06_end_to_end/`: each contains `CMakeLists.txt`, `main/CMakeLists.txt`, `main/app_main.cpp`, `tools/idf.ps1`, `sdkconfig`, and a generated `dependencies.lock`.
- `firmware/components/board_support/`: centralized pins, board constants, hardware validation, LED, and memory reporting.
- `firmware/components/pipeline_core/`: request/state/error types and state-machine policies.
- `firmware/components/trigger_service/`: GPIO2 ISR, debounce, release gate, ID namespaces, BUSY, and duplicate arbitration.
- `firmware/components/camera_service/`: OV3660/PSRAM initialization and RAII frame ownership.
- `firmware/components/face_detection/`: fixed ESPDet detector, largest-face selection, margin, and clamp.
- `firmware/components/image_processing/`: PSRAM crop/JPEG reusable buffers and quality-85 encoding.
- `firmware/components/application_protocol/`: binary header, enums, CRC32, encoder, and decoder.
- `firmware/components/ble_transport/`: NimBLE GATT, dynamic-MTU chunks, notification backpressure, transfer, and RSSI metrics.
- `firmware/components/system_controller/`: end-to-end orchestration and recognition-result handling.
- `firmware/components/metrics/`: uniform metric and RSSI aggregation output.

Each component directory contains its `CMakeLists.txt`, public headers under `include/`, implementation source where applicable, and managed-component manifests where required.

## PC client

- `pc_client/pyproject.toml`
- `pc_client/requirements.lock`
- `pc_client/config.example.toml`
- `pc_client/src/pc_client/{__init__,__main__,protocol,assembler,database,face_engine,recognizer,ble_client}.py`
- `pc_client/tests/{test_protocol,test_protocol_consistency,test_database,test_jpeg_decode}.py`

## Protocol and clients

- `protocol/ble_protocol.yaml`
- `clients/ios/BLEProtocol.swift`
- `docs/BLE_PROTOCOL.md`
- `docs/TESTING.md`
- `docs/TROUBLESHOOTING.md`
- `docs/PRIVACY.md`

## Phase tests and results

- `tests/phase01/` through `tests/phase06/`: each contains `README.md`, `TEST_PLAN.md`, `RESULT_TEMPLATE.md`, `expected_output.txt`, and `run_test.ps1`; Phase 3 additionally contains `extract_jpeg.py` for device-JPEG restoration and OpenCV decode.
- `results/phase01/actual/.gitkeep` through `results/phase06/actual/.gitkeep`.
- `results/LOCAL_VERIFICATION.md`.

## Tools

- `tools/idf.ps1`
- `tools/build_all.ps1`
- `tools/create_python_venv.ps1`
- `tools/check_protocol_consistency.py`
- `tools/parse_serial_log.py`
