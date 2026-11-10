# Event 1 State Diagram

本文件依目前 `event1_audio_face`、`EventManager`、audio recorder、camera pipeline 與 PC `EventStore` 的實作繪製。**一次 VAD utterance = 一個 Event**；同一個 `event_id` 擁有一條 audio 分支，以及多個包含 N 張臉的 camera request（開始、每 30 秒及 NO_FACE 重拍）。兩條分支非同步執行，終態可以以任意順序到達。

圖中的 `OPEN / WAITING / COMPLETE / COMPLETE_WITH_ERROR` 是實際 `EventState`；audio 的 `PENDING / READY / FAILED` 和 face batch 的狀態則分別保存在 `AudioStatus`、`FaceBatchStatus`。其餘中文節點是操作步驟或判斷，用來展開流程，不是新增的 firmware enum。

## 1. Event 主狀態機

```mermaid
stateDiagram-v2

    direction LR
    [*] --> AwaitVad
    state "等待 VAD_START" as AwaitVad
    state Admission <<choice>>
    state "EVENT_REJECTED" as Rejected
    state FinishCheck <<choice>>
    state "發布 EVENT_COMPLETE / 釋放 Event slot" as Release

    AwaitVad --> Admission: SILENCE → SPEECH / VAD_START
    Admission --> OPEN: slot 與 ID 可用 / 建立 event_id
    Admission --> Rejected: 16 個 slot 已滿或 ID 耗盡
    Rejected --> AwaitVad: 本次不建立 audio / face owner

    OPEN: audio_status = PENDING
    OPEN: face_status = PENDING
    OPEN: 快照 RAW pre-roll / 啟動兩條分支
    OPEN --> OPEN: 收到一個分支終態 / 另一分支仍 PENDING
    OPEN --> WAITING: VAD_END / 保存結束 sample 與 timestamp
    OPEN --> WAITING: 觸發後滿 300000 ms / 保存上限 sample 與 timestamp
    OPEN --> OPEN: 每 30 秒且相機可用 / 新 request / 同一 WAV
    OPEN --> OPEN: client NO_FACE / 最多重拍 3 次 / 保留所有結果

    WAITING: VAD 已結束 / 等待尚未完成的分支
    WAITING --> WAITING: 收到一個分支終態 / 另一分支仍 PENDING
    WAITING --> FinishCheck: audio 與 face 都已 terminal

    FinishCheck --> COMPLETE: READY 且 face 為 COMPLETED 或 NO_FACE 且 failed_count = 0
    FinishCheck --> COMPLETE_WITH_ERROR: 其他 terminal 組合
    COMPLETE --> Release
    COMPLETE_WITH_ERROR --> Release
    Release --> [*]

    note right of OPEN
        event_id = boot_session 左移 32 bit | sequence
        上層限 16 個同時存活的 Event
        camera 只接受一個 active request
        camera BUSY 仍保留新 Event 的 audio
    end note

    note right of WAITING
        VAD_END 不等於 AUDIO_READY
        必須等待 WAV writer 的 terminal callback
        舊 Event 等 face 時，新 utterance 可開新 Event
    end note
```

正常 audio terminal 在 VAD_END 後到達，因此經過 `WAITING`。`refresh_event_state()` 現在要求 `vad_ended`、audio terminal 及最後一輪 face terminal；錄音中的任何單輪結束不會完成 Event。`WAITING` 是 utterance 已結束的意思，不代表已有任一分支完成。

Audio 最長為 VAD 觸發後 5 分鐘，pre-roll 另計。到上限時由 RAW sample clock 觸發既有 end callback 與 recorder END，最後一個 frame 會裁切至上限，不因正常到上限而標記失敗。此時保存的 `vad_end_sample`／timestamp 表示強制結束邊界；log 以 `reason,max_duration` 區分。收尾後等待 VAD 回到非語音，再收到下一次語音轉換才可建立新 Event，不重設 VAD、不立即續開。

啟動前，app 必須成功初始化 NVS boot counter、EventManager 與 audio pipeline 才能接收 VAD Event。NVS 初始化／commit／counter overflow、EventManager task 或 audio init/start 失敗會停止 Event 啟動；camera/BLE init/start 失敗則允許 audio 繼續，每個 Event 的 face 分支直接回報 `FAILED`。

## 2. Audio / Face 的非同步完成匯合

以下節點表示兩個 status 欄位的組合，與前面的 `EventState` 同時存在。

```mermaid
stateDiagram-v2
    direction TB
    [*] --> BothPending
    state "Audio PENDING / Face PENDING" as BothPending
    state "Audio terminal / Face PENDING" as AudioFirst
    state "Audio PENDING / Face terminal" as FaceFirst
    state "Audio terminal / Face terminal" as BothTerminal
    state Outcome <<choice>>

    BothPending --> AudioFirst: EVENT_AUDIO_READY / READY 或 FAILED
    BothPending --> FaceFirst: FACE_BATCH_END / 任一 face 終態
    AudioFirst --> BothTerminal: FACE_BATCH_END
    FaceFirst --> BothPending: 錄音中且新週期／NO_FACE 重拍 / 新 request
    FaceFirst --> BothTerminal: EVENT_AUDIO_READY
    BothTerminal --> Outcome: vad_ended 且 refresh_event_state
    Outcome --> COMPLETE: audio READY 且所有輪次成功且無歷史錯誤
    Outcome --> COMPLETE_WITH_ERROR: 任一分支失敗
    COMPLETE --> [*]
    COMPLETE_WITH_ERROR --> [*]
```

| 欄位 | 非終態 | 終態 | 對 Event 的影響 |
|---|---|---|---|
| Audio | `PENDING` | `READY`、`FAILED` | `FAILED` 仍需等待 face terminal |
| Face batch | `PENDING` | `COMPLETED`、`NO_FACE`、`PARTIAL`、`BUSY`、`TIMEOUT`、`DISCONNECTED`、`FAILED` | 任一終態仍需等待 audio terminal |
| Event | `OPEN`、`WAITING` | `COMPLETE`、`COMPLETE_WITH_ERROR` | 發布完成資訊後才釋放 Event slot |

`UNKNOWN` 是有效且已完成的單臉 recognition result，不會單獨造成 Event error。`NO_FACE` 是 ESP 未偵測到臉的成功 batch 終態；所有輪次皆成功且 audio 為 `READY` 時可得到 `COMPLETE`。手機回覆的單臉 CLIENT_NO_FACE 則計入 failed 並觸發有限重拍。單臉 `FAILED` 也計入 `completed_faces`，但會使 batch／Event 帶有錯誤。

## 3. Audio 分支：RAW pre-roll → WAV → terminal

```mermaid
stateDiagram-v2
    direction TB
    [*] --> Snapshot
    state "PENDING / 快照 RAW ring" as Snapshot
    state BeginCheck <<choice>>
    state "PENDING / writer 錄音中" as Recording
    state "PENDING / 已標記音訊錯誤" as Damaged
    state "PENDING / BEGIN 未入列" as BeginRejected
    state EndCheck <<choice>>
    state "PENDING / writer finalize" as Finalizing
    state ResultCheck <<choice>>

    Snapshot --> BeginCheck: enqueue BEGIN 與 pre-roll
    BeginCheck --> Recording: BEGIN 成功入列 / 建立 WAV
    BeginCheck --> BeginRejected: SD 未運行、配置或 queue admission 失敗
    Recording --> Recording: enqueue live RAW PCM / 寫入與定期 flush
    Recording --> Damaged: sample gap、DSP、submit 或 WAV open/write/flush 錯誤
    Damaged --> Damaged: 保留 owner / 處理後續 RAW

    Recording --> EndCheck: VAD_END / Event 進 WAITING / enqueue END
    Recording --> EndCheck: 觸發後滿 5 分鐘 / 裁切 RAW / enqueue END
    Damaged --> EndCheck: 觸發後滿 5 分鐘 / enqueue END / 保留既有錯誤
    Damaged --> EndCheck: VAD_END / enqueue END / 保留既有錯誤
    Recording --> EndCheck: pipeline stop / 強制標記失敗並結束
    Damaged --> EndCheck: pipeline stop / 強制結束
    BeginRejected --> FAILED: VAD_END 或 pipeline stop / fallback callback
    EndCheck --> Finalizing: END 成功入列
    EndCheck --> FAILED: END 入列失敗 / fallback callback
    Finalizing --> ResultCheck: 關閉 WAV、更新 header、append audio_events.csv
    ResultCheck --> READY: PCM 連續且所有 writer / metadata 操作成功
    ResultCheck --> FAILED: producer、writer、close 或 audio metadata 錯誤
    Recording --> FAILED: writer stop / 強制 finalize 並回報失敗
    Damaged --> FAILED: writer stop / 強制 finalize 並回報失敗
    READY --> [*]: audio terminal callback / EVENT_AUDIO_READY
    FAILED --> [*]: audio terminal callback / EVENT_AUDIO_READY
```

WAV 保存 **pre-DSP RAW PDM capture，16 kHz / mono / PCM16**，路徑為 `/sd/event_<16HEX>.wav`。pre-roll 預設要求 1,000 ms，ring 容量為 2,000 ms；快照包含 VAD 延遲期間已捕捉的 RAW 資料，冷啟動只保存實際可用的資料。

`audio_last_sample` 是實際保存資料的 exclusive boundary，可晚於 `vad_end_sample`。writer 的早期 open/write 錯誤先保留在結果中，到 END 或強制 finalize 才回報 terminal。BEGIN／END 入列失敗走 pipeline fallback；此時不能保證 WAV 或 SD metadata 完整。Audio 失敗不會取消 camera ownership。

`EVENT_AUDIO_READY` 是 terminal 訊息名稱，payload 可以是 `READY` **或 `FAILED`**，不包含 audio bytes。

## 4. Face 分支：request admission

```mermaid
flowchart TD
    Open[Event 開始] --> Initial[初始 request]
    Initial --> Done[保存當輪 FACE_BATCH_END 與所有 faces]
    Tick[錄音中的 30 秒時間點] --> Gate{Event 未結束且非匯出中?}
    Gate -->|否| Stop[不新增 request]
    Gate -->|是| Busy{已有 pending batch 或相機不可用?}
    Busy -->|是| Skip[跳過本週期，不補排]
    Busy -->|否| Periodic[新 request，重拍預算歸零]
    Periodic --> Done
    Done --> Miss{client NO_FACE 且重拍少於 3 次?}
    Miss -->|否| Wait[等待下一個 30 秒時間點或 Event 結束]
    Miss -->|是且 Event 仍 active| Retry[新 request，完整重拍，保留舊結果]
    Retry --> Done
```

下方 admission 圖套用於初始輪；後續週期的 BUSY／不可用直接跳過，不產生 request-0 假輪次。NO_FACE 重拍等待相機可用，但 VAD_END、5 分鐘上限或匯出 gate 後不再新增 Event request。任何早期輪次失敗仍使最終 Event 為 COMPLETE_WITH_ERROR；最後一輪成功不會清除歷史。

```mermaid
stateDiagram-v2
    direction TB
    [*] --> CameraCheck
    state CameraCheck <<choice>>
    state ClientCheck <<choice>>
    state RequestCheck <<choice>>
    state "PENDING / 已取得 request_id" as Accepted
    state "Event-owned camera pipeline" as Pipeline

    CameraCheck --> FAILED: camera init/start 不可用
    CameraCheck --> ClientCheck: camera 可用
    ClientCheck --> DISCONNECTED: BLE 未連線、未訂閱 event/image 或 MTU 小於 67
    ClientCheck --> RequestCheck: client ready / submit_event_capture
    RequestCheck --> BUSY: camera 已有 active request
    RequestCheck --> FAILED: 其他 submit 拒絕
    RequestCheck --> Accepted: Accepted / 綁定 event_id 與 request_id
    Accepted --> Pipeline: controller 成功發送 EVENT_OPEN
    Accepted --> DISCONNECTED: EVENT_OPEN 發送失敗且已斷線 / recover
    Accepted --> FAILED: EVENT_OPEN 發送失敗但仍連線 / recover
    Pipeline --> [*]: 依下一張圖產生 batch terminal
    BUSY --> [*]: FACE_BATCH_END / request_id = 0
    DISCONNECTED --> [*]: FACE_BATCH_END
    FAILED --> [*]: FACE_BATCH_END
```

未接受 camera request 時，`request_id = 0`，仍發布 Event lifecycle 並繼續錄音。`BUSY` 結束初始輪的 face 分支；後續 30 秒輪次仍可嘗試，忙碌的週期不補排。GPIO／BLE manual capture 共用 camera arbitration，但不建立 audio Event。

controller 必須在 Event-owned camera work 前成功發送 `EVENT_OPEN`，讓 PC 先建立 association。EventManager 也會發布相同的 OPEN；PC 以 idempotent 方式接受相同內容。

## 5. Camera / 多人臉 pipeline

此圖使用實際 `SystemState` 名稱。整個 composite 的錯誤出口涵蓋 capture、detect、rotation、bbox、crop、JPEG、BLE transfer 與 recognition validation 的失敗。

```mermaid
stateDiagram-v2
    direction TB
    [*] --> Active
    state "已接受的 camera request" as Active {
        [*] --> CAPTURING
        CAPTURING --> DETECTING: 取得 XGA RGB565 frame
        DETECTING --> CAPTURING: 無臉且可重拍 / 釋放舊 frame
        DETECTING --> DETECTING: 最後一張無臉 / 原地旋轉 180 度後再 detect 一次
        DETECTING --> NO_FACE: 全部嘗試仍無臉
        DETECTING --> CROPPING: 有臉 / 依面積排序 / 選第一張
        CROPPING --> ENCODING: 擴張 bbox、clamp、複製 crop
        ENCODING --> TRANSMITTING: JPEG 完成 / 釋放 crop slot
        TRANSMITTING --> WAITING_RESULT: IMAGE_BEGIN / CHUNK / END 成功 / 釋放 JPEG slot
        WAITING_RESULT --> WAITING_RESULT: 舊或錯誤 request_id / image_id / 忽略
        WAITING_RESULT --> CROPPING: 有效 OK、UNKNOWN 或 FAILED / 存 association / 還有下一張臉
        WAITING_RESULT --> COMPLETED: 最後一張有效回覆 / 發布 batch terminal
    }

    Active --> ERROR_RECOVERY: pipeline error、transfer disconnect 或 timeout
    NO_FACE --> IDLE: FACE_BATCH_END 為 NO_FACE / 釋放 request
    COMPLETED --> IDLE: 釋放 request
    ERROR_RECOVERY --> IDLE: 釋放 buffers / 必要時重啟 camera / 發布 batch terminal
    IDLE --> [*]

    note right of DETECTING
        預設 initial capture + 最多 2 次 recapture
        成功 detect 立即停止重拍
        rotation 沿用最後 frame，不再拍照
        rotation 仍屬 DETECTING
    end note

    note right of WAITING_RESULT
        每張 face 有獨立 image_id
        共用 event_id / request_id / face_count
        face_index 由 0 開始
        每張 recognition 等待上限 15 秒
        有效 FAILED 回覆也繼續下一張臉
    end note
```

`SystemState::COMPLETED` 表示這個 camera loop 已處理完全部回覆，並不保證 `FaceBatchStatus` 是 `COMPLETED`。實際 batch terminal 判斷如下；`completed = recognized + unknown + failed`。

| 結束原因 | 條件／優先順序 | FaceBatchStatus |
|---|---|---|
| 所有 capture／fallback 都無臉 | expected = completed = 0 | `NO_FACE` |
| 全部 N 張有效回覆 | failed = 0，允許 UNKNOWN | `COMPLETED` |
| 全部 N 張有效回覆 | failed > 0，且 recognized + unknown > 0 | `PARTIAL` |
| 全部 N 張有效回覆 | 所有回覆皆 FAILED | `FAILED` |
| Pipeline 中途失敗 | **先判斷** completed > 0；包含先前有效 FAILED 回覆 | `PARTIAL` |
| Pipeline 中途失敗 | completed = 0，原因為 disconnect | `DISCONNECTED` |
| Pipeline 中途失敗 | completed = 0，原因為 image／recognition timeout | `TIMEOUT` |
| Pipeline 中途失敗 | completed = 0，其他錯誤 | `FAILED` |

Pipeline error 會停止剩餘 faces，保留所有已返回的 association，回 `IDLE` 接收其他 request。camera 完成不會等待 WAV；EventManager 仍保存這個 Event，直到 audio 也 terminal。

## 6. PC EventStore：接收終態與保存結果

PC 沒有 firmware 的 `OPEN / WAITING` enum；以下是依 terminal flags 與 `completed_at` 展開的接收狀態。

```mermaid
stateDiagram-v2
    direction TB
    [*] --> Receiving: EVENT_OPEN 或任一 branch terminal / 建立 SQLite row
    state "Event 未完成 / 收集 lifecycle 與 faces" as Receiving
    state "已收到兩個 device branch terminals" as TerminalsReceived
    state "PC 已保存完成 Event" as StoredComplete

    Receiving --> Receiving: EVENT_OPEN / 建立或補齊該 request 的 batch mapping
    Receiving --> Receiving: image / recognition 保存後才嘗試 BLE 回傳
    Receiving --> Receiving: 一個 branch terminal / 設定對應 received flag
    Receiving --> Receiving: BLE disconnect / 保存觀察與既有結果
    Receiving --> Receiving: 過早 EVENT_COMPLETE / 拒絕並保留既有資料
    Receiving --> TerminalsReceived: 另一個 device branch terminal 到達
    TerminalsReceived --> TerminalsReceived: BLE disconnect / 保留兩個 terminal
    TerminalsReceived --> StoredComplete: EVENT_COMPLETE 且 IDs / branch statuses 一致
    StoredComplete --> StoredComplete: 相同 OPEN / terminal / COMPLETE 重送 / idempotent
    StoredComplete --> [*]

    note right of Receiving
        branch terminal 可先於 OPEN 建立 shell row
        image 必須已有 Event request mapping
        本地 disconnect 不設定 device terminal flag
        audio PENDING 維持 PENDING
        不推算或虛構 EVENT_COMPLETE
    end note
```

PC 會拒絕 conflicting duplicate、跨 Event／boot-session 的 ownership、stale image／recognition，以及與既有終態不一致的 COMPLETE。斷線觀察只把尚未收到 device face terminal 的 face status 標成 `DISCONNECTED`；所有既有 faces／recognition 保留，未完成 Event 等待真實 device terminal。重連不自動重播錯過的 lifecycle；SD／structured logs 是缺失 Event 的追查依據。

## 7. 狀態、訊息與保存位置

| 發生點 | BLE lifecycle | SD／PC 保存 |
|---|---|---|
| 接受 VAD Event／face request mapping | `EVENT_OPEN` | SD lifecycle OPEN／FACE_BATCH_OPEN；PC `events` 與 `event_batches` |
| 單張有效 recognition 回覆 | PC → ESP `RECOGNITION_RESULT` | SD `face_associations.csv`；PC `event_faces`，另追蹤 `result_sent` |
| Audio terminal | `EVENT_AUDIO_READY`，status 為 READY 或 FAILED | WAV、`audio_events.csv`、lifecycle AUDIO_READY；PC audio status／sample boundaries |
| Face batch terminal | `FACE_BATCH_END` | face associations 的 terminal sentinel 與 lifecycle FACE_BATCH_END；PC `event_batches` 各輪 status／counts |
| Audio + Face 都 terminal | `EVENT_COMPLETE`，含最終 Event／audio／face status | lifecycle COMPLETE；PC `event_status` 與 `completed_at` |

Face terminal sentinel 的 `image_id = 0`、`face_index = -1`，不是一張臉。BLE lifecycle 發送失敗會記錄 protocol failure，但不阻止本地 Event 狀態完成與 slot 釋放；SD metadata 入列／寫入失敗也會另外計數與記錄，不能假設每次完成都留下完整的外部證據。

## 實作對照

- [EventState、terminal validation 與完成判斷](../firmware/components/pipeline_core/include/event_types.hpp)：`apply_vad_end()`、`apply_audio_terminal()`、`apply_face_terminal()`、`refresh_event_state()`。
- [Event ID、16-slot admission 與 release](../firmware/components/event_manager/include/event_ledger.hpp)：`EventLedger::open()`、`release()`。
- [EventManager callbacks、分支 admission 與 publication](../firmware/components/event_manager/event_manager.cpp)：`on_open()`、`on_end()`、`run()`、`publish_complete()`。
- [VAD 邊緣](../firmware/components/vad/vad_service.c)：`vad_service_on_afe_result()`。
- [Audio pre-roll、BEGIN／END 與 fallback](../firmware/components/audio_pipeline/audio_pipeline.c)：`vad_event()`、`end_event()`、`pipeline_task()`。
- [WAV writer 與 audio terminal](../firmware/components/recorder/sd_recorder.c)：`recorder_task()`、`finish_event()`。
- [Camera loop、recognition 與 recovery](../firmware/components/system_controller/system_controller.cpp)：`pipeline_task()`、`await_result()`、`recover()`。
- [Camera arbitration](../firmware/components/trigger_service/trigger_service.cpp)：`submit()`、`submit_event()`、`complete()`。
- [PC durable lifecycle](../pc_client/src/pc_client/event_store.py)：`audio_ready()`、`face_batch_end()`、`complete_event()`、`mark_disconnected()`。
- [Event 1 架構與操作](EVENT1.md)、[BLE wire layouts](BLE_PROTOCOL.md)。
