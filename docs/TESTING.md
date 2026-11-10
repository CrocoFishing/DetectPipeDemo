# Testing Guide

Recurring Event/file-management regressions: run `tools/test_event_ledger.ps1`, `tools/test_sd_export_core.ps1`, `tests/event1/run_audio_tests.ps1`, and Python `pytest pc_client/tests tests/event1 -q` from the repository venv. Event tests cover 30-second cadence/skip behavior, no scheduling at five minutes, bounded NO_FACE retries, prior-error retention, multiple batches, duplicate/stale ownership and lossless old-database migration. Deletion core tests verify that path/ETag/directory/missing-file checks prevent unlink. Inspect SD fixtures with `tools/inspect_event_sd.py`. Swift contract tests require Xcode/iOS SDK; Windows does not establish Swift compilation or iPhone behavior. Hardware cases E1-16 onward and SD delete cases are required separately.

Run phases in order and copy each template to `results/phaseNN/actual/` before recording. Never mark a hardware test PASS from build output alone.

Common firmware commands:

```powershell
powershell.exe -ExecutionPolicy Bypass -File .\tools\idf.ps1 -App phaseNN_name build
powershell.exe -ExecutionPolicy Bypass -File .\tools\idf.ps1 -App phaseNN_name -p COM5 flash monitor
```

Common host verification:

```powershell
.\pc_client\.venv\Scripts\python.exe -m pytest .\pc_client -q
.\pc_client\.venv\Scripts\python.exe -m ruff check .\pc_client
.\pc_client\.venv\Scripts\python.exe -m mypy .\pc_client\src
```

Metrics log format is `[METRIC] phase=5 image_id=12 metric=ble_transfer_ms value=842 unit=ms`. PC BLE quality writes CSV. Serial logs should be saved as UTF-8 text and test decisions as Markdown/JSONL/CSV rather than screenshots alone.

Phase 5 matrix has 3 distances × 3 environments × 3 sizes × at least 10 repeats = at least 270 transfers:

| Distance | Environment | Image bytes | Repeats |
|---|---|---:|---:|
| 0.5 m, 2 m, 5 m | unobstructed, human obstruction, one wall | 20,000 / 50,000 / 100,000 | ≥10 each |

Record RSSI current/min/max/mean/stddev, negotiated MTU, bytes, chunks, duration, throughput, missing/duplicate/CRC/disconnect/reconnect/success/retry/congestion. Default pass: ≥95% successful transfers in each condition, zero accepted CRC-corrupt images, zero missing chunks in a successful image, no leak trend across 10 requests, and every failure returning to advertising/IDLE. Projects may tighten these demo thresholds before product use.

See each phase plan for exact preconditions, recovery, and evidence path.

