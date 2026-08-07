# Phase 6 Test Plan

- Purpose: validate registration/SQLite model isolation and both complete trigger directions with real cropped JPEG and recognition result.
- Preconditions: Phases 1–5 passed; at least two good samples registered per person; InsightFace model downloaded; GPIO2 button wired; BLE adapter ready.
- Hardware: full XIAO ESP32-S3 Sense, OV3660, external D1/GPIO2 button, PC BLE; no screen/mic/SD/UART adapter.
- Firmware: `firmware/apps/phase06_end_to_end`.
- Build/flash: wrapper `-App phase06_end_to_end build`, then `-p COM5 flash monitor`.
- Steps: run host tests; register samples; start PC client with `--wait-only`, press external button 10 times; restart normal `run` to send PC trigger 10 times; include known/unknown/no-face/multiple-face/stale-result/disconnect cases; match request/image IDs across logs.
- Data: ESP serial logs, PC logs/JSONL, recognition results, BLE CSV, DB metadata (do not copy sensitive embeddings into public reports).
- Expected: each accepted path follows all states; ESP logs only face detection before transfer and PC performs face recognition; no-face safely returns; result IDs match; IDLE resumes.
- Pass: both directions complete repeatedly, JPEG decodes, expected known/UNKNOWN decisions use configured threshold, no stale result accepted, no buffer/queue leak.
- Fail: ESP performs identity recognition, PC skips landmark re-detection/alignment, name used as unique ID, ID mismatch accepted, concurrent captures, or failure does not recover.
- Recovery: stop client, disconnect, power-cycle, reconnect/resubscribe, use fresh request ID; handle DB/model mismatch by a new compatible DB; record failures.
- Results: `results/phase06/actual/`.

