# Event 資料契約（iOS 匯入參考）

本文件描述 `event1_audio_face` 實際寫入 SD 的 Event 資料。一次 VAD utterance 是一個 Event；每個 Event 擁有一條音訊分支，以及最多一個包含多張臉的相機 request。音訊與人臉可以先後以任意順序結束。

SD 原始 CSV/WAV 是持久資料契約；[BLE_PROTOCOL.md](BLE_PROTOCOL.md) 描述即時通知的二進位格式，[IOS_SD_EXPORT.md](IOS_SD_EXPORT.md) 描述完整下載。匯出 manifest 的 `format_version=1` 是下載清單版本；既有 CSV 以本文件列出的 header 識別欄位。Swift 參考型別位於 `clients/ios/EventDataTypes.swift`，CSV 匯入時先正確處理引號，再按欄名轉型。

## 識別、關聯與時間

| 值 | 儲存／Swift 表示 | 語意 |
|---|---|---|
| `event_id` | 16 位大寫十六進位字串／`SDStoredEventID` | 非零；保留前導零。`boot_session_id << 32 \| event_sequence`。 |
| `boot_session_id` | Event ID 前 8 位 hex／`UInt32` | NVS `event1/boot_session` 每次啟動成功提交後遞增。 |
| `event_sequence` | Event ID 後 8 位 hex／`UInt32` | 本次啟動內從 1 遞增。匯出暫停／恢復不重設。 |
| `request_id`, `image_id` | CSV 十進位／`UInt32` | request/image 的作用域是 boot session；以 `(boot_session_id, id)` 建索引。 |
| sample 欄位 | CSV 十進位／`UInt64` | 單位是 16 kHz mono PCM frame；與 byte offset 不同。 |
| `*_timestamp_us` | CSV 十進位／`Int64` | `esp_timer` 的開機後微秒，**不是 Unix time**；跨 boot 無法直接排序成真實日期。 |

例如 `0000002500000068` 的 boot session 是 37、sequence 是 104。iOS/JSON/database 應保存這個字串，避免 `Double` 的 53-bit 精度限制。保留 NVS 可維持跨重開機唯一性；清除／更換 NVS 會開啟新的識別歷史。

SD 平面目錄的既有 Event 檔案如下；匯出也會包含其他一般檔案及子目錄。

```text
event_<EVENT_ID>.wav
audio_events.csv
face_associations.csv
event_lifecycle.csv
```

三份 CSV 以 `event_id` join。人臉 JPEG 在現有流程中經 BLE 即時傳送，沒有寫到 SD，因此歷史匯出資料不含該 JPEG。GPIO／BLE 手動拍照是診斷 capture，沒有音訊 Event。

CSV 使用 UTF-8、逗號分隔、含 header，writer 使用 LF；帶引號的 `person_id` 可以包含逗號、雙引號與換行，雙引號以 `""` 轉義。請使用完整 CSV parser；不能用逐行 `split(",")`。整數欄位為無千分位的十進位，空白欄位表示該列不適用。

## 音訊資料

`audio_events.csv` 的 header 與順序：

```csv
event_id,audio_file,audio_source,sample_rate,channels,bits_per_sample,audio_first_sample,vad_start_sample,vad_end_sample,audio_last_sample,vad_start_timestamp_us,vad_end_timestamp_us,audio_status
```

| 欄位 | 型別／值 | 語意 |
|---|---|---|
| `event_id` | Event ID 字串 | 該筆音訊的 owner。 |
| `audio_file` | 相對檔名字串 | `event_<EVENT_ID>.wav`；不得直接將未驗證路徑寫入 App 檔案系統。 |
| `audio_source` | `RAW` | 儲存 DSP 前的擷取 PCM；不是 PDM bitstream。 |
| `sample_rate` | `UInt32`，16000 | 每秒 frame 數。 |
| `channels` | `UInt16`，1 | 單聲道。 |
| `bits_per_sample` | `UInt16`，16 | signed PCM16 little-endian。 |
| `audio_first_sample` | `UInt64` | 實際存檔的第一個 sample，包含可取得的 pre-roll。 |
| `vad_start_sample` | `UInt64` | VAD utterance 起點。 |
| `vad_end_sample` | `UInt64` | VAD utterance 結束邊界。 |
| `audio_last_sample` | `UInt64` | 實際存檔末端的 **exclusive** sample 邊界。 |
| `vad_start_timestamp_us` | `Int64` | VAD 起點的開機後微秒。 |
| `vad_end_timestamp_us` | `Int64` | VAD 結束的開機後微秒。 |
| `audio_status` | `READY`／`FAILED` | 兩者都是音訊終態；失敗資料可保留供診斷。 |

WAV 為標準 44-byte RIFF/WAVE PCM header，接 16 kHz／mono／PCM16 little-endian。對 READY 音訊應驗證：

```text
audio_first_sample <= vad_start_sample <= vad_end_sample <= audio_last_sample
saved_frames = audio_last_sample - audio_first_sample
PCM data bytes = saved_frames * 2
file bytes = 44 + PCM data bytes
```

應解析 RIFF chunk 與 PCM 格式再比對 frame 數；不能僅憑副檔名認定可播放。預設 pre-roll 目標為 1000 ms，冷啟動可能不足；儲存範圍也包含 VAD 決策延遲期間的實際 RAW frames。FAILED WAV 可能截短、無檔案或需要修復，不能套用 READY 的成功假設。開機恢復僅修復最新 WAV header，不會補造 CSV 終態。

sample index 在匯出暫停後保留，但停止擷取期間沒有新 samples；不要用 sample index 推算停機期間的經過時間或真實日期。

## 人臉關聯與 batch 終態

`face_associations.csv` 的 header：

```csv
event_id,request_id,image_id,face_index,face_count,status,person_id,similarity,processing_time_ms
```

一般人臉列的型別：`event_id` 字串、`request_id/image_id/processing_time_ms` 為 `UInt32`、`face_index/face_count` 為 `UInt16`、`similarity` 為 `Float`、`person_id` 為 UTF-8 字串（最多 32 bytes）。`face_index` 從 0 開始且小於 `face_count`；臉按原始框面積由大到小，面積相同維持 detector 順序。`person_id` 是穩定身份鍵，顯示名稱沒有存入這份 CSV。

| `status` | 意義 |
|---|---|
| `RECOGNIZED` | client 回傳已辨識結果。 |
| `UNKNOWN` | client 完成辨識但沒有匹配身份；計入 completed。 |
| `FAILED` | client 回傳辨識失敗；保留其他已完成的人臉。 |

每個 batch 另外有一列 sentinel：

```text
image_id=0, face_index=-1, face_count=expected_face_count
person_id=<empty>, similarity=<empty>, processing_time_ms=0
```

sentinel 的 `status` 為 `COMPLETED / NO_FACE / PARTIAL / BUSY / TIMEOUT / DISCONNECTED / FAILED`。這列應映射成 `SDStoredFaceBatchTerminal`，不能加入人臉陣列或把 `face_index=-1` 轉成 unsigned。`face_count` 保留預期總數，即使零張結果成功回傳仍有參考價值。request 尚未被接受時 `request_id=0`，尤其是 BUSY 或事前斷線；0 不形成跨 Event 的 request 索引。

`completed = recognized + unknown + failed` 且 `completed <= expected`。COMPLETED 需要所有預期人臉完成且沒有 failed；NO_FACE 表示沒有臉，預期與完成數均為 0。PARTIAL／TIMEOUT／DISCONNECTED／FAILED 保留已有結果；缺少的臉不能自行補成 UNKNOWN。

## 生命週期與匯入判定

`event_lifecycle.csv` 的 header：

```csv
event_id,state,timestamp_us,status
```

`timestamp_us` 是寫入該 transition 時的開機後微秒。`state/status` 對照：

| `state` | `status` | 意義 |
|---|---|---|
| `OPEN` | `OK` | Event 已建立。 |
| `AUDIO_READY` | `READY`／`FAILED` | 音訊分支終態；state 名稱即使失敗仍是 AUDIO_READY。 |
| `FACE_BATCH_END` | 上述 batch 終態 | 人臉分支終態。 |
| `COMPLETE` | `COMPLETE`／`COMPLETE_WITH_ERROR` | 兩條分支皆終態。 |

OPEN 應最先出現，兩條分支終態順序不限，COMPLETE 在兩者之後。`COMPLETE` 需要 READY 音訊，以及 COMPLETED／NO_FACE 的人臉分支，且沒有失敗人臉。其他終態組合使用 `COMPLETE_WITH_ERROR`。

App 匯入時先按 Event ID 聚合，再驗證 lifecycle、音訊列、sentinel、一般人臉與 WAV；用 `(boot_session, request_id)`、`(boot_session, image_id)` 檢查 ownership。缺少必要列、CSV/WAV 不一致、遺失終態或斷電殘留，應保留可取得資料並顯示 App 推導的 `INCOMPLETE`／驗證錯誤。`INCOMPLETE` 不是 firmware 傳送的 Event status。重試下載是同一份檔案的替換／略過，不能把 CSV 全部再次 append 至 App database；可用 Event ID 與作用域內的 image ID 做匯入鍵。衝突的 ownership／重複終態應報錯，不採最後一列覆寫。

BLE 可能錯過舊通知；重新連線後以 SD 的持久資料補齊。PC 的 `tools/inspect_event_sd.py` 可作離線檢查參考，其輸出 JSON 是診斷格式，其中部分 CSV 數值仍是字串；iOS 型別轉換應按本文件處理。

## 可執行範例

`protocol/examples/event_data/` 提供三組刻意縮短的合成 WAV／原始 CSV：

| 資料夾 | Event ID | 預期 |
|---|---|---|
| `complete` | `0000002500000068` | READY 音訊、1 張 RECOGNIZED 加 1 張 UNKNOWN，COMPLETE。 |
| `busy` | `0000002500000069` | READY 音訊、request_id=0 的 BUSY sentinel，COMPLETE_WITH_ERROR。 |
| `incomplete` | `000000250000006A` | OPEN 與 DISCONNECTED sentinel，缺少音訊／完成終態，INCOMPLETE。 |

完整案例的三份 CSV 例如：

```csv
event_id,audio_file,audio_source,sample_rate,channels,bits_per_sample,audio_first_sample,vad_start_sample,vad_end_sample,audio_last_sample,vad_start_timestamp_us,vad_end_timestamp_us,audio_status
0000002500000068,event_0000002500000068.wav,RAW,16000,1,16,1000,1002,1004,1008,1000000,1000125,READY
```

```csv
event_id,request_id,image_id,face_index,face_count,status,person_id,similarity,processing_time_ms
0000002500000068,2147483650,1,0,2,RECOGNIZED,"P,""01",0.710000,52
0000002500000068,2147483650,2,1,2,UNKNOWN,"",0.310000,48
0000002500000068,2147483650,0,-1,2,COMPLETED,,,0
```

```csv
event_id,state,timestamp_us,status
0000002500000068,OPEN,1000000,OK
0000002500000068,FACE_BATCH_END,1000100,COMPLETED
0000002500000068,AUDIO_READY,1000200,READY
0000002500000068,COMPLETE,1000201,COMPLETE
```

這個 WAV 應有 8 frames／16 PCM bytes／60 file bytes。`P,"01` 是合法的單一 person_id，CSV 中的雙引號已轉義。可從專案根目錄執行 `python tools/inspect_event_sd.py protocol/examples/event_data/complete`，並將 `complete` 換成其他案例比對。
