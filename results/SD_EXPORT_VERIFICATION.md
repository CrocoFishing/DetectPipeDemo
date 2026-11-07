# SD export verification - 2026-11-01

Implementation is complete for host/build review. **Xcode, iPhone and ESP32/SD hardware acceptance are pending.** Build success does not establish Wi-Fi/BLE coexistence, available runtime heap, transfer throughput or recording recovery on a physical board.

<<<<<<< HEAD
**2026-11-02 update:** [Flash/monitor runtime fixes and limited board verification](SD_EXPORT_HARDWARE_20261102.md) cover startup, BLE/SoftAP control/resume, manual NO_FACE captures and post-export READY recordings. A later six-hour observation exposed residual SD queue loss; the queue was increased to 256 slots and re-flashed, with two more control/resume cycles and a post-export READY WAV passing. Production FIFO regressions cover 980 ms and 4,980 ms writer stalls; the current Python regression has 81 passing tests. Full iPhone/file-transfer/endurance acceptance remains pending; the table below preserves the original 2026-11-01 results.

=======
>>>>>>> fad0770f9a6e5c0128515521f1e160a23e27bf04
## Passed checks

| Check | Result |
|---|---|
| Python unit/contract regression | 70 passed: PC protocol/store/session, C++ golden packets, SD reconstruction and complete/busy/incomplete app examples. |
| Protocol consistency command | 3 passed: normative YAML agrees with Python/C++/Swift constants and export payload definitions. |
| Ruff | Passed for PC client, SD inspector and SD metadata/contract tests. |
| mypy | Passed, 9 source files. |
| Actual C audio artifact checks | Passed WAV/PCM, storage error/recovery, pre-roll/reset/discontinuity and queue admission cases. |
| Recorder lifecycle | 5 cases passed. |
| AFE allocation/lifecycle | 15 cases passed. |
| Audio pipeline lifecycle | 9 cases passed, including export suspend, retained SD, restoration failure/retry and partial suspend. |
| I2S capture lifecycle | 11 cases passed. |
| Export integration coordinator | Passed drain ordering, timeout preservation, cancellation, partial suspend, restore retry, writer barrier failure and missing SD. |
| Event ledger | Actual C++ ledger passed 200 Events, branch completion order, reboot/session and stale/failure ownership checks. |
| Export core C++ | Passed UTF-8/path traversal/depth, 64-bit Range (including offsets above 2 GiB), exact NDJSON escaping and integer-safe receipts. |
| Firmware builds | `event1_audio_face`, `phase05_ble` and `phase06_end_to_end` passed with ESP-IDF 6.0. |
| Whitespace check | `git diff --check` passed. |

The integrated image fits its 6 MiB app partition with approximately 34% free. Effective configuration selects UTF-8 FatFS filenames, Wi-Fi core 1 and software radio coexistence. The SR model partition remains part of the integrated flash arguments.

## Reproduction

From the repository root:

```powershell
# Use a new workspace-local basetemp for each run; pytest removes an existing
# basetemp directory. This avoids this machine's inaccessible default temp root.
New-Item -ItemType Directory -Path results/sd_export/actual -Force | Out-Null
.\pc_client\.venv\Scripts\python.exe -m pytest pc_client tests/event1 -q --basetemp=results/sd_export/actual/pytest-review
.\pc_client\.venv\Scripts\python.exe tools/check_protocol_consistency.py
.\pc_client\.venv\Scripts\python.exe -m ruff check pc_client tools/inspect_event_sd.py tests/event1/test_sd_metadata.py tests/event1/test_event_contract_fixtures.py
.\pc_client\.venv\Scripts\python.exe -m mypy pc_client/src
powershell.exe -ExecutionPolicy Bypass -File tests/event1/run_audio_tests.ps1
powershell.exe -ExecutionPolicy Bypass -File tools/test_event_ledger.ps1
powershell.exe -ExecutionPolicy Bypass -File tools/test_sd_export_core.ps1
powershell.exe -ExecutionPolicy Bypass -File tools/idf.ps1 -App event1_audio_face build
powershell.exe -ExecutionPolicy Bypass -File tools/idf.ps1 -App phase05_ble build
powershell.exe -ExecutionPolicy Bypass -File tools/idf.ps1 -App phase06_end_to_end build
```

On this Windows installation, sandboxed async BLE simulation stalled and the default pytest temp directory had ACL errors. The successful full run used a workspace-local temp directory outside the sandbox. The ESP-IDF profile's optional EIM synchronization also required running the build outside the sandbox. These environmental failures were corrected before recording the passing results above.

## Remaining acceptance

- Build the Swift package and run its XCTest target with an iOS Simulator destination on macOS/Xcode. New Swift code was statically reviewed; Windows has no Swift/iOS compiler.
- Use [the iPhone/SD hardware matrix](../tests/event1/SD_EXPORT_HARDWARE_TEST_PLAN.md), including permissions, GATT upgrades, dropped BLE/Wi-Fi, large FAT32 files, card errors, cancellation and repeated export/audio recovery.
- Record runtime free/largest internal, PSRAM and DMA heaps; software tests cannot prove that all allocations fit on the board.
- Verify complete file inventories/hashes against an independent SD copy, and capture a new speech Event after every export/resume cycle.

See [BLE/HTTP protocol](../docs/BLE_PROTOCOL.md), [iOS integration](../docs/IOS_SD_EXPORT.md) and [Event data contract](../docs/EVENT_DATA_MODEL.md).
