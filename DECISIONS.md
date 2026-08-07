# Architecture Decision Records

## ADR-001: NimBLE

Accepted. ESP-IDF 6.0 NimBLE compiled successfully and has lower memory cost than Bluedroid for a BLE-only peripheral. No compatibility reason required Bluedroid. GATT and connection handling therefore use NimBLE exclusively.

## ADR-002: Fixed detector

Accepted. Only `HumanFaceDetect::ESPDET_PICO_224_224_FACE` is instantiated, backed by the S3 artifact and identified by `espdet_pico_224_224_face_s8_s3`. No detector comparison, conversion, retraining, margin experiment, or accuracy benchmark is included.

## ADR-003: RGB565 camera frame and reusable PSRAM

Accepted. QVGA RGB565 supports ESP-DL preprocessing and a row-copy crop without a full RGB888 frame. Crop and 160 KiB JPEG slots are allocated once in PSRAM. esp32-camera retains its DMA frame ownership.

## ADR-004: Protocol header and MTU

Accepted. A 32-byte transport-independent header contains all mandatory fields. BLE payload per notification is `negotiated_mtu - 3 - 32`; MTU below 36 is rejected. There is no hard-coded MTU or second BLE-only protocol.

## ADR-005: Demo security

Accepted for development only. Pairing/encryption/authenticated-result enforcement default to false in the single board policy header. Production must enable authenticated encrypted pairing and application authorization; see `docs/PRIVACY.md`.
