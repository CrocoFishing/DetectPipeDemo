# BLE-started Wi-Fi SD export acceptance

Status: **pending hardware and Xcode validation**. Host tests and successful firmware compilation do not establish radio coexistence, permission behavior, audio recovery, or throughput.

Limited board results from 2026-11-02 are recorded in [the flash/monitor verification](../../results/SD_EXPORT_HARDWARE_20261102.md): startup, two control/resume cycles, manual NO_FACE captures and a post-export READY WAV. The full matrix below remains the acceptance target.

Use the integrated `event1_audio_face` image and an iPhone running iOS 16 or later. Record board/camera/SD model, card format/capacity, firmware revision, iOS/Xcode versions, negotiated MTU, distance, power source and free/largest internal/DMA/PSRAM heaps. Preserve evidence locally under `results/sd_export/actual/` with credentials redacted.

## Setup

1. Confirm the existing camera/audio Event acceptance setup and WAV/CSV reconstruction first.
2. Verify effective sdkconfig matches the integrated app defaults, including UTF-8 FatFS, Wi-Fi core 1/software coexistence, NimBLE external host allocation, disabled Wi-Fi IRAM acceleration, bounded radio buffers and 16 KiB camera staging DMA. Existing saved sdkconfig choices can override new defaults.
3. Build/flash with `tools/idf.ps1 -App event1_audio_face`. Add the iOS Swift package to a signed iOS host and configure the permissions/entitlement described in `docs/IOS_SD_EXPORT.md`.
4. Prepare a card containing multiple complete Events, an incomplete Event, zero-byte/extra/hidden files, nested directories, spaces, `+`, `%`, `&` and Chinese names. Keep an independent path/size/SHA-256 inventory for comparison.

## Required scenarios

| Scenario | Acceptance |
|---|---|
| Full export | Local path/size/SHA-256 inventory exactly matches the card; manifest terminal/count/digest verified; source files unchanged. |
| Empty card | Valid empty manifest and completion; recording resumes. |
| FAT32 file above 2 GiB | Manifest size/hash and resumed Range beyond signed 32-bit offset match the original; include a file near the FAT32 per-file limit when storage permits. |
| Active utterance/recognition | Already admitted work finishes normally; final lifecycle/face/audio rows are present before READY; new manual triggers return BUSY. |
| Quiesce exceeds 30 s | Export fails with timeout; current utterance is not cut or marked FAILED by export; normal admission returns. |
| MTU 67 | Status fits; long-read Export Info is complete and immutable across read blobs. |
| Hotspot/local-network permission refused | App reports actionable failure; cancel or lease expiry releases export mode. |
| Leave foreground or interrupt Wi-Fi | Partial data/journal retained; reconnect resumes with Range and verifies full file hash. |
| BLE disconnect while HTTP active | Download continues; reconnect/status query recovers session state. |
| Snapshot changes between sessions | Unchanged verified WAVs skipped; changed CSV/partial files downloaded using current content identity and ETag. |
| Truncated/corrupt manifest or file | Never acknowledges completion; preserves useful partials and reports error. |
| Cancel during preparing/hashing/download | Readers close before resume; hotspot and HTTP sockets close; recording restarts. |
| 120 s inactivity | Export expires and recovers without user intervention; valid hashing/streaming is not mistaken for idle. |
| SD removed/read error or oversized path | Explicit failure; no silent file omission or completion claim. |
| Repeated/old session commands | Wrong session rejected; repeated completion/cancel/query behaves consistently. |
| Existing iPhone firmware upgrade | Service Changed/rediscovery refreshes cached characteristics; existing capture UUIDs work. |
| Audio restart allocation failure | Error remains visible over BLE, admission stays gated; cancel retries restoration. |
| Reboot during export | Old session/token invalid; SD remains readable and incomplete Events remain identifiable. |

## Resource and recovery acceptance

Run at least 20 complete export/resume cycles with a speech Event after every cycle. Compare free/largest heap blocks at matching lifecycle points and check for a monotonic decline, leaked sockets/tasks, watchdog resets and DMA allocation failures. Check sample counter and Event sequence progression, reset pre-roll contents, and timestamps in resumed WAV/CSV data. Record measured bytes/second and duration; no throughput guarantee is inferred from nominal Wi-Fi rates.

Run the legacy BLE/manual face flow after export, plus a subsequent VAD Event and SD inspection. Sign off hardware acceptance only when all applicable scenarios have recorded pass/fail evidence; document platform file-offset limitations explicitly rather than dropping unsupported files.
