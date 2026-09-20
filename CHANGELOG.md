# Change Log

## Unreleased

### Face pipeline and camera

- Changed camera capture from the earlier VGA/QVGA path to 1024×768 XGA RGB565 and increased face expansion to 20% per side (40% total).
- Added a configurable bounded no-face fallback: one initial capture, two recaptures by default, then one in-place 180-degree detection of the final frame.
- Replaced largest-face-only selection with stable, area-descending processing of every valid detected face.
- Reused the same crop/JPEG buffers while assigning a unique image ID to each face under one request, and added per-batch OK/UNKNOWN/FAILED summaries.

### Protocol and clients

- Added the optional v1 `FACE_SEQUENCE` flag. `reserved[15:0]` carries the zero-based face index and `reserved[31:16]` carries the face count on `IMAGE_BEGIN` and `IMAGE_END`.
- Kept legacy compatibility: an unflagged image is interpreted as face 0 of 1.
- Updated the firmware transport, Python client, and iOS assembler to validate matching face-sequence metadata; the Python client now enforces batch order and recognizes each face sequentially.

### Tests and documentation

- Added Python coverage for face-sequence encoding, invalid/mismatched metadata, face-batch ordering, duplicate image IDs, and result summaries.
- Extended the Phase 3 JPEG extraction utility and the Phase 3/6 plans for multi-face and fallback scenarios.
- Updated architecture, protocol, camera expectations, and decision records for XGA, 40% total margin, retry/rotation fallback, and every-face processing.
- Validation on 2026-09-21: 24 pytest tests passed; Ruff and mypy passed; all six ESP-IDF targets built successfully.
- Hardware validation of the XGA/multi-face/fallback changes is still pending. Existing raw logs predate these changes.

## 2026-09-20 — Local evidence compared with the GitHub baseline

### Comparison baseline

- Repository: <https://github.com/CrocoFishing/DetectPipeDemo>
- Branch: `main`
- Commit: `346a1ac9fb57d0c28c58f3aa1e236d7c76c76c3b` (`first commit`, 2026-08-07)
- Comparison method: refreshed `origin`, then checked `git diff origin/main`, `git status`, and ignored files under `results/`.

At the 2026-09-20 comparison snapshot, all tracked source, configuration, protocol, test-plan, and documentation files matched `origin/main` before the documentation update. There were no local firmware or PC-client code changes at that snapshot; the later changes are recorded under **Unreleased** above.

### Local-only additions found

The working tree contained 27 files that were absent from the GitHub baseline:

| Location | Files | What was added |
|---|---:|---|
| `results/phase01/actual/` | 2 | Camera/PSRAM serial log and a partially filled result template |
| `results/phase02/actual/` | 4 | Initialization, normal press, short/bouncy press, and hold serial logs |
| `results/phase03/actual/` | 7 | Six detection/JPEG logs and a partially filled result template |
| `results/phase04/actual/` | 2 | Board protocol smoke log and a result template containing the 13-test host run |
| `results/phase05/actual/` | 8 | Seven BLE metric CSV files and an unchanged result template |
| `results/phase06/actual/` | 3 | ESP32 and PC end-to-end logs and an unchanged result template |
| `results/TEST_DATA_SUMMARY.md` | 1 | Derived inventory, metrics, evidence assessment, and remaining test gaps |

The 26 files under `results/**/actual/` are intentionally ignored by `.gitignore`; they remain local raw evidence. `results/TEST_DATA_SUMMARY.md` is not ignored and is the reviewable, privacy-reduced record intended for version control.

### Evidence represented by the additions

- All six firmware targets retain successful local build records; the host lint, type, unit, and protocol checks also passed.
- Phase 01–03 add limited camera, trigger/state-machine, face-detection, crop, and JPEG smoke evidence.
- Phase 04 records 13/13 host protocol tests passing and a board smoke result.
- Phase 05 contains 173 successful BLE transfers across seven CSV files, totaling 9,200,000 bytes and 19,162 chunks, with no recorded sequence, CRC, disconnect, retry, or congestion failures.
- Phase 06 records four accepted flows: two known-person results, one `UNKNOWN`, and one `NO_FACE`; all returned to `IDLE`.

These additions are partial smoke/measurement evidence, not a completed hardware acceptance run. Several required repetitions, environment labels, negative/recovery cases, and signed PASS/FAIL fields are still missing. See [`results/TEST_DATA_SUMMARY.md`](results/TEST_DATA_SUMMARY.md) for the detailed evidence and gap analysis.

### Documentation updated in this comparison

- `README.md`: corrected the verification status so it distinguishes the original no-board baseline from later local hardware evidence.
- `FILE_MANIFEST.md`: listed this changelog, the summary, and the ignored raw-evidence policy.
- `results/TEST_DATA_SUMMARY.md`: added an explicit GitHub-baseline comparison and provenance.
