# Event 1 verification — 2026-10-04 (Asia/Taipei)

Software integration is implemented and compile/host checks pass. Hardware acceptance and the complete Event 1 definition of done remain pending. No ESP32 serial ports were detected; no flash, microphone/camera/SD/BLE hardware run or real InsightFace recognition was performed.

## Frozen sources and repository

| Item | Recorded value |
|---|---|
| Independent checkout | `recordNdetect`, branch `codex/event1-audio-face-association` |
| DetectPipeDemo multiface baseline | `981a531c2880086d259d0c1604756a3a9730d4c2` |
| AudioVal baseline | `148b0af0dbe017b3d921cbaa589ec8e88a0e7f7e` |
| Source worktrees after integration | both clean; original baselines untouched |
| Integration remote | none; temporary local-source origin removed |
| Physical hardware revision | NOT RECORDED; no attached board |
| Event SDK configuration | copied to `results/event1/actual/sdkconfig.event1` |
| Exact Event dependencies | tracked `firmware/apps/event1_audio_face/dependencies.lock` |

Phase 0 source refs are frozen, and the original 24-test PC suite passed in the integration checkout. The requested fresh hardware validation of both baselines is NOT RUN, so Phase 0 hardware acceptance is still open.

## Firmware compile/link

ESP-IDF 6.0 / Windows toolchain, target ESP32-S3. All seven targets passed. These results establish compilation/linking and partition-size validity.

| Target | Binary bytes | Result |
|---|---:|---|
| phase01_camera |253312|PASS|
| phase02_trigger |156256|PASS|
| phase03_face_pipeline |2606368|PASS|
| phase04_protocol |138944|PASS|
| phase05_ble |531648|PASS|
| phase06_end_to_end |2924368|PASS|
| event1_audio_face / EventPipeDemo |3626816|PASS|
| Event SR model image |288069|PASS, generated|

Legacy regression builds used copied baseline packages with Component Manager disabled and `EVENTPIPE_OFFLINE_COMPONENTS`; ESP-DL3.3.9 / human_face_detect0.5.0 / esp32-camera2.1.7. The integrated target used normal registry resolution with ESP-SR2.5.3 / ESP-DL3.3.11 / dl_fft0.8.0 / ESP-DSP1.8.0. ESP-SR requires ESP-DL>=3.3.10, so the integrated manifest pins3.3.11. Detector selection and existing XGA/recapture/final180/multiface code remain preserved; actual behavior after dependency integration still needs hardware validation.

Integrated app size is0x375740 in the0x600000 app partition, leaving42% free. Flash arguments include the SR model at0x610000. Raw logs are in `results/event1/actual/`, including `event1_build_final.log` and `builds/<legacy-app>.log`.

## Host checks

| Check | Result / evidence |
|---|---|
| `pytest pc_client tests/event1 -q` | **56 passed**, `actual/host_pytest.log` |
| Ruff lint + format |PASS for PC client, SD inspector and its tests|
| mypy `pc_client/src` |PASS,9 source files|
| actual C++ Event ledger / terminal helpers |PASS, MSVC C++17 `/W4 /WX`,200 Events across two boot sessions; both completion orders, stale IDs, duplicate audio/END, failures, BUSY, bounded capacity|
| actual C++ / Python wire packets |PASS, all4 v2 messages byte-for-byte golden equality; malformed padding/status/count rejection|
| actual C WAV/ring + production queue admission |PASS, MSVC C11 `/W4 /WX`; PCM/header, short write, open/header/flush/close errors, interrupted-file repair, ring wrap/zero/discontinuity/PSRAM fallback, END reservation under writer stalls|
| durable PC EventStore |100 Events in each completion order, partial/disconnect/audio failures, UNKNOWN/NO_FACE/BUSY, unsigned IDs, reboot counter reuse, stale/crossed ownership, late old-boot messages and reconnect/manual-ID reuse|
| SD reconstruction |RAW pre-roll, WAV/sample count, all face associations, partial zero-result sentinel, crossed request rejection, missing lifecycle evidence retained as INCOMPLETE|
| Git diff whitespace check |PASS|
| Swift compilation |NOT RUN; compiler unavailable on this host|

The SD inspector validates WAV structure/frame count but cannot prove physical sample continuity from WAV bytes alone. Firmware DMA/queue gap instrumentation and real captured_samples/dropped_samples/queue_overruns evidence are required.

## Artifact fingerprints

| Artifact | SHA256 |
|---|---|
| Event sdkconfig |`23FA4F04FBFB3EE2C0654BAC6BAD23C8BEEA9DA9171ACE911F2985168BC1CE30`|
| Event dependencies.lock |`374334F55BB4DDEFA8AB03441C336F167825B7C37B3200327036917BD8703A43`|
| EventPipeDemo.bin |`E4F700DD9D0B53D643F7446E183D53954F7C6AFBAAFE5F0DE88F020A20453E50`|
| srmodels.bin |`2485A4CAF5754547459622A0BA126428C3DAB1CEF8BF57BA24E463586D2232EE`|

## Open acceptance

All E1-01–E1-14 hardware rows are NOT RUN in `tests/event1/HARDWARE_TEST_PLAN.md`. In particular, zero audio drops/overruns during full XGA/ESP-DL/JPEG/BLE workload, 60-second combined boot, real pre-roll/VAD/WAV quality, physical SD error/stall behavior, heap/PSRAM stability, real face recognition and preserved-NVS reboot uniqueness are unverified. Synthetic tests do not satisfy those hardware checks.

Bounded resources are explicit:16 concurrent firmware Event owners, one camera request,16 queued face metadata rows,32 writer packets. Exhaustion/SD failures are logged/counted; no occupied owner is overwritten. Notifications missed while BLE is disconnected are not replayed in Event1; PC retains incomplete Events and local SD/structured logs supply reconstruction evidence. These limits need evaluation in the hardware stress run.
