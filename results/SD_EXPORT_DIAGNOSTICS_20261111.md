# SD export failure analysis — 2026-11-11

The supplied board log shows an SD DMA allocation failure and a later HTTP listener conflict. This change has host coverage; physical-board transfer and repeated-cycle acceptance remain pending.

## Observations and limits

- At 254391 ms, `sdmmc_read_sectors` reports `ESP_ERR_NO_MEM` (`0x101`). The installed ESP-IDF 6.0 implementation allocates an internal DMA-capable sector bounce buffer when the destination is external RAM or unaligned. The export I/O buffer is in PSRAM, so available PSRAM does not satisfy this allocation. The log does not record the DMA heap at the exact failure.
- Recording resumes after the failure. At 254995 ms it reports 24,615 free internal bytes but a largest block of only 3,456 bytes. Subsequent export suspension reports largest internal blocks of 9,728 and then 7,680 bytes; total free memory alone does not establish that another internal 8 KiB HTTP stack fits.
- At 294279 ms, HTTP `listen()` fails with errno 112. The installed Xtensa toolchain defines 112 as `EADDRINUSE`. SO_REUSEADDR is already enabled; adding it again would not resolve a surviving listener.
- The previous network startup at 289491 ms unwinds before reporting READY. The old service did not log its returned startup error, so the log cannot prove why that attempt failed or which path left a listener. The installed SDK's `httpd_start()` also frees HTTP metadata without closing initialized sockets if task creation fails. Moving the HTTP stack to PSRAM reduces exposure to that failure; this repository change does not patch the external SDK. A reset is required to clear an already orphaned listener.
- Separately, the old application ignored `httpd_stop()` errors and cleared the handle before confirming the HTTP worker exited. That could orphan a live server and resume SD recording while a reader still owned the snapshot. This is a confirmed code defect; the supplied log does not establish that this particular path occurred.

## Changes

- Put the integrated HTTP task's 8 KiB stack in PSRAM, including its local FatFS file cache. The task serves SD and performs no flash writes.
- Prefer PSRAM for eligible Wi-Fi/lwIP allocations. Explicitly bound the resulting static TX pool to four buffers and the PSRAM TX cache to eight. Preserve the existing RX limits.
- On HTTP shutdown failure, retain the server, network and I/O buffer, publish RESTORING/INTERNAL, and retry on the export worker. Restore audio only after HTTP shutdown succeeds. Preserve the intended terminal state/error through retries.
- Log internal/DMA free and largest blocks at network readiness and startup/read/shutdown failures. Reject short snapshot reads before sending their bytes or acknowledging completion.
- Update both tracked defaults and this checkout's saved, ignored sdkconfig; CMake confirmed the effective PSRAM, external-stack and bounded TX settings.

## Validation

- `tools/test_sd_export_core.ps1`: core cases and production-service network lifecycle test passed. The latter injects three failed shutdowns, recovery, duplicate cleanup, short reads and SD errors; it verifies no buffer release or recording resume occurs before successful shutdown.
- `tests/event1/run_audio_tests.ps1`: WAV/artifact, recorder initialization/FIFO, AFE, pipeline, capture and export coordinator tests passed.
- Integrated `event1_audio_face` ESP-IDF 6.0 build passed; the image fits the 6 MiB app partition with 34% free. The build required the installed toolchain outside the sandbox; no SDK source was modified.
- `git diff --check` passed.

Run the repeated export/hash/download/resume matrix in [the hardware plan](../tests/event1/SD_EXPORT_HARDWARE_TEST_PLAN.md), comparing the new `network_ready` and failure DMA diagnostics. Host tests do not establish throughput or runtime heap margins.
