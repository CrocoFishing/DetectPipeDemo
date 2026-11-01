# EventPipeDemo — Event 1 整合計畫

## 1. 目標

整合以下兩個既有系統：

- `AudioVal`
  - Always-on PDM microphone
  - Audio capture
  - DSP / ESP-SR
  - VAD
  - Ring buffer / pre-roll
  - VAD-triggered event WAV
  - SD writer
  - audio metrics

- `DetectPipeDemo`
  - 使用 `codex/xga-multiface-pipeline` branch
  - XGA 1024×768 RGB565 capture
  - `espdet_pico_224_224_face_s8_s3`
  - bounded recapture
  - final-frame 180° fallback
  - multi-face detection
  - 每張臉依面積由大到小處理
  - 每張臉獨立 crop / JPEG / BLE / recognition
  - `request_id + image_id + face_index + face_count`
  - PC InsightFace recognition

目前 DetectPipeDemo 的 multiface branch 已經會在**同一個 request 下處理所有偵測到的人臉**，每張臉取得自己的 `image_id`，並以 `face_index/face_count` 表示 batch，不需要重新實作 multi-face pipeline。

Event 1 最終要形成：

```text
                    Event ID
                       │
             ┌─────────┴─────────┐
             │                   │
           Audio              Face Batch
             │                   │
         event.wav          request_id
                                 │
                   ┌─────────────┼─────────────┐
                   │             │             │
                 Face 0        Face 1        Face N
                   │             │             │
                image_id      image_id      image_id
                   │             │             │
              recognition   recognition   recognition
```

核心驗收目標：

> 對每次 VAD speech event 建立唯一 Event ID，保存該 Event 的 audio，並將同一時間觸發的整個 multi-face recognition batch 關聯至同一 Event ID。

---

# 2. Event 1 範圍

Event 1 包含：

```text
Always-on microphone
        ↓
       VAD
        ↓
    EVENT_OPEN
        ↓
      event_id
      /      \
     /        \
 audio         face capture
   │               │
pre-roll        detect
   │               │
WAV            N faces
   │               │
SD          BLE → InsightFace
     \          /
      \        /
      EVENT_COMPLETE
```

Event 1 必須完成：

1. Event ID allocation。
2. VAD → Event lifecycle。
3. VAD event audio recording。
4. Event → DetectPipe capture request association。
5. 一個 Event 關聯整個 multi-face batch。
6. 每個 face recognition result 能回溯到 Event。
7. Audio 與 camera pipeline 同時工作時不得破壞 audio continuity。
8. BLE/disconnect/no-face/recognition failure 不得讓 Event ownership 遺失。
9. ESP32 與 PC client 都能重建完整 association。

Event 1 **不包含**：

```text
audio BLE upload
STT
Apple Foundation Models
LLM
conversation aggregation
speaker diarization
event summarization
event notification
idle upload
跨多次 VAD 的 conversation merge
```

這些建立在 Event 1 完成之後。

---

# 3. Repository 策略

不要修改 AudioVal 與原 DetectPipeDemo 作為 validation baseline。

建立新的整合 branch / repository。

建議：

```text
Base:
DetectPipeDemo
└── codex/xga-multiface-pipeline

New branch:
codex/event1-audio-face-association
```

以 DetectPipeDemo multiface branch 為主體，再匯入 AudioVal reusable components。

原因是 DetectPipeDemo 已經具備：

```text
firmware/components
firmware/apps
protocol
pc_client
clients/ios
tests
results
```

比較適合作為完整系統骨架。

AudioVal 則作為 audio subsystem source。

AudioVal 目前使用 16 kHz、mono、PCM16 audio，並已經將 audio capture、DSP、VAD、ring、SD writer 分離；event WAV 使用 application ring buffer 保存 pre-roll，SD I/O 不直接發生在 capture path。

---

# 4. Event 定義

Event 1 中採用：

> **一次 VAD utterance = 一個 Event**

例如：

```text
silence

VAD_START
│
├── Event #104 OPEN
├── snapshot pre-roll
└── request face capture

speech...

VAD_END
│
└── finalize audio

face recognition finished
│
└── face batch terminal

audio terminal && face terminal
│
└── Event #104 COMPLETE
```

暫時不處理：

```text
VAD Event 104
VAD Event 105
VAD Event 106
        ↓
同一段 conversation
```

conversation/event aggregation 留給後續階段。

---

# 5. Event ID

不要使用：

```text
event_id = request_id
```

兩者語意不同。

保留：

```text
event_id
request_id
image_id
face_index
face_count
```

關係為：

```text
event_id
   │
   └── request_id
          │
          ├── image_id A
          │     face_index = 0
          │
          ├── image_id B
          │     face_index = 1
          │
          └── image_id C
                face_index = 2
```

## Event ID 格式

ESP32 內部使用：

```cpp
uint64_t event_id;
```

組成：

```text
upper 32 bit = boot_session_id
lower 32 bit = event_sequence
```

即：

```cpp
event_id =
    ((uint64_t)boot_session_id << 32)
    | event_sequence;
```

例如：

```text
boot_session = 37
event = 42

00000025-0000002A
```

`boot_session_id`：

- boot 時從 NVS 取得
- +1
- 寫回 NVS

`event_sequence`：

- boot 後由 1 開始
- 每次 Event +1
- 不需每個 Event 寫 NVS

如此可以避免 reboot 後 Event ID collision。

Client / SQLite 建議將 Event ID 表示為固定 16 hex-digit string：

```text
000000250000002A
```

而不是依賴 SQLite signed INTEGER。

---

# 6. EventContext

ESP32 只保留 bounded metadata，不保存完整 face array。

概念結構：

```cpp
enum class EventState {
    OPEN,
    WAITING,
    COMPLETE
};

enum class AudioStatus {
    PENDING,
    READY,
    FAILED
};

enum class FaceBatchStatus {
    PENDING,
    COMPLETED,
    NO_FACE,
    PARTIAL,
    BUSY,
    TIMEOUT,
    DISCONNECTED,
    FAILED
};

struct EventContext {
    uint64_t event_id;

    uint64_t vad_start_sample;
    uint64_t vad_end_sample;

    int64_t vad_start_timestamp_us;
    int64_t vad_end_timestamp_us;

    uint64_t audio_first_sample;
    uint64_t audio_last_sample;

    uint32_t request_id;

    uint16_t expected_face_count;
    uint16_t completed_face_count;

    AudioStatus audio_status;
    FaceBatchStatus face_status;

    EventState state;
};
```

ESP32 不需要：

```cpp
std::vector<FaceResult>
```

完整 face association 由：

```text
structured log
SD metadata
PC EventStore
```

保存。

---

# 7. Audio association

AudioVal 現有 pipeline：

```text
PDM
 ↓
I2S
 ↓
capture
 ↓
raw metrics / ring
 ↓
DSP / ESP-SR
 ↓
VAD
```

保留原本架構。AudioVal 的 `audio_frame_t` 已包含 monotonic sample index 與 timestamp，而 VAD-triggered event recording 使用 application pre-roll ring。

Event audio 必須紀錄：

```text
event_id
audio_first_sample
vad_start_sample
vad_end_sample
audio_last_sample
audio_path
audio_source
sample_rate
channels
bits_per_sample
```

重要：

```text
audio_first_sample
<
vad_start_sample
```

在有 pre-roll 時應成立。

例如：

```text
audio_first_sample = 1,280,000
vad_start_sample   = 1,296,000
audio_last_sample  = 1,360,000
```

---

# 8. Audio source

Event 1 先保留 AudioVal 原本 semantics：

```text
event WAV = raw capture + application pre-roll
```

不要在整合時偷偷改為 AGC / NS output。

Audio metadata 加入：

```text
audio_source = RAW
```

之後可以擴充：

```text
RAW
PROCESSED
RAW_AND_PROCESSED
```

等 Audio 2 / STT 結果確定後再決定正式產品使用哪個版本。

AudioVal 目前明確區分 event raw audio 與 selectable DSP output，因此整合階段應保留這個 distinction。

---

# 9. Multi-face association

直接使用目前 multiface pipeline：

```text
Capture
 ↓
Detect
 ↓
recapture if needed
 ↓
optional final 180° fallback
 ↓
N faces
 ↓
stable sort by area
 ↓
face 0
 crop
 JPEG
 BLE
 recognition
 ↓
face 1
 crop
 JPEG
 BLE
 recognition
 ↓
...
 ↓
face batch complete
```

目前 branch 預設一次 initial capture，加最多兩次 recapture；若仍然沒有臉，最後一張 frame 可進行一次 in-place 180° detection。偵測成功後會停止 fallback，並依面積由大至小逐張處理所有有效人臉。

這段邏輯 **Event 1 不重新實作**。

唯一需要增加的是：

```text
CaptureRequest
+
event_id
```

概念：

```cpp
struct CaptureRequest {
    uint32_t request_id;

    bool has_event;
    uint64_t event_id;

    CaptureSource source;
};
```

例如：

```text
source = VAD_EVENT
event_id = 104
request_id = 81
```

後面的 camera pipeline 不需要知道 audio 如何工作。

---

# 10. 一個 Event 對應一個 Face Batch

假設一次 capture 偵測到三張臉：

```text
Event 104
│
└── CaptureRequest 81
      │
      ├── face_index 0
      │    image_id 220
      │    P001 / Alice
      │
      ├── face_index 1
      │    image_id 221
      │    UNKNOWN
      │
      └── face_index 2
           image_id 222
           P017 / Bob
```

最終 Event：

```json
{
  "event_id": "0000002500000068",
  "face_batch": {
    "request_id": 81,
    "status": "COMPLETED",
    "face_count": 3,
    "faces": [
      {
        "face_index": 0,
        "image_id": 220,
        "person_id": "P001",
        "status": "RECOGNIZED"
      },
      {
        "face_index": 1,
        "image_id": 221,
        "status": "UNKNOWN"
      },
      {
        "face_index": 2,
        "image_id": 222,
        "person_id": "P017",
        "status": "RECOGNIZED"
      }
    ]
  }
}
```

這就是之後：

```text
Event.interlocutors[]
```

的基礎。

---

# 11. Face Batch terminal state

Face 不是單一 `face_done` boolean，而是整個 batch。

定義：

```text
PENDING
COMPLETED
NO_FACE
PARTIAL
BUSY
TIMEOUT
DISCONNECTED
FAILED
```

例如：

### 正常

```text
3 faces detected
3 recognition returned

face_status = COMPLETED
```

### 沒有人臉

所有：

```text
initial
recapture
recapture
rotation fallback
```

均失敗：

```text
face_status = NO_FACE
```

### Partial

例如：

```text
Face 0 recognized
Face 1 recognized
Face 2 BLE disconnect
```

Event 不應丟棄前兩個結果：

```text
completed_face_count = 2
expected_face_count = 3

face_status = PARTIAL
```

目前 DetectPipeDemo 在 transfer/result failure 時會進 recovery 並中止剩餘 face，因此整合層必須能表達 partial batch，而不是把整個 Event 當作不存在。

---

# 12. Event lifecycle

## EVENT_OPEN

由：

```text
VAD_START
```

觸發。

EventManager：

```text
allocate event_id

record:
vad_start_sample
vad_start_timestamp

start audio event

submit face CaptureRequest
```

重要：

```text
audio recording
```

與：

```text
face pipeline
```

是並行 branch。

不能：

```text
VAD
 ↓
camera
 ↓
camera finished
 ↓
開始錄音
```

---

# 13. Audio terminal

VAD_END：

```text
VAD_END
 ↓
append remaining PCM
 ↓
close WAV
 ↓
finalize header
 ↓
AUDIO_READY
```

成功：

```text
audio_status = READY
```

SD error：

```text
audio_status = FAILED
```

即使 audio failed：

```text
Event 仍然存在。
```

---

# 14. Face terminal

camera pipeline 最後透過 callback / queue 回 EventManager：

```cpp
FaceBatchResult {
    event_id;
    request_id;

    expected_faces;
    completed_faces;

    status;
};
```

EventManager 不需要等待每一個 recognition result。

camera controller 負責原本的：

```text
request/image validation
stale result rejection
face ordering
batch summary
```

EventManager 只接收最終 batch state。

---

# 15. Event completion

Event completion 條件：

```cpp
audio_terminal &&
face_terminal
```

其中 terminal 包含成功或失敗。

因此：

```text
audio READY
face COMPLETED
→ EVENT_COMPLETE
```

也可以：

```text
audio READY
face NO_FACE
→ EVENT_COMPLETE
```

或：

```text
audio READY
face DISCONNECTED
→ EVENT_COMPLETE
```

甚至：

```text
audio FAILED
face COMPLETED
→ EVENT_COMPLETE
```

Event complete 不代表所有 subsystem 成功。

Event 本身另外保存：

```text
COMPLETE
COMPLETE_WITH_ERROR
```

---

# 16. 不讓 VAD 直接控制 Camera

禁止：

```text
VAD callback
 ↓
camera_capture()
```

改成：

```text
Audio/VAD
   │
   │ EventSignal
   ▼
EventManager
   │
   │ CaptureRequest
   ▼
TriggerService / SystemController
   │
   ▼
existing camera pipeline
```

這樣：

```text
audio
camera
BLE
event lifecycle
```

彼此保持解耦。

---

# 17. TriggerService

現有 TriggerService 保留：

```text
GPIO2
BLE START_CAPTURE
duplicate protection
BUSY arbitration
request_id
queue length = 1
```

multiface branch 目前就是所有 trigger 經同一 arbitration，camera request queue 保持 bounded。

新增第三種 internal trigger：

```text
EVENT_VAD
```

因此來源：

```cpp
enum class CaptureSource {
    GPIO_BUTTON,
    BLE_CLIENT,
    EVENT_VAD
};
```

---

# 18. Button / BLE trigger policy

Event 1 正常產品流程：

```text
VAD
→ Event
→ face capture
```

原本：

```text
GPIO button
PC START_CAPTURE
```

先保留作為：

```text
diagnostic/manual capture
```

但預設：

```text
不建立 Audio Event
```

如此不需要解決：

> 手動按下按鈕之後，audio event 應該什麼時候結束？

未來若需要：

```text
MANUAL_EVENT
```

再另外定義。

---

# 19. BUSY policy

這是 Event 1 很重要的情況。

例如：

```text
Event 104
↓
face batch still running

VAD_START
↓
Event 105
```

Capture queue 現在只有一個 request，而且直到整個 request completion/recovery 前維持 BUSY。

Event 105 仍然必須建立：

```text
Event 105
├── audio = READY
└── face = BUSY
```

不能：

```text
camera BUSY
→ Event 105 disappeared
```

Event 1 不新增無限 camera queue。

之後若實際測試發現 BUSY miss 太多，再於 Event 2 評估：

```text
bounded deferred capture
```

---

# 20. BLE protocol

現有 v1 header：

```text
32 bytes

request_id
image_id
chunk_index
total_chunks
CRC
reserved
```

而 multiface 已經使用：

```text
reserved[15:0]  = face_index
reserved[31:16] = face_count
```

所以**不能再把 Event ID 塞進 reserved**。

Event 1 建議：

```text
Protocol v2
```

但：

> 不更動既有 32-byte image header layout。

這可以最大程度保留現在已完成的 multi-face protocol。

---

# 21. Protocol v2 association 方法

新增 Event message。

建議 message type：

```text
15 EVENT_OPEN
16 EVENT_AUDIO_READY
17 FACE_BATCH_END
18 EVENT_COMPLETE
```

## EVENT_OPEN

Header：

```text
request_id = associated face request
image_id = 0
```

Payload：

```cpp
uint64_t event_id;
uint8_t trigger_type;
uint8_t reserved[7];
```

Client 建立：

```text
request_id → event_id
```

之後既有：

```text
IMAGE_BEGIN
IMAGE_CHUNK
IMAGE_END
RECOGNITION_RESULT
```

完全依靠：

```text
request_id
image_id
face_index
face_count
```

即可回溯到 Event。

因此不用在每一個 BLE chunk 重複傳 8 bytes Event ID。

---

# 22. EVENT_AUDIO_READY

Payload 至少包含：

```text
event_id
audio_status
audio_first_sample
audio_last_sample
```

Event 1 **不傳 audio bytes**。

此 message 主要用於：

```text
PC EventStore
debug
association validation
```

---

# 23. FACE_BATCH_END

Payload：

```text
event_id
request_id

expected_face_count
completed_face_count

recognized_count
unknown_count
failed_count

face_batch_status
```

如此 PC 不必用 timeout 猜：

> 到底還有沒有下一張臉？

---

# 24. EVENT_COMPLETE

Payload：

```text
event_id

event_status
audio_status
face_status
```

Client 收到後可以 finalize Event。

---

# 25. PC EventStore

目前 PC client 已經處理：

```text
BLE
image assembly
face sequence
InsightFace
recognition result
SQLite identity database
```

這些全部保留。multiface branch 的 client 已會檢查 face sequence metadata 與 batch ordering。

新增：

```text
EventStore
```

資料模型：

```text
events
event_faces
```

### events

```text
event_id
request_id
opened_at
completed_at
audio_status
audio_file
face_status
face_count
event_status
```

### event_faces

```text
event_id
request_id
image_id

face_index
face_count

recognition_status
person_id
person_name
similarity
processing_time_ms
```

Primary association：

```text
event_id + image_id
```

---

# 26. ESP32 SD storage

不建立：

```text
/events/<event_id>/
```

避免大量 filesystem operation。

保留 flat layout：

```text
/sd/
├── event_0000002500000068.wav
├── event_0000002500000069.wav
├── event_000000250000006A.wav
│
├── audio_events.csv
├── face_associations.csv
└── event_lifecycle.csv
```

---

# 27. audio_events.csv

一個 Event 一筆：

```text
event_id
audio_file

audio_source
sample_rate

audio_first_sample
vad_start_sample
vad_end_sample
audio_last_sample

vad_start_timestamp_us
vad_end_timestamp_us

audio_status
```

---

# 28. face_associations.csv

一張 face 一筆：

```text
event_id
request_id
image_id

face_index
face_count

status
person_id
similarity
processing_time_ms
```

例如：

```text
104,81,220,0,3,RECOGNIZED,P001,0.71,52
104,81,221,1,3,UNKNOWN,,0.31,48
104,81,222,2,3,RECOGNIZED,P017,0.66,54
```

NO_FACE：

```text
105,82,0,-1,0,NO_FACE,,,0
```

---

# 29. event_lifecycle.csv

用 append-only event log：

```text
event_id,state,timestamp_us,status
```

例如：

```text
104,OPEN,820000000,OK
104,AUDIO_READY,824000000,OK
104,FACE_BATCH_END,825300000,COMPLETED
104,COMPLETE,825301000,OK
```

不要讓 EventManager 直接頻繁 `fwrite()`。

所有 SD metadata 仍經：

```text
sd_writer / metadata writer
```

避免 filesystem latency 進入 audio/camera realtime path。

---

# 30. Audio 優先權

整合後最重要的 realtime requirement 是：

> Camera workload 不得破壞 always-on audio。

AudioVal 的設計本來就要求 capture 不可 block、DSP 必須 realtime 跟上、SD stall 必須被 buffer，並持續追蹤 dropped samples、queue high-water、SD latency 等指標。

優先順序：

```text
Audio capture continuity
        >
Audio DSP / VAD realtime
        >
Camera latency
        >
Face recognition latency
```

Camera 慢可以接受。

Audio drop 不可以。

---

# 31. Resource instrumentation

整合後額外紀錄：

```text
internal heap free/min
PSRAM free/min

audio queue high-water
audio dropped_samples
audio overrun

SD writer queue high-water
SD max write latency

camera capture latency
face detection latency
JPEG latency

BLE transfer latency
recognition latency

EventManager queue high-water
open Event count
```

特別觀察：

```text
XGA RGB565 frame
+
crop buffer
+
160 KiB JPEG
+
ESP-DL
+
ESP-SR AFE
+
audio ring
+
SD buffers
+
NimBLE
```

同時存在時 PSRAM/heap 是否穩定。

---

# 32. Integration configuration

Event 1 不需要把 AudioVal 全部功能搬進來。

匯入：

```text
audio capture
audio frame representation
DSP / ESP-SR wrapper
VAD
ring buffer
event recorder
WAV writer
SD writer
audio metrics
audio configuration
```

暫時不需要：

```text
USB UAC
AudioSTTBench
Audio 6 USB route
continuous recording enabled by default
```

可以保留程式碼，但：

```text
Event 1 build default disabled
```

降低整合變數。

---

# 33. Board configuration 統一

目前兩個 repo 都有自己的 board/config definition。

整合後只能有一個：

```text
board_config
```

至少統一：

```text
camera pins
PDM DATA GPIO41
PDM CLK GPIO42

SD SPI pins
SD CS

external trigger GPIO2

camera resolution
camera pixel format

PSRAM
BLE
ESP-SR model
```

AudioVal 目前文件中的 PDM 為 GPIO41/GPIO42，microSD 使用 SPI GPIO7/8/9，現行 profile 的 CS 為 GPIO3；DetectPipeDemo 則保留 GPIO2 作 external trigger。

開機時必須印出完整：

```text
CONFIG,...
```

供測試留存。

---

# 34. Phase 0 — Freeze Baselines

## 目的

在真正整合前確認兩個來源 branch 的狀態。

### Detect baseline

checkout：

```text
codex/xga-multiface-pipeline
```

執行：

```text
host pytest
ruff
mypy
protocol tests
all firmware build
```

另外重新執行至少必要的：

```text
camera
multi-face
no-face recapture
rotation fallback
end-to-end recognition
```

因為目前 branch 文件明確指出：

```text
host suite passed
firmware builds passed
fresh hardware validation still required
```


### Audio baseline

至少確認：

```text
audio capture
VAD
ring
event WAV
SD
```

各自能運作。

### Phase 0 驗收

```text
Detect baseline PASS
Audio baseline PASS
commit hashes recorded
hardware revision recorded
sdkconfig recorded
```

---

# 35. Phase 1 — Combined Boot / Resource Integration

先不建立 Event。

只把兩套 subsystem 放到同一 firmware：

```text
Camera init
Face detector init
BLE init

PDM init
ESP-SR init
VAD init
Ring init
SD init
```

執行：

```text
60 s always-on audio
BLE connected
camera initialized
```

再手動執行原本 face capture。

### 驗證

```text
audio dropped_samples = 0
audio overrun = 0

camera capture OK
multiface OK
BLE OK

SD OK
VAD still running

heap stable
PSRAM stable
```

### 最重要測試

在：

```text
capture
detect
JPEG
BLE
InsightFace
```

執行期間持續觀察：

```text
captured_samples
```

必須連續增加。

---

# 36. Phase 2 — EventManager

暫時不接 Camera。

流程：

```text
VAD_START
 ↓
EventManager
 ↓
allocate Event ID
 ↓
EVENT_OPEN

VAD_END
 ↓
audio terminal mock
 ↓
face terminal mock
 ↓
EVENT_COMPLETE
```

測試：

```text
100 個 VAD events
```

驗證：

```text
duplicate Event ID = 0
missing Event = 0
OPEN/COMPLETE pairing = 100%
reboot 後 ID 不 collision
```

---

# 37. Phase 3 — Event Audio

加入：

```text
EventManager
 ↕
ring/event recorder
```

流程：

```text
VAD_START
 ↓
Event ID
 ↓
snapshot ring
 ↓
event_<event_id>.wav
 ↓
VAD_END
 ↓
finalize WAV
```

測試：

```text
single utterance
back-to-back utterances
short utterance
long utterance
pre-roll wrap
SD slow/error simulation
```

驗證：

```text
Event ID ↔ WAV = 1:1

audio_first_sample
<
vad_start_sample
<
audio_last_sample

WAV valid

sample continuity valid

dropped_samples = 0
```

---

# 38. Phase 4 — Event + Existing Multi-Face Pipeline

這個 Phase 才將：

```text
EventManager
 ↓
CaptureRequest(event_id)
```

接到現有 TriggerService。

不要修改核心：

```text
capture
detection
recapture
rotation fallback
sort
crop
JPEG
BLE
recognition
```

只傳遞 Event context。

測試：

### 1 face

```text
Event
└── Face 0
```

### 2 faces

```text
Event
├── Face 0
└── Face 1
```

### 3+ faces

確認：

```text
same event_id
same request_id
different image_id
correct face_index
correct face_count
```

### NO_FACE

確認完整：

```text
initial
recapture 1
recapture 2
rotation fallback
```

後：

```text
face_status = NO_FACE
```

---

# 39. Phase 5 — Protocol v2 + PC EventStore

更新：

```text
protocol/ble_protocol.yaml

firmware protocol
Python client protocol
iOS BLEProtocol.swift
protocol tests
```

但：

```text
現有 IMAGE_BEGIN
IMAGE_CHUNK
IMAGE_END
RECOGNITION_RESULT
FACE_SEQUENCE
```

行為保持不變。

增加：

```text
EVENT_OPEN
EVENT_AUDIO_READY
FACE_BATCH_END
EVENT_COMPLETE
```

PC 建立：

```text
request_id → event_id
```

再形成：

```text
event_id
 └── faces[]
```

### 驗證

client log 必須能顯示：

```text
EVENT 104
audio READY

face 0 → Alice
face 1 → UNKNOWN
face 2 → Bob

COMPLETE
```

---

# 40. Phase 6 — Full End-to-End Integration

完整流程：

```text
Always-on Mic
      ↓
     VAD
      ↓
 EventManager
      │
 ┌────┴─────┐
 │          │
Audio      Multi-Face
 │          │
Ring       Camera
 │          │
WAV        Detect
 │          │
SD         Recapture/Fallback
            │
           N faces
            │
           BLE
            │
        PC InsightFace
            │
        Face results
 │          │
 └────┬─────┘
      ↓
EVENT_COMPLETE
```

---

# 41. Phase 6 Test Matrix

| ID | Audio | Face condition | Network | Expected |
|---|---|---|---|---|
| E1-01 | speech | 1 known | normal | audio + known |
| E1-02 | speech | 1 unknown | normal | audio + UNKNOWN |
| E1-03 | speech | 2 known | normal | 2 face associations |
| E1-04 | speech | known + unknown | normal | mixed result |
| E1-05 | speech | 3+ faces | normal | all sequentially associated |
| E1-06 | speech | no face | normal | audio + NO_FACE |
| E1-07 | speech | fallback detects face | normal | same Event association |
| E1-08 | speech | faces | disconnect | audio + PARTIAL/DISCONNECTED |
| E1-09 | speech | faces | recognition timeout | audio + TIMEOUT/PARTIAL |
| E1-10 | two rapid VAD | camera busy | normal | second Event audio retained + BUSY |
| E1-11 | speech | 3+ faces | normal | audio has zero drops during full batch |
| E1-12 | repeated events | mixed | normal | no association crossover |
| E1-13 | speech | faces | SD stress | camera works, audio reports error/drop explicitly |
| E1-14 | reboot | — | — | Event ID does not collide |

---

# 42. Association invariants

系統必須永遠符合：

### Invariant 1

```text
event_id identifies exactly one Event.
```

### Invariant 2

```text
one Event has at most one Event-1 face CaptureRequest.
```

### Invariant 3

```text
all faces under the CaptureRequest
belong to the same Event.
```

### Invariant 4

```text
every image_id belongs to exactly one request_id.
```

### Invariant 5

```text
every recognition result must match:
request_id + image_id
```

### Invariant 6

```text
face_index ∈ [0, face_count)
```

### Invariant 7

```text
audio failure cannot delete face data.
```

### Invariant 8

```text
face failure cannot delete audio.
```

### Invariant 9

```text
camera workload cannot silently drop audio.
```

---

# 43. Event 1 Definition of Done

Event 1 只有滿足以下全部條件才算完成：

```text
[ ] multiface branch 直接整合完成

[ ] XGA camera pipeline 保留

[ ] bounded recapture 保留

[ ] 180° fallback 保留

[ ] all-face processing 保留

[ ] FACE_SEQUENCE protocol 保留

[ ] 每個 VAD speech 產生唯一 Event ID

[ ] reboot 後 Event ID 不 collision

[ ] 每個 Event 產生 audio artifact

[ ] audio artifact 保存 pre-roll

[ ] Event 可關聯一個 capture request

[ ] capture request 可關聯 N 張臉

[ ] 每張臉有獨立 image_id

[ ] face_index / face_count 正確

[ ] recognition result 不會掛到錯 Event

[ ] UNKNOWN 仍然是有效 face association

[ ] NO_FACE 仍然是有效 face batch result

[ ] partial transfer/result failure 不會刪除已完成 face

[ ] camera BUSY 不會刪除 Audio Event

[ ] BLE disconnect 不影響 local audio recording

[ ] SD error 不影響 face pipeline ownership

[ ] audio dropped_samples = 0
    during normal camera/detection/JPEG/BLE operation

[ ] audio overrun = 0

[ ] Event 可以在 audio 先完成時正常結束

[ ] Event 可以在 face batch 先完成時正常結束

[ ] PC 可以完整重建 Event → faces

[ ] SD metadata 可以完整重建 Event → audio

[ ] PC/SD 使用相同 Event ID
```

---

# 44. Event 1 最終資料模型

完成後概念上應能重建：

```json
{
  "event_id": "0000002500000068",

  "trigger": {
    "type": "VAD",
    "sample_index": 13192160,
    "timestamp_us": 824510000
  },

  "audio": {
    "status": "READY",
    "file": "event_0000002500000068.wav",
    "source": "RAW",
    "sample_rate": 16000,
    "first_sample": 13176160,
    "vad_start_sample": 13192160,
    "vad_end_sample": 13272160,
    "last_sample": 13288960
  },

  "face_batch": {
    "request_id": 51,
    "status": "COMPLETED",
    "face_count": 3,

    "faces": [
      {
        "face_index": 0,
        "image_id": 18,
        "recognition": {
          "status": "RECOGNIZED",
          "person_id": "P003",
          "similarity": 0.62
        }
      },

      {
        "face_index": 1,
        "image_id": 19,
        "recognition": {
          "status": "UNKNOWN",
          "similarity": 0.34
        }
      },

      {
        "face_index": 2,
        "image_id": 20,
        "recognition": {
          "status": "RECOGNIZED",
          "person_id": "P009",
          "similarity": 0.69
        }
      }
    ]
  },

  "status": "COMPLETE"
}
```

---

# 45. 後續 Event 2+

Event 1 做完後才進入：

```text
Event 2
NO_VISIBLE_FACE retry strategy
capture scheduling
BUSY/deferred capture policy

Event 3
audio upload
idle transfer

Event 4
STT

Event 5
interlocutor aggregation
duplicate identity merge

Event 6
AFM / LLM event understanding

Event 7
multiple VAD events
→ conversation/session aggregation
```

其中 multi-face 已經在 Event 1 完成，因此之後不需要重新修改基本 face ownership model。

最終架構自然變成：

```text
Social Interaction
│
├── Event 104
│    ├── audio
│    └── interlocutors[]
│
├── Event 105
│    ├── audio
│    └── interlocutors[]
│
└── Event 106
     ├── audio
     └── interlocutors[]
```

再交由 STT / AFM / LLM 做上層事件整理。

---

# 46. 實作原則總結

這次整合最重要的不是：

> 把 AudioVal 的程式貼進 DetectPipeDemo。

而是建立一個新的 ownership layer：

```text
                   EventManager
                        │
          ┌─────────────┴─────────────┐
          │                           │
     Audio ownership             Face ownership
          │                           │
       WAV file                    request
                                      │
                                   N images
                                      │
                                N identities
```

AudioVal 與 DetectPipeDemo 都維持各自已驗證的 subsystem responsibility。

新的 EventManager 只負責：

```text
identity
association
lifecycle
terminal state
```

而不負責：

```text
DSP
VAD algorithm
camera
face detection
JPEG
BLE image transfer
InsightFace
filesystem write
```

如此 Event 1 完成後，後續的 STT、interlocutor、event center、AFM 與 idle upload 都能建立在穩定的：

Event → Audio → Face Batch → Faces → Identity

資料鏈上，而不需要再次重構底層 pipeline。