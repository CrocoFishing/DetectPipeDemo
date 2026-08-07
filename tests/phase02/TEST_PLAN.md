# Phase 2 Test Plan

- Purpose: verify post-init GPIO2 ISR, 40 ms debounce, one request per press/release, mock BLE request, BUSY, duplicate ID, namespaces, and state logs.
- Preconditions: Phase 1 passed; momentary button wired D1/GPIO2 to GND; BOOT/GPIO0 untouched.
- Hardware: board, external button, USB only.
- Firmware: `firmware/apps/phase02_trigger`.
- Build: `powershell.exe -ExecutionPolicy Bypass -File .\tools\idf.ps1 -App phase02_trigger build`.
- Flash: same wrapper with `-App phase02_trigger -p COM5 flash`.
- Run steps: monitor; verify automatic mock ID 42 accepted, ID 43 BUSY, later ID 42 duplicate; press/release 10 times normally, 10 bounce-prone taps, and hold 5 s; attempt presses during mock active; inspect source and bit31 namespaces.
- Data: serial log and JSONL `{time,action,request_id,source,result,state}`.
- Expected: `expected_output.txt`; each physical press has exactly one EXTERNAL_BUTTON request, long hold has one, busy press is rejected and never later queued.
- Pass: all arbitration/debounce/namespace/state checks pass without capture work in ISR or queue growth.
- Fail: GPIO0 required, repeated long-press requests, accepted concurrent work, duplicate accepted, or state/request mismatch.
- Recovery: verify pull-up/wiring, release button before reset, power-cycle, save failure log, retry after checking GPIO2 only.
- Results: `results/phase02/actual/`.

