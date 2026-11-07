# Flash/monitor and runtime verification - 2026-11-02

Limited board verification completed on the connected XIAO ESP32-S3 Sense (ESP32-S3 revision 0.2, OV3660, 8 MiB flash/PSRAM), USB Serial/JTAG **COM8**, ESP-IDF 6.0. The final integrated firmware and VAD model were flashed and verified by esptool. NVS and SD contents were preserved. The serial monitor was closed after collecting evidence, leaving the application running and COM8 available.

## Actual failures and fixes

| Observation | Change |
|---|---|
| Camera 8 KiB task and audio-processing 8 KiB task could not be allocated; internal largest block about 4.5-6 KiB. | Integrated profile puts supported NimBLE host/MSYS allocations in PSRAM, keeping controller/DMA/task stacks internal. |
| Camera then started, but SD writer 6 KiB and AFE fetch 4 KiB stacks still could not fit; largest block 2,816 bytes. | Disabled Wi-Fi IRAM acceleration to return shared internal memory to the heap. |
| Wi-Fi initialization expected 10 static RX buffers but allocated only 3; START returned WIFI_START_FAILED. | Bounded single-client profile: static RX 4, dynamic RX/TX 8 each, RX/TX BA window 4, management short buffers 6; default five RX management buffers retained. |
| After the first networking initialization/resume, largest DMA block was 368 bytes. SD sector reads require a 512-byte DMA bounce buffer and returned ESP_ERR_NO_MEM/EIO. | Reduced camera RGB565 staging DMA from 32 KiB to 16 KiB; PSRAM DMA mode remains disabled and the existing framebuffer/cache path is retained. |
| A separate long recording filled the original 32-slot writer queue: 19 rejected 320-sample packets, 6,080 samples lost. | Increased the PSRAM-backed writer queue to 64 slots (72,192 bytes), preserving two control slots and about 1.2 seconds of actual audio packets. Added an actual producer/FIFO stalled-writer regression. |

Only the integrated app's defaults/effective sdkconfig received the radio/camera memory profile. The camera format, full XGA frame buffers, detector, VAD/AGC/NS settings and task stack sizes were retained. Radio peak throughput is a tradeoff and has not been measured.

## Initial short-run results (64-slot queue)

- **Startup:** camera, BLE, SD mount, SD writer, I2S capture and AFE initialized; log reported `Event 1 running ... Wi-Fi SD export=available` and `depth=64 bytes=72192 storage=PSRAM`.
- **BLE/Wi-Fi:** exact-device Windows BLE smoke passed two START -> READY -> Export Info read -> CANCEL -> CANCELLED cycles. ATT MTU 517; each credential JSON read was 227 bytes. Password/token values were neither printed nor saved in the report. No client Wi-Fi network was changed.
- **Restoration:** largest internal/DMA block was **9,216 bytes** after both export/resume cycles. Internal free was 28,591 then 28,599 bytes at the two matching resume points. The first network initialization retains global TCP/IP/event infrastructure; this is distinct from an unbounded per-session leak.
- **Camera:** two diagnostic manual requests completed NO_FACE with zero invalid protocol packets. Serial evidence contains six captured RGB565 XGA frames, each 1,024 x 768 / 1,572,864 bytes, exercising the bounded recapture path. No face JPEG was produced, saved or validated in these requests.
- **Post-export recording:** Event `0000000D00000001` finalized `/sd/event_0000000D00000001.wav` with audio READY / ESP_OK and sample range `[2888640, 3188800)` (300,160 PCM frames, 18.76 s including pre-roll/VAD tail). Audio/lifecycle metadata publication completed. This is the writer's successful close/metadata result, not an independent WAV read-back/hash.
- **Final observation at 338,985 ms:** audio running, captured samples 5,346,880, dropped samples 0, queue overruns 0, ring overruns 0, SD short writes 0 and metadata drops 0. SD queue high-water 15; internal free 28,563 bytes. No panic, watchdog or stack-overflow appeared in this final run.
- Four Event protocol publication failures in this recording were expected after the diagnostic BLE client disconnected; face status was DISCONNECTED. Local audio/SD remained READY. Offline association is not a transport replay guarantee.
- **Host regression:** 81 Python tests passed; Ruff and strict mypy passed. The complete audio host suite including the new 980 ms stalled-writer/PCM-order/END-reservation regression passed.

## Follow-up after the user's speech test

The board remained running without reset for approximately 6.4 hours. Audio capture and SD remained active, with zero SD short writes and metadata drops; however, the 64-slot queue had reached high-water 62 and accumulated 121 queue overruns / 38,720 dropped samples (2.42 seconds at 16 kHz). This invalidates any long-duration zero-loss conclusion from the earlier short run. The write timer covers `fwrite`, so its maximum cannot establish a bound for all card/flush/metadata stalls.

After the user confirmed speaking, the no-reset monitor recorded two new terminal audio results:

| Event | Saved sample range | Audio result |
|---|---|---|
| `0000000D0000002C` | `[365381056, 365589760)` / 13.044 s | READY / ESP_OK |
| `0000000D0000002D` | `[367190464, 367339200)` / 9.296 s | READY / ESP_OK |

The cumulative dropped-sample/overrun counters did not increase during these recordings. Lifecycle AUDIO_READY/COMPLETE publication occurred; their face branch was DISCONNECTED because the BLE diagnostic client was offline, so overall COMPLETE_WITH_ERROR does not indicate an audio write failure. These are successful writer close/metadata results, without independent SD read-back.

The queue was subsequently increased to **256 slots / 288,768 PSRAM bytes**, retaining the control reservation policy and approximately five seconds of incoming 320-sample packets. The complete audio host suite passed again, including actual production producer/FIFO tests with the writer paused for 980 ms and 4,980 ms, copied PCM/order checks, saturation counters, END admission and rejected-BEGIN cleanup.

The updated image was built and flashed on COM8 with all written images verified. App binary: 4,132,912 bytes; SHA-256 `3EE99CDC28617978C97759E189BA54F6FF65F963FEA04FF0F6FDBF6DF50EC14D`. Startup confirmed `depth=256 bytes=288768 storage=PSRAM` and normal camera/SD/AFE/audio initialization.

- Two more BLE START -> READY -> credential read -> CANCEL -> CANCELLED cycles passed with MTU 517, 227-byte Export Info and no export errors. No client Wi-Fi settings were changed.
- After restoration, internal free was 28,591 then 28,587 bytes; largest internal/DMA blocks were 9,216 bytes in both cycles. Audio capture resumed with approximately 1.86 million PSRAM bytes available.
- A subsequent VAD Event `0000000E00000002` finalized READY / ESP_OK with range `[1023360, 1120000)` (96,640 frames / 6.04 s), confirming SD recording after both export/resume cycles.
- At 162,690 ms, captured samples were 2,524,800; dropped samples, queue overruns, ring overruns, SD short writes and metadata drops were all zero. This is a short verification of the 256-slot configuration; its endurance acceptance remains pending.
- Startup also reported `RECOVERY,failed` / `ESP_ERR_INVALID_SIZE` for the older `0000000D00000030.wav`. Its contents were preserved. The file was not independently inspected, and no successful recovery or cause is claimed. New recordings in boot `0000000E` finalized normally.

## Evidence and limits

Private raw evidence stays under ignored `results/sd_export/actual/20261102/`:

- `initial_failure.log`, `psram_only_failure.log`, `iram_off_wifi_failure.log`: original allocation and SD DMA failures.
- `bounded_wifi_quiesce_timeout.log`: an active utterance exceeded the 30 s export preparation limit; admission was restored and the recording was not truncated by export. That older queue configuration separately lost data during SD stalls.
- `final_monitor.log`, `final_ble_smoke.json`, `camera_smoke.json`: final successful startup, control/resume and manual capture evidence.
- `log.EventPipeDemo.20261102110923.txt`: no-reset follow-up, including the two new READY speech Events and cumulative long-run counters.
- `queue256_monitor.log`, `queue256_ble_smoke.json`: updated queue configuration's control/resume and post-export READY recording evidence.

Earlier interrupted/failed recordings were left on the card; no historical audio was invented or deleted. The initial short-run boot successfully inspected its newest prior WAV (`RECOVERY,no_action,...,ESP_OK`); the later boot's separate recovery failure is recorded above.

This does **not** complete the [full hardware matrix](../tests/event1/SD_EXPORT_HARDWARE_TEST_PLAN.md). iOS/Xcode, actual HTTP manifest/file downloads and Range resume, face JPEG transfer after this profile change, long-duration SD continuity with the 256-slot queue and the 20-cycle endurance matrix remain to be validated. The original 5.6-minute observation was followed by the longer 64-slot run above; that longer run exposed residual loss and is not a zero-loss acceptance pass.

Re-run the credential-safe control check with:

```powershell
.\pc_client\.venv\Scripts\python.exe tools/smoke_sd_export_ble.py --address <device-BLE-MAC> --cycles 2 --timeout 45 --report results/sd_export/actual/ble-smoke.json
```

The tool does not join the AP or download SD files. The iOS integration and [Event data contract](../docs/EVENT_DATA_MODEL.md) describe those separate flows.
