# Phase 4 Test Plan

- Purpose: validate serialize/deserialize, partial/invalid/version/length/CRC/unknown errors and image sequence/reassembly/timeout behavior.
- Preconditions: PC venv installed; no board is required for host tests.
- Hardware: none for required tests; optional board only for firmware smoke log.
- Firmware: `firmware/apps/phase04_protocol`.
- Build/flash: wrapper `-App phase04_protocol build`; optional `-p COM5 flash monitor`.
- Steps: run `run_test.ps1`; inspect each named pytest; optionally observe firmware valid/invalid-magic smoke marker.
- Data: pytest text/JUnit if desired, plus protocol consistency output.
- Expected: all tests pass; invalid inputs are rejected; out-of-order legal chunks reassemble; duplicate counted; missing/CRC/timeout fail.
- Pass: every listed test passes and YAML/Python/C++/Swift constants agree.
- Fail: any corrupt/partial packet accepted, missing sequence accepted, incompatible BLE-specific format, or UART made mandatory.
- Recovery: reinstall locked venv, ensure editable/path config, inspect YAML as source, do not hand-edit numeric constants independently.
- Results: `results/phase04/actual/`.

