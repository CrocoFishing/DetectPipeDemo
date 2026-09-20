# Phase 6 Test Plan

- Purpose: validate registration/SQLite model isolation and both trigger directions with size-sorted, sequential recognition of every detected face.
- Preconditions: Phases 1–5 passed; at least two good samples registered per person; InsightFace model downloaded; GPIO2 button wired; BLE adapter ready.
- Hardware: full XIAO ESP32-S3 Sense, OV3660, external D1/GPIO2 button, PC BLE; no screen/mic/SD/UART adapter.
- Firmware: `firmware/apps/phase06_end_to_end`.
- Build/flash: wrapper `-App phase06_end_to_end build`, then `-p COM5 flash monitor`.
- Steps: run host tests; register samples; start PC client with `--wait-only`, press external button 10 times; restart normal `run` to send PC trigger 10 times. Include known/unknown/no-face and three-face scenes, mixed OK/UNKNOWN/FAILED results, stale/wrong results, a middle-face recognition timeout, disconnect, and another trigger during the batch. Match the shared request ID, unique image IDs, zero-based face metadata, descending raw areas, and PC/ESP summaries. For no-face, verify one initial capture plus at most two recaptures, one optional rotated detection of the final frame, BUSY throughout, and exactly one terminal `NO_FACE`.
- Data: ESP serial logs, PC logs/JSONL, recognition results, BLE CSV, DB metadata (do not copy sensitive embeddings into public reports).
- Expected: each valid face follows `CROPPING → ENCODING → TRANSMITTING → WAITING_RESULT`; a matching result advances to the next face or completes the batch. ESP performs detection only, PC performs recognition, FAILED continues, timeout/disconnect aborts remaining faces, and IDLE resumes after completion or recovery.
- Pass: both directions complete repeatedly, every JPEG decodes, face order/count and summaries match, expected known/UNKNOWN decisions use the configured threshold, stale results are not accepted, BUSY lasts for the full batch, and no buffer/queue leak occurs.
- Fail: ESP performs identity recognition, PC skips landmark re-detection/alignment, name used as unique ID, ID mismatch accepted, concurrent captures, or failure does not recover.
- Recovery: stop client, disconnect, power-cycle, reconnect/resubscribe, use fresh request ID; handle DB/model mismatch by a new compatible DB; record failures.
- Results: `results/phase06/actual/`.
