# Testing Guide

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

