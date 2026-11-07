# Event 1 hardware acceptance — NOT RUN

No serial ports were detected on 2026-10-04. All rows below remain NOT RUN; host fixtures and compilation do not count as hardware evidence.

## Freeze and setup

Record board/camera/expansion revision, SD card model/filesystem, PDM wiring, PC BLE adapter, firmware SHA or working-tree snapshot, both baseline commits, SDK configuration hashes and client versions. Default board is XIAO ESP32-S3 Sense / OV3660, PDM41/42, SPI7/8/9/CS3, diagnostic trigger2. Confirm actual board revision; configure SD profile if necessary. Preserve NVS boot history when flashing. Use Event app flash arguments including VAD model at0x610000.

Before combined testing, independently validate the frozen AudioVal and DetectPipeDemo baselines: capture/VAD/ring/SD/WAV and camera/multiface/no-face recapture/180 fallback/recognition. Record PASS only from real runs. Then run a 60-second combined boot with BLE connected and camera initialized, followed by manual face captures while microphone remains always-on.

Start `python -m pc_client --event-database event_store.db run --wait-only`. Capture complete serial CONFIG/STATUS/RESOURCE/EVENT/FACE/error output. Copy all three SD CSVs and WAVs after each run. Inspect with `python tools/inspect_event_sd.py <SD-copy> --output events.json`; compare IDs/request/image/index/count with SQLite. A sentinel row `image_id=0,face_index=-1` is a batch terminal, never a detected face.

## Matrix

| ID | Scenario | Required evidence | Actual |
|---|---|---|---|
| E1-01 | speech +1 known face | one RAW pre-roll WAV, one returned face, same Event ID | NOT RUN |
| E1-02 | speech +1 unknown | UNKNOWN remains a valid association | NOT RUN |
| E1-03 | speech +2 known | one request, two different image IDs, indices0/1 | NOT RUN |
| E1-04 | known +unknown | both results retained, correct statuses | NOT RUN |
| E1-05 | 3+faces | every valid face processed in area order; same Event/request | NOT RUN |
| E1-06 | no face | initial +2 recaptures +final180 attempted; NO_FACE +audio | NOT RUN |
| E1-07 | fallback finds face | no extra Event/request allocation | NOT RUN |
| E1-08 | disconnect midbatch | completed faces retained; PARTIAL or DISCONNECTED; local audio continues | NOT RUN |
| E1-09 | recognition timeout | TIMEOUT/PARTIAL terminal, audio owner intact | NOT RUN |
| E1-10 | rapid utterances while camera busy | second WAV retained; second face BUSY/request0; no deferred camera queue | NOT RUN |
| E1-11 | speech +full3+face workload | captured_samples continuously advances; dropped_samples=0; queue_overruns=0 | NOT RUN |
| E1-12 |100 repeated mixed Events | unique IDs, complete pairing, no crossed request/image/face metadata | NOT RUN |
| E1-13 | SD stall/error/full/unmounted | explicit audio/metadata failures; camera results retained; capture stays active | NOT RUN |
| E1-14 | reboot with NVS preserved | boot session increases; Event IDs never collide; PC accepts reset request/image counters | NOT RUN |

Also exercise short/long utterances, ring wrap, back-to-back audio while SD is slow, both branch completion orders, GPIO/BLE manual requests with no audio Event, and MTU below67 with explicit readiness error.

## Pass criteria

Normal combined runs require zero dropped_samples and queue_overruns, no unreported SD metadata drops, zero crossed owners and no missing OPEN/COMPLETE pairs. Compare actual WAV PCM frame count to `audio_last_sample-audio_first_sample`; for available pre-roll require first<vad_start. UNKNOWN/NO_FACE are valid outcomes. COMPLETE_WITH_ERROR must retain useful branch artifacts and finished faces.

Observe internal/PSRAM free/min, audio/SD/Event queue high-water, maximum capture/DSP/SD latency and camera/detection/JPEG/BLE/recognition latency during full workload. A lower camera latency is not a reason to accept audio gaps. Missing evidence stays NOT RUN or INCOMPLETE.

SD/structured-log reconstruction is required when BLE lifecycle notifications were missed. PC disconnect observations preserve data and do not fabricate terminal messages or audio completion.

## Result record

```text
date/time/timezone:
board + expansion/camera revision:
SD card + filesystem:
source baselines:
integration revision/snapshot:
sdkconfig SHA256:
PC/client/model/BLE adapter:
test ID + utterance/face conditions:
event IDs + request/image IDs:
audio drops/overruns/queue high-water:
heap/PSRAM free/min:
SD latency/errors + WAV inspection:
face ordering/result/terminal:
OPEN/COMPLETE pairing + observed status:
raw evidence paths:
actual PASS/FAIL/NOT RUN and reason:
```
