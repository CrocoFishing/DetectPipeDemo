# Architecture

## Data ownership and control flow

```text
GPIO2 ISR ─notify─> TriggerService task ─CaptureRequest(value)─> queue(length 1)
BLE write ─copy─> BLE RX queue ─same TriggerService arbitration─┘
                                      │
                                      v
camera_pipeline_task: bounded capture/detect attempts -> size-sorted faces -> sequential crop/JPEG/BLE/result loop
```

`CaptureRequest` is passed by value. Camera, crop, and JPEG bytes are never copied into a FreeRTOS queue and never placed on a task stack.

- Camera frame: owned by `FrameLease`; retained while the sorted faces are processed and returned after the final crop or on recovery.
- Crop: one preallocated PSRAM RGB565 slot, acquired by `copy_crop`, released after JPEG encoding.
- JPEG: one preallocated 160 KiB PSRAM slot, held through `IMAGE_END`, then released before waiting for recognition.
- DMA buffers: owned by esp32-camera and allocated through its PSRAM/DMA configuration. Control packets, queues, and task stacks remain internal RAM.
- Backpressure: capture queue length is one; acceptance atomically marks BUSY before enqueue. No new capture is accepted until completion/recovery. BLE notification allocation and queue congestion retry every 5 ms for at most 500 ms per notification.
- Disconnect: transfer detects `connected=false`, fails, increments metrics, releases frame/crop/JPEG in recovery, and resumes advertising.

## Modules and tasks

| Module | Responsibility |
|---|---|
| `SystemController` | module startup, state transitions, failure recovery |
| `TriggerService` | ISR-safe notification, 40 ms debounce, release gate, ID namespaces, duplicate/BUSY arbitration |
| `CameraService` | OV3660 init/capture and RAII frame ownership |
| `FaceDetectionService` | only `espdet_pico_224_224_face_s8_s3`, legal results, stable area-descending sort, coordinate mapping |
| `ReusableImageBuffers` / image processing | in-place RGB565 rotation, 20% each-side expansion, clamp, crop, JPEG quality 85 |
| `ApplicationProtocol` | transport-independent 32-byte header, little-endian serialization, CRC32 |
| `BleTransport` | NimBLE GATT, MTU-derived chunks, notify retry, RSSI and transfer metrics |
| `RecognitionResultHandler` | implemented in controller result queue/validator; rejects stale or incorrect IDs |
| `MetricsService` | canonical `[METRIC]` logs and RSSI statistics |

| Task | Core/priority | Notes |
|---|---:|---|
| `trigger_task` | any / 6 | only debounce, GPIO read, release wait, request submit |
| `camera_pipeline_task` | core 1 / 7 | capture through application transfer and result wait |
| `ble_event_task` | any / 6 | validates writes; submits triggers/results |
| NimBLE host task | stack managed | GATT/GAP callbacks copy bounded values only |
| `ble_tx_task` | Phase 5 / 5 | synthetic quality transfers; Phase 6 sends synchronously from pipeline with bounded backpressure |
| `metrics_task` | any / 2 | 10 s connection-quality summary; RSSI at most 1 Hz |

All large allocations use PSRAM and occur during initialization. The final-frame rotation is in-place and does not allocate another camera-sized buffer. The pipeline task registers with the task watchdog and resets it between capture/detection attempts and waits. Timeouts are bounded; there is no unbounded trigger queue.

## Face pipeline

The camera provides XGA RGB565. ESP-DL `ImagePreprocessor` letterbox-resizes it to the model's fixed 224×224 tensor and maps results back to source dimensions. Results below 0.50 are removed. By default, the pipeline makes one initial capture and at most two recaptures. If all three normal-orientation detections miss, the final frame is rotated 180 degrees in-place and detected once more without another capture. Any successful attempt short-circuits the fallback sequence. Every valid half-open box from that frame is stably sorted by original area, largest first. Each face is expanded by 20% per side (40% total width and height), cropped, encoded, transferred, and recognized before the next face reuses the same buffers. Expansion is clamped to the original frame.

## State policies

| State | Entry/action | Timeout | Success | Failure | Trigger? | Recovery |
|---|---|---:|---|---|---|---|
| INITIALIZING | validate PSRAM; create pools; init camera/model/BLE/button | 15 s | IDLE | ERROR_RECOVERY | no | reinitialize module/reboot only if fatal |
| IDLE | wait for one request | none | CAPTURING | IDLE | yes | none |
| CAPTURING | acquire camera frame lease | 4 s (esp32-camera bounded wait) | DETECTING | ERROR_RECOVERY | no | return frame; reinit camera |
| DETECTING | fixed detector; legal-format check; bounded recapture/final rotation fallback | 6 s per invocation | CROPPING, CAPTURING, or NO_FACE | ERROR_RECOVERY | no | return frame; reload detector |
| CROPPING | take next area-sorted face, expand/clamp/copy | 1 s per face | ENCODING | ERROR_RECOVERY | no | release crop slot |
| ENCODING | JPEG callback into reusable slot, quality 85 | 3 s | TRANSMITTING | ERROR_RECOVERY | no | release JPEG slot |
| TRANSMITTING | begin/chunks/end; CRC and bounded retry | 15 s | WAITING_RESULT | ERROR_RECOVERY | no | abort and reclaim buffers |
| WAITING_RESULT | match protocol/request/image IDs for current face | 15 s per face | CROPPING or COMPLETED | ERROR_RECOVERY | no | report stale/timeout; abort remaining faces |
| COMPLETED | log face-batch result/metrics | 250 ms | IDLE | ERROR_RECOVERY | no | complete request |
| NO_FACE | emit one NO_FACE after all attempts; release frame | 250 ms | IDLE | ERROR_RECOVERY | no | complete request |
| ERROR_RECOVERY | report error; release all ownership; module-specific reset | 3 s | IDLE | INITIALIZING/fatal | no | never retain image buffers |

## Error classification

| Class | Examples | Action |
|---|---|---|
| recoverable | no face, duplicate, BUSY, stale result | report and return/continue safely |
| retryable | BLE congestion, transient notify allocation | bounded 5 ms delay, max 500 ms per notification |
| connection-reset-required | disconnect mid-transfer, notify not subscribed | abort image, reconnect/resubscribe |
| module-reinitialize-required | capture failure, camera init after prior success, detector load failure | release ownership and reinitialize module |
| fatal | PSRAM unavailable, queues/pools cannot be allocated repeatedly | remain stopped; do not start button ISR |
