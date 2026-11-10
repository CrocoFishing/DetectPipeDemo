# Recurring Event recognition and SD deletion verification

Date: 2026-11-11 (Asia/Taipei). Source: working tree based on `dba9efd`, including the existing MODE_0/1000 ms non-speech setting and five-minute audio cap. This is host/incremental-build evidence, not a board or iPhone acceptance result. Earlier integration evidence remains in [EVENT1_VERIFICATION.md](EVENT1_VERIFICATION.md).

## Results

| Check | Result |
|---|---|
| Repository venv: `pytest pc_client/tests tests/event1 -q` | **PASS — 91 tests**. Includes actual C++/Python wire comparison, multi-request notification/result routing, distinct receiver NO_FACE, preserved earlier successes/errors, legacy SQLite migration, pre-OPEN audio shell ownership, duplicate/stale IDs, SD reconstruction and fixtures. |
| `tools/test_event_ledger.ps1` | **PASS**. Production C++ ledger, 200 Events, completion orders, 30-second slots and skipped ticks, three-retry bound, no admission at five minutes/end, prior-error retention and bounded capacity. |
| `tools/test_sd_export_core.ps1` | **PASS**. Production path/range/receipt/deletion policies; invalid path/If-Match, missing files/directories and I/O failure do not authorize another unlink. |
| `tests/event1/run_audio_tests.ps1` | **PASS**. WAV/pre-roll/FIFO checks, recorder lifecycle (5), AFE lifecycle (15), pipeline lifecycle/duration (15), capture lifecycle (11), and export coordination. |
| `ruff check pc_client tools/inspect_event_sd.py tests/event1/test_event_multibatch_contract.py` | **PASS**. Changed Python files also pass format checks. |
| `mypy pc_client/src --no-incremental` | **PASS**, nine source files. |
| Integrated ESP-IDF 6.0 / ESP32-S3 incremental Ninja compile/link/image/partition check | **PASS**. `EventPipeDemo.bin`: 4,136,288 bytes (`0x3f1d60`); `0x600000` app partition has 34% free. |
| Swift/iOS package compile and XCTest | **NOT RUN** — Swift/Xcode/iOS SDK unavailable on this Windows host. New wire-helper tests were added and source reviewed. |
| Flash/physical SD DELETE, recurring camera/phone model tests | **NOT RUN**. No device files were deleted by these development checks. |

The sandbox's Windows asyncio/toolchain restrictions required running Python async tests and the ESP32 compiler in the normal execution environment. The first full test run exposed an inspector error-message compatibility issue; the final 91-test run includes its correction. No hardware PASS is inferred from compiler or simulated results.

## Behavior under test

- One Event/WAV retains initial/every-30-second face requests and bounded full-batch retries; no catch-up backlog. Each request has unique ownership and its own terminal, with indices restarting per request.
- Receiver NO_FACE is wire status 2 / SD CLIENT_NO_FACE. UNKNOWN and generic FAILED do not request recapture. A failed earlier batch still produces overall COMPLETE_WITH_ERROR if a later batch succeeds.
- SQLite migrates the old Event-wide face-index constraint to `(event_id, request_id, face_index)` and backfills request history. It never discards earlier recognized faces.
- Explicit authenticated DELETE validates one regular file and its current-session ETag. Any attempted unlink invalidates the manifest, including uncertain I/O failure. Export completion alone never deletes files.
- Swift `deleteFiles(paths:)` rejects unsupported devices/empty or duplicate paths, obtains fresh listing/ETags, reports acknowledged partial outcomes and closes a management-only session via CANCEL. The phone App's actual model callback must send `RecognitionResult.noFace(...)` for zero detections; that model/App is not included in this repository.

## Remaining acceptance

Run E1-15–E1-20 in [the Event hardware matrix](../tests/event1/HARDWARE_TEST_PLAN.md) and deletion scenarios in [the SD export hardware matrix](../tests/event1/SD_EXPORT_HARDWARE_TEST_PLAN.md). Verify real 30-second timing and skipped slots under long recognition, continuous RAW audio under camera load, three-recapture limits, no new Event batch after end/export gating, physical unlink and rejection cases, refreshed manifests, CSV recreation, Wi-Fi/CANCEL audio restoration, and iOS foreground/cancellation/partial-response behavior. Earlier board results do not validate these new features.
