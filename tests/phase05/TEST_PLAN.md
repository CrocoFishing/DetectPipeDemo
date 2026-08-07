# Phase 5 Test Plan

- Purpose: validate advertising/connect/reconnect/subscriptions/MTU-derived chunks, sequence/CRC/application completion, congestion/disconnect recovery, and RSSI/quality metrics.
- Preconditions: Phase 4 passed; Windows BLE adapter and venv; test locations measured; board powered.
- Hardware: XIAO ESP32-S3 Sense and PC BLE adapter; camera/button not required by synthetic app.
- Firmware: `firmware/apps/phase05_ble`.
- Build/flash: wrapper `-App phase05_ble build`, then `-p COM5 flash monitor`.
- Steps: subscribe via `ble-test`; for each 0.5/2/5 m × unobstructed/human/one wall × 20/50/100 kB, run ≥10 transfers; annotate condition in copied CSV; repeat disconnect mid-transfer and reconnect; test without subscription and low/changed MTU where the OS permits.
- Data: `ble_metrics.csv`, ESP serial log, environment notes; fields listed in `docs/TESTING.md`.
- Expected: dynamic negotiated MTU, legal chunk size/count, exact bytes/CRC, max 1 Hz RSSI, bounded retry, advertising after disconnect.
- Pass: ≥95% success per condition, no corrupt image accepted, successful transfer has zero missing chunks, failures recover; record actual thresholds if project changes them.
- Fail: fixed-MTU assumption, corrupt success, unbounded retry/queue, buffer retained after disconnect, or missing required metrics.
- Recovery: reconnect/resubscribe/fresh request ID; move closer; remove interference; power-cycle; preserve failure CSV/log.
- Results: `results/phase05/actual/ble_metrics.csv` and copied template.

