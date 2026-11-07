# Event 1 integration

The integration branch is `codex/event1-audio-face-association` in this independent repository. Source baselines: DetectPipeDemo `981a531c2880086d259d0c1604756a3a9730d4c2`, AudioVal `148b0af0dbe017b3d921cbaa589ec8e88a0e7f7e`. The source plan is preserved in `docs/EVENT1_REQUEST.md`.

See [Event 1 state diagrams](EVENT_STATE_DIAGRAM.md) for the complete Event lifecycle, asynchronous audio/face completion, WAV recording, camera admission/multi-face processing, failure paths, and PC persistence.

For app-side interpretation of SD artifacts, use the [Event data model](EVENT_DATA_MODEL.md). The integrated app can now export all SD files through a [BLE-started Wi-Fi session](BLE_PROTOCOL.md); [iOS integration](IOS_SD_EXPORT.md) supplies the Swift module workflow. WAV bytes travel over HTTP in this explicit export mode. Recording and manual captures pause only after existing owners finish and the writer acknowledges its final metadata barrier.

## Ownership and lifecycle

One VAD utterance is one Event. The VAD-start callback reserves an Event ID and bounded metadata synchronously, then immediately copies the application RAW pre-roll ring into the writer queue. It only wakes the EventManager task; camera and BLE operations occur in that separate task and the existing controller. The manager can track 16 simultaneous owners without any face vectors. Exhaustion is logged as `EVENT_REJECTED`; no owner is overwritten. Its metadata signals occupy reserved slots and wake a task notification, so terminal signals cannot disappear because a lifecycle queue is full.

The upper 32 bits of the Event ID are a boot counter committed once to NVS namespace `event1`, key `boot_session`; the lower 32 bits are a sequence beginning at 1. NVS initialization, commit and counter-overflow failures stop Event startup. No NVS error handler erases the counter. PC/SD IDs are 16 uppercase hexadecimal digits. Erasing/replacing NVS intentionally starts a new identity history and is outside the preserved-NVS reboot guarantee.

EventManager submits at most one `CaptureRequest` with `has_event`, `event_id` and `EventVad`. GPIO and BLE submissions share the original queue-of-one arbitration and generate no audio Event. If camera is busy, the new utterance still records audio and its face branch terminates `BUSY`, with request ID 0 because no request was accepted. Missing/unready BLE terminates the face branch `DISCONNECTED` locally. Camera initialization/task allocation failures leave audio Events enabled with face `FAILED`.

The controller emits `EVENT_OPEN` before any Event-owned camera work, guaranteeing request mapping before the first image. EventManager also sends an idempotent OPEN for lifecycle publication, including mappingless Events. It never changes capture/detection/recapture/rotation/sorting/crop/JPEG sequencing. Each validated result emits a bounded face association; each pipeline exit emits a final batch result. Previously returned results survive PARTIAL, timeout and disconnect.

Audio terminal is emitted after the writer closes/finalizes the WAV and appends its metadata, or reports a concrete failure. READY and FAILED are both terminal. Face terminal includes COMPLETED, NO_FACE, PARTIAL, BUSY, TIMEOUT, DISCONNECTED and FAILED. Completion requires both terminals, in either order. COMPLETE_WITH_ERROR preserves both branches.

## Audio semantics and scheduling

Event WAV is RAW PDM capture, 16 kHz / mono / PCM16. DSP/VAD uses AudioVal's default RAW mode with the 80 Hz DC blocker; that does not change saved event PCM. Application pre-roll defaults to 1000 ms with a 2000 ms ring. `audio_last_sample` is an exclusive boundary of actual saved capture, including the raw frames collected while VAD output lags. A cold-start utterance can have no available pre-roll and reports its actual first sample. ESP-SR retains the baseline 5000 ms minimum noise/silence setting; utterance ending can therefore be delayed by about five seconds. No multi-utterance conversation aggregation is added.

Capture priority20/core0, AFE fetch16/core1, audio processing15/core1, EventManager9/core1, SD writer8/core1, camera7/core1. Capture queues without waiting. DMA overflow and queue gaps advance sample indices and increment dropped samples/overruns; ring history resets on a discontinuity. Affected WAVs report FAILED. SD data/metadata leave two queue control slots; BEGIN leaves one for END. Writer errors never cancel camera ownership. The writer performs all filesystem work, including append-only metadata. UART per-frame audio metrics default off; periodic summaries remain enabled.

## Storage and protocol

SD flat layout:

```text
/sd/event_<16HEX>.wav
/sd/audio_events.csv
/sd/face_associations.csv
/sd/event_lifecycle.csv
```

`audio_events.csv` has one terminal row per accepted utterance, with source/format, actual first/exclusive-last sample, VAD samples/timestamps and audio status. `face_associations.csv` has one row per validated returned result, including UNKNOWN/FAILED, and one terminal sentinel per batch: `image_id=0`, `face_index=-1`, `face_count=expected_faces`, status=terminal. The sentinel preserves the request/count even if transfer failed before the first result. It is not a face. `event_lifecycle.csv` appends OPEN/AUDIO_READY/FACE_BATCH_END/COMPLETE. Queue/SD metadata failures are explicitly counted and logged; an unavailable SD cannot promise local artifacts.

Protocol v2 keeps the 32-byte header and FACE_SEQUENCE reserved word. New messages are 15 OPEN (16-byte payload), 16 AUDIO_READY (32), 17 FACE_BATCH_END (32), 18 COMPLETE (16). FACE payload has nine trailing reserved bytes; no compiler struct is copied onto wire. All event messages have image/chunk/flags/reserved header fields zero. MTU 67 is required. Audio bytes are never sent. See `docs/BLE_PROTOCOL.md` for wire layouts and enums.

PC `EventStore` persists `events` and `event_faces` independently of identity embeddings. Request/image ownership is unique within boot session, allowing original counters to restart after reboot. Each recognition is saved before BLE write, and sent state is tracked separately. Exact duplicate lifecycle messages are idempotent; conflicts/stale/crossed owners are rejected. Disconnect observations retain every face and leave audio pending until a real device terminal arrives. Notifications missed during disconnect are not invented; SD/structured logs remain authoritative evidence for such Events. No unbounded firmware replay or audio upload is added in Event 1.

## Build and inspect

```powershell
powershell.exe -ExecutionPolicy Bypass -File .\tools\idf.ps1 -App event1_audio_face build
powershell.exe -ExecutionPolicy Bypass -File .\tools\test_event_ledger.ps1
powershell.exe -ExecutionPolicy Bypass -File .\tests\event1\run_audio_tests.ps1
python -m pytest pc_client tests/event1 -q
python -m ruff check pc_client tools/inspect_event_sd.py tests/event1/test_sd_metadata.py
python -m mypy pc_client/src
python -m pc_client --event-database event_store.db run --wait-only
python tools/inspect_event_sd.py <copied-SD-directory> --output events.json
```

The SD inspector retains available artifacts while marking a declared completion INCOMPLETE when OPEN or either terminal transition is missing/inconsistent; the CLI writes the JSON and exits2 for missing completion evidence.

Integrated flash includes the SR model image at 0x610000 and uses the dedicated `firmware/event1.partitions.csv` (6 MiB app, 1 MiB model). Use the generated app flash arguments or `tools/idf.ps1 -App event1_audio_face -p <port> flash monitor`; flashing just the application binary omits the VAD model.

<<<<<<< HEAD
On a normal first startup, `event_audio: ... recorder init failed` indicates recorder queue allocation failed before SD mounting. ESP-IDF's dynamic `xQueueCreate()` uses internal RAM even with PSRAM enabled, so camera/BLE allocations can leave too small a contiguous block. The recorder keeps queue control in internal RAM and allocates packet storage in PSRAM, with an internal fallback. The current queue has 256 slots (288,768 PSRAM bytes), retaining two reserved control slots and about five seconds of actual 320-sample packets. A successful boot logs `sd_recorder: queue ready: depth=256 bytes=288768 storage=PSRAM`. Allocation failures report requested size and heap availability; writer-task allocation failures identify their 6,144-byte internal stack separately.

Hardware testing on 2026-11-02 first exposed a writer stall that filled the original 32-slot queue and dropped 19 capture packets, prompting an increase to 64 slots. A later observation lasting about 6.4 hours with the 64-slot queue still recorded 121 cumulative queue overruns and 38,720 dropped samples (2.42 seconds at 16 kHz), so the queue was increased to 256 slots. Production recorder/FIFO host regressions pass with the writer paused for 980 ms and 4,980 ms, including PCM copies/order and END admission at saturation. These bounded host tests do not establish long-duration hardware acceptance of the new queue size; that verification remains pending.

The integrated Wi-Fi profile keeps NimBLE host allocations in PSRAM, disables Wi-Fi IRAM acceleration, and bounds the single-client radio buffers. Camera RGB565 staging DMA is 16 KiB, preserving the existing framebuffer/cache path while leaving DMA-capable memory for SD sector reads after networking globals have been initialized. Task stacks and I2S DMA remain internal. Existing saved sdkconfig choices must match the app defaults. These choices trade peak Wi-Fi throughput and extra camera DMA interrupts for memory headroom; throughput and long-duration acceptance still require hardware evidence.
=======
On a normal first startup, `event_audio: ... recorder init failed` indicates recorder queue allocation failed before SD mounting. The 32 packet slots require 36,096 bytes; ESP-IDF's dynamic `xQueueCreate()` uses internal RAM even with PSRAM enabled, so camera/BLE allocations can leave too small a contiguous block. The recorder now keeps queue control in internal RAM and allocates packet storage in PSRAM, with an internal fallback. A successful boot logs `sd_recorder: queue ready: depth=32 bytes=36096 storage=PSRAM`. Allocation failures log the requested size plus free/largest internal and PSRAM blocks, and the app reports the ESP error name separately for initialization and start failures.
>>>>>>> fad0770f9a6e5c0128515521f1e160a23e27bf04

In the observed boot, the AFE printed its pipeline/configuration and then exited with `DSP init failed: ESP_ERR_NO_MEM`: library creation had completed, but the application bridge or fetch task could not be allocated. The 512-sample bridge needs 11,384 bytes of PCM buffers and queue storage; the capture-to-processing queue needs another 7,968 bytes. Both now allocate task-only payloads in PSRAM first, with internal fallback and internal queue control. Task stacks, semaphores and I2S DMA stay internal. `esp_sr_afe: bridge ready` reports the backing for each buffer; failures identify the resource and report heap sizes before cleanup. The pipeline also logs internal/PSRAM/DMA free and largest blocks around DSP/capture initialization. The WakeNet warning is expected with `wakenet_init=false`; this profile runs VADNet without wake-word detection.

The next hardware boot mounted SD and created the AFE bridge, but I2S DMA allocation failed: internal free was 20,823 bytes with a largest block of 1,536 bytes; DMA free was 19,275 bytes with a largest block of 1,216 bytes. Startup now reserves the processing task's 8,192-byte internal stack and initializes I2S before ESP-SR. I2S retains eight 320-sample mono PCM16 DMA buffers (5,120 payload bytes) and reserves the capture task's 4,096-byte stack. The AFE reserves its 4,096-byte fetch stack before loading/creating the library. Workers wait for startup notifications; capture stays disabled until the pipeline is fully initialized. Capture and processing workers remain allocated while stopped so restart needs no new stacks for those workers. Partial init/start failures use `audio_pipeline_deinit()` to stop workers and release DSP, I2S, recorder, ring and queue resources; stop timeouts preserve resources still in use. This prevents an unsuccessful boot from leaving AFE repeatedly reporting an empty input ring. A successful boot now logs `audio_capture: resources ready` before AFE creation and then `Event 1 running` after capture starts.

Event 1 now selects `CONFIG_BOARD_SD_PROFILE_LEGACY=y`, the existing GPIO21 CS profile, in both effective and default configurations. This matches the locally compiled AudioVal configuration and [Seeed's onboard Sense microSD pin table](https://wiki.seeedstudio.com/xiao_esp32s3_sense_filesystem/#card-slot-circuit-design-for-expansion-boards): CS=21, SCLK=7, MISO=8, MOSI=9. The status LED is disabled because it shares GPIO21. The historical profile identifier remains `legacy-gpio21`. A successful mount logs `SD mounted at /sd`. An `sdmmc_card_init` timeout occurs before filesystem mounting, so first verify the CS profile and physical card connection rather than reformatting the card.

The six phase apps select only their required in-repository components so audio manifests do not contaminate camera-only dependency resolution. `EVENTPIPE_OFFLINE_COMPONENTS` optionally supplies a copied local component cache for compile validation. Normal builds use the registry and generated locks. Source camera/DSP baselines are never written by this repository.

## Remaining acceptance

The full hardware definition of done remains open until `tests/event1/HARDWARE_TEST_PLAN.md` has real evidence. In particular, compile/link and synthetic ownership tests cannot establish zero audio drops during XGA/ESP-DL/JPEG/NimBLE work, microphone calibration, SD latency, physical model inference or real InsightFace recognition. STT, audio upload, AFM/LLM, deferred captures and conversation merge are outside Event 1.
