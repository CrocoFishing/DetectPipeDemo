# Architecture Decision Records

## ADR-001: NimBLE

Accepted. ESP-IDF 6.0 NimBLE compiled successfully and has lower memory cost than Bluedroid for a BLE-only peripheral. No compatibility reason required Bluedroid. GATT and connection handling therefore use NimBLE exclusively.

## ADR-002: Fixed detector

Accepted. Only `HumanFaceDetect::ESPDET_PICO_224_224_FACE` is instantiated, backed by the S3 artifact and identified by `espdet_pico_224_224_face_s8_s3`. No detector comparison, conversion, retraining, margin experiment, or accuracy benchmark is included.

## ADR-003: RGB565 camera frame and reusable PSRAM

Superseded by ADR-006. The original GitHub baseline used the lower-resolution RGB565 path described here; its historical hardware evidence must not be treated as validation of the current XGA configuration.

## ADR-004: Protocol header and MTU

Accepted. A 32-byte transport-independent header contains all mandatory fields. BLE payload per notification is `negotiated_mtu - 3 - 32`; MTU below 36 is rejected. There is no hard-coded MTU or second BLE-only protocol.

## ADR-005: Demo security

Accepted for development only. Pairing/encryption/authenticated-result enforcement default to false in the single board policy header. Production must enable authenticated encrypted pairing and application authorization; see `docs/PRIVACY.md`.

## ADR-006: XGA RGB565 and reusable PSRAM

Accepted. The current pipeline captures 1024×768 XGA RGB565, uses the fixed 224×224 detector preprocessor, and expands each detected face by 20% per side. Crop and 160 KiB JPEG slots are allocated once in PSRAM, and esp32-camera retains its DMA frame ownership. This configuration requires new hardware and memory-pressure evidence; the earlier QVGA result logs are historical only.

## ADR-007: Bounded no-face fallback

Accepted. A request uses one initial capture plus at most two recaptures. If all normal-orientation detections miss, the final frame is rotated 180 degrees in-place and detected once more without allocating another frame. Any successful attempt ends the fallback sequence; exhausting it emits one `NO_FACE`.

## ADR-008: Sequential every-face processing

Accepted. Every valid detector box is stably sorted by original area, largest first. Faces share one request ID but receive unique image IDs and are cropped, encoded, transferred, and recognized sequentially with reusable buffers. The optional `FACE_SEQUENCE` flag stores zero-based face index/count metadata in the existing v1 header; unflagged images remain face 0 of 1 for compatibility.
