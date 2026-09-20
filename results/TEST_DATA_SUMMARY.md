# 測試資料完整整理

原始整理日期：2026-09-13  
GitHub 基準比對日期：2026-09-20  
最新工作區盤點日期：2026-09-21  
資料範圍：`results/` 內現有紀錄，最晚實測資料為 2026-08-04  
時區說明：Phase 05 CSV 使用 UTC（`Z`）；ESP32 日誌使用開機後毫秒；其他 Markdown 紀錄依檔案文字為準。

> 重要：本文件整理的硬體證據最晚來自 2026-08-04，早於目前尚未提交的 XGA、多臉依序處理、重拍及 180 度 fallback 修改。這些舊資料只能描述先前版本，不能當作新流程的硬體驗證。

## 0. 與 GitHub 前一版本的差異

比對基準為 <https://github.com/CrocoFishing/DetectPipeDemo> 的 `main` 分支提交 `346a1ac9fb57d0c28c58f3aa1e236d7c76c76c3b`（`first commit`，2026-08-07）。更新遠端索引後，`HEAD` 與 `origin/main` 相同；在本次文件更新前，所有 Git 已追蹤的 source、configuration、protocol、test plan 與 documentation 都沒有內容差異。

2026-09-20 比對快照相對 GitHub 基準新增的是測試證據，而不是程式碼：

| 本機新增位置 | 檔案數 | Git 狀態 | 內容 |
|---|---:|---|---|
| `results/phase01/actual/` | 2 | ignored | Camera/PSRAM serial log、部分填寫的結果表 |
| `results/phase02/actual/` | 4 | ignored | initialization、normal、short/bouncy、hold 四份 serial log |
| `results/phase03/actual/` | 7 | ignored | 6 份 detection/JPEG log、部分填寫的結果表 |
| `results/phase04/actual/` | 2 | ignored | board protocol smoke log、含 13 項 host test 輸出的結果表 |
| `results/phase05/actual/` | 8 | ignored | 7 份 BLE metrics CSV、未填寫的結果表 |
| `results/phase06/actual/` | 3 | ignored | ESP32/PC end-to-end log、未填寫的結果表 |
| `results/TEST_DATA_SUMMARY.md` | 1 | untracked（比對當時） | 本彙整與衍生統計 |

合計有 27 個 GitHub 基準不存在的檔案，其中 26 個 raw evidence 受 `.gitignore` 的 `results/**/actual/*` 規則保護，只保留在本機；本文件不受忽略，適合納入版本控制。基於隱私與可審查性，本文件只整理既有 log/CSV 的必要數值，不複製影像、embedding 或更多人物資料。

本次比對的文件化變更另記於根目錄 [`CHANGELOG.md`](../CHANGELOG.md)，並同步修正 `README.md` 的驗證狀態與 `FILE_MANIFEST.md` 的結果檔案清單。

2026-09-21 工作區另出現尚未提交的 source、protocol、client 與 test-plan 修改；其內容與驗證狀態統一記錄在 `CHANGELOG.md` 的 **Unreleased** 章節，並應與本份歷史證據分開提交與審查。

## 1. 結論摘要

- 本次整理前，`results/` 原始資料共有 33 個檔案：27 個有效內容檔，以及 6 個 `.gitkeep` 目錄占位檔。加上本整理文件後，目前共有 34 個檔案。
- 六個 firmware app 都有成功的編譯／連結紀錄；主機端格式、靜態型別、單元測試與協定一致性檢查也都通過。
- Phase 04 有最完整的正式自動測試證據：13 個協定測試全數通過，板端煙霧測試也輸出 `result=PASS`。
- Phase 01、02、03、05、06 都有成功的板端或端到端煙霧測試紀錄，但沒有完成各自測試計畫要求的全部重複次數、情境或結果表欄位，因此不能只靠目前檔案宣告正式硬體驗收 PASS。
- Phase 05 共 173 筆 BLE 傳輸資料，全部 `transfer_success=True`，沒有 missing sequence、duplicate sequence、CRC failure、disconnect、retry 或 queue congestion。共傳送 9,200,000 bytes（約 8.77 MiB）、19,162 chunks。
- Phase 05 計畫的最低資料量是 270 筆，目前是最低要求的 64.1%。原始 CSV 沒有 distance 與 environment 欄，無法可靠還原完整的 3 距離 × 3 環境矩陣。
- Phase 06 已觀察到四個請求：1 個 PC 觸發已知人物、1 個外部按鈕觸發已知人物、1 個外部按鈕觸發 UNKNOWN、1 個外部按鈕觸發 NO_FACE。四個流程最後都回到 IDLE。
- 所有結果模板的正式 PASS/FAIL、測試者、日期或 hardware status 仍未完整填寫；Phase 02 的 `actual/` 甚至沒有結果模板。

### 證據覆蓋評估

| Phase | 已記錄證據 | 相對測試計畫覆蓋 | 證據評估 | 正式狀態 |
|---|---|---|---|---|
| 01 Camera | 1 次開機、1 次 QVGA RGB565 擷取、PSRAM/heap | 計畫要求 10 次 reset/capture | 單次煙霧測試成功，記憶體前後一致 | 模板未勾選，不足以正式 PASS |
| 02 Trigger | 21 個外部按鈕 request、15 accepted、6 BUSY；另有 mock BLE arbitration | normal 10/10、short log 10 筆、hold 1 筆；5 秒保持與獨立 active-work 情境未完整標註 | debounce/arbitration/state path 有良好證據 | 無 actual 結果表，不足以正式 PASS |
| 03 Face pipeline | 6 次 JPEG_READY：5 次單臉、1 次雙臉 | 計畫要求單臉／無臉／多人各 10 次，另含 edge face | 幾何與 JPEG metadata 一致；缺無臉、邊界 clamp、PC decode 證據 | 模板未勾選，不足以正式 PASS |
| 04 Protocol | 13/13 pytest PASS，0.06 s；板端 smoke PASS | 自動測試集合完整 | 主機協定測試可視為已成功執行 | 模板狀態仍未填；硬體 smoke 狀態未勾選 |
| 05 BLE | 173 筆、7 批；全部傳輸成功 | 最低 270 筆，且條件標籤不完整 | 現有資料品質良好，但矩陣與故障恢復測試不足 | 模板未勾選，不足以正式 PASS |
| 06 End-to-end | 4 個 accepted flow；3 個完整辨識傳輸、1 個 NO_FACE | 計畫要求外部按鈕 10 次、PC 觸發 10 次及更多異常情境 | 四個代表性 smoke flow 成功 | 模板未勾選，不足以正式 PASS |

## 2. 檔案盤點

| 類別 | 檔案數 | 內容 |
|---|---:|---|
| 共用驗證紀錄 | 1 | `LOCAL_VERIFICATION.md` |
| Phase 01 | 2 | 1 份 serial log、1 份部分填寫模板 |
| Phase 02 | 4 | initialization、normal、short/bouncy、hold 四份 serial log |
| Phase 03 | 7 | 6 份偵測 log、1 份部分填寫模板 |
| Phase 04 | 2 | 1 份板端 log、1 份含 pytest 輸出的模板 |
| Phase 05 | 8 | 7 份 BLE CSV、1 份未填模板 |
| Phase 06 | 3 | 1 份 ESP log、1 份 PC log、1 份未填模板 |
| 占位檔 | 6 | 每個 phase 各 1 個 `.gitkeep`，不含測試資料 |

原始有效內容檔總數為 27；加上本文件後，有效內容檔為 28。原始資料均保留不變，本文件只做彙整與衍生計算。

## 3. 共用本機驗證紀錄

來源：`results/LOCAL_VERIFICATION.md`

### 環境

| 項目 | 紀錄值 |
|---|---|
| 日期 | 2026-08-01 |
| 主機 | Windows PowerShell |
| 板子連線 | 未連接 ESP32-S3 |
| 直接執行 `idf.py --version` | 原始 shell 找不到命令 |
| repo wrapper `tools/idf.ps1 ... --version` | ESP-IDF v6.0 |
| ESP-IDF commit | `662a3be354759d9487bf4b1a629fadb766cb1800`，標記為 `v6.0-dirty` |
| Python | CPython 3.13.7 |
| Git | 2.49.0.windows.1 |
| Swift compiler | 找不到，因此未做 Swift syntax compilation |

### Firmware build

這些結果只代表 configure、compile、link 成功，不代表 flash 或硬體功能通過。

| App | 結果 | Binary bytes |
|---|---|---:|
| `phase01_camera` | PASS | 253,424 |
| `phase02_trigger` | PASS | 157,184 |
| `phase03_face_pipeline` | PASS | 2,596,032 |
| `phase04_protocol` | PASS | 148,976 |
| `phase05_ble` | PASS | 531,600 |
| `phase06_end_to_end` | PASS | 2,916,640 |

Phase 05 與 06 在最終 BLE security、timeout 與 Device Information 修改後有重新 build。

### Host validation

| 檢查 | 紀錄結果 |
|---|---|
| editable PC-client installation | PASS |
| `ruff format --check pc_client` | PASS，12 files already formatted |
| `ruff check pc_client` | PASS |
| `ruff check tests/phase03/extract_jpeg.py` | PASS |
| `mypy pc_client/src` | PASS，8 source files |
| `pytest pc_client -q` | PASS，18 tests |
| YAML/Python/C++/Swift protocol consistency subset | PASS，2 tests |
| `python -m pc_client --help` | PASS，8 個必要 subcommand 都存在 |
| Phase 03 JPEG restore utility `--help` | PASS |
| Swift syntax compilation | NOT RUN |

### 本紀錄明確排除的項目

本機驗證紀錄本身沒有執行相機、GPIO、ESPDet、BLE radio matrix、InsightFace 真實照片與 Phase 06 端到端硬體測試。後續 phase log 顯示部分硬體測試確實在 2026-08-01 至 2026-08-04 另外執行，但不應回填或改寫 2026-08-01 這份「無板連線」紀錄的原始結論。

## 4. Phase 01：Environment and Camera

來源：`results/phase01/actual/full_log`、`results/phase01/actual/RESULT_TEMPLATE.md`

### 啟動與硬體資訊

| 項目 | 紀錄值 |
|---|---|
| ESP-IDF | v6.0-dirty |
| Bootloader compile time | 2026-08-01 02:59:58 |
| App compile time | 2026-08-01 02:59:35 |
| Project | `phase01_camera` |
| App version | 1 |
| ELF SHA256 前綴 | `b39dd133f...` |
| ESP32-S3 chip revision | v0.2 |
| CPU | 240 MHz |
| Flash | 8 MB，QIO，80 MHz |
| PSRAM | 8,388,608 bytes，80 MHz，memory test OK |
| Sensor | OV3660，PID `0x3660`，I2C address `0x3c` |
| Camera mode | RGB565、QVGA、2 個 PSRAM frame buffers |

### 擷取結果

| 指標 | 數值 | 核對 |
|---|---:|---|
| Width | 320 px | 符合 QVGA |
| Height | 240 px | 符合 QVGA |
| Frame bytes | 153,600 | 精確等於 320 × 240 × 2 bytes，符合 RGB565 |
| Format code | 0 | 日誌同時標示 RGB565 |
| Capture 記錄時間 | boot 後約 1,360 ms | `before_capture` 到 frame log 約 341 ms |
| Test marker | `PASS_LOCAL_CHECK` | 同一行明確標示 `hardware_claim=false` |

### Heap/PSRAM

| 階段 | Internal free | Internal largest | PSRAM free | PSRAM largest |
|---|---:|---:|---:|---:|
| startup | 334,035 | 270,336 | 8,386,308 | 8,257,536 |
| before_capture | 297,991 | 241,664 | 8,077,860 | 7,995,392 |
| after_frame_return | 297,991 | 241,664 | 8,077,860 | 7,995,392 |
| startup → before_capture delta | -36,044 | -28,672 | -308,448 | -262,144 |
| before_capture → after_frame_return delta | 0 | 0 | 0 | 0 |

兩個 frame buffer 的影像資料本體是 307,200 bytes；PSRAM free 從 startup 到 before capture 減少 308,448 bytes，與兩個 buffer 加少量配置開銷一致。frame return 前後四個 heap 指標完全相同，這次擷取沒有觀察到額外流失；但只有一個 boot/capture，不能代替計畫要求的 10 次 reset soak test。

### 缺口

- 模板只有 Run 1 的數字，PASS/FAIL 與 Hardware status 都未填。
- 缺少測試者、日期、board、port 與完整 ESP-IDF commit 欄位。
- 只有一次開機與一次 capture，計畫要求 10 次。
- 沒有 CSV per-boot 紀錄，因此無法分析多次啟動的 heap trend。

## 5. Phase 02：Triggers and State Machine

來源：`results/phase02/actual/full_log_init`、`full_log_normal`、`full_log_short`、`full_log_hold`

### 初始化與 mock arbitration

- GPIO2 外部觸發在系統初始化後啟用，debounce 為 40 ms。
- 狀態由 `INITIALIZING` 正常進入 `IDLE`。
- Mock BLE request 使用 image/request ID 42，從 IDLE 進入 CAPTURING。
- `mock accepted=0 busy=1 duplicate_while_busy=1` 表示 result code 0 為接受、工作中請求回傳 code 1。
- mock cycle 在 500 ms 後 `CAPTURING → COMPLETED → IDLE`。
- 完成後再次使用 ID 42 回傳 code 2，且記錄 `expected=2`，支持 duplicate-after-complete 判斷。

### 外部按鈕統計

| Log | Request attempts | Accepted（code 0） | BUSY（code 1） | Accepted image IDs | 完整 cycle 時間 |
|---|---:|---:|---:|---|---|
| `full_log_normal` | 10 | 10 | 0 | 1–10 | 每次 503 ms |
| `full_log_short` | 10 | 4 | 6 | 11、14、17、20 | 每次 503 ms |
| `full_log_hold` | 1 | 1 | 0 | 21 | 503 ms |
| 合計 | 21 | 15 | 6 | 1–21 中被接受的 15 個 | 15 個外部 cycle 均完整回 IDLE |

外部 request ID 從 2,147,483,649 到 2,147,483,669，連續且全部設定 bit 31，即十六進位 `0x80000001` 至 `0x80000015`。這與外部按鈕 namespace 的設計一致。

`full_log_short` 的 6 個 BUSY request 是低位 ID 12、13、15、16、18、19。它們都發生在前一個約 503 ms 工作週期尚未結束時，沒有在回到 IDLE 後被延遲執行；這支持「busy request 不排隊」的要求。

### 狀態轉換統計

包含 mock BLE cycle 後，共有 16 個完整工作週期：

| Transition | 次數 |
|---|---:|
| `IDLE → CAPTURING` | 16 |
| `CAPTURING → COMPLETED` | 16 |
| `COMPLETED → IDLE` | 16 |
| `INITIALIZING → IDLE` | 1 |

16 個 cycle 的平均時間 502.8 ms，範圍 500–503 ms。沒有觀察到 cycle 缺尾、狀態不匹配或接受中的併發工作。

### 缺口

- `results/phase02/actual/` 沒有 `RESULT_TEMPLATE.md`，無正式表格或 Hardware status。
- 沒有測試計畫要求的 JSONL `{time,action,request_id,source,result,state}`。
- `full_log_hold` 只有一次 request 與 503 ms 工作週期；檔案本身沒有記錄按鈕實際維持 5 秒的電位或 release 時點，所以只能確認「片段內沒有重複 request」，不能獨立證明完整 5 秒 hold。
- 6 個 active-work BUSY 事件存在，但沒有另外標記為測試計畫的「During active work 5」獨立場景。
- 未記錄 GPIO 波形或 ISR timing，40 ms debounce 主要由初始化設定與輸出事件數支持。

## 6. Phase 03：Fixed Face Detection Pipeline

來源：`results/phase03/actual/1detected.log` 至 `6detected.log`、`RESULT_TEMPLATE.md`

### 共用設定與整體統計

- 6 個 log 都輸出 `JPEG_READY`。
- 實際偵測分布是 5 次 `face_detection_count=1`、1 次 `face_detection_count=2`；檔名的 1–6 是執行順序，不是偵測到的人臉數。
- `2detected.log` 至 `6detected.log` 記錄模型 `espdet_pico_224_224_face_s8_s3`、輸入 224×224、threshold 0.50；`1detected.log` 沒有保留 model-loaded 行。
- 5 個 log 保留 camera frame 行，皆為 320×240、153,600 bytes、RGB565；`1detected.log` 沒有保留該行。
- 所有 JPEG quality 都是 85。
- JPEG 大小平均 2,355.5 bytes，中位數 2,088 bytes，範圍 1,091–3,910 bytes。
- 六個 CRC32 全部不同。
- 從 face-count log 到 JPEG encoded log 的延遲平均 9.5 ms，範圍 3–16 ms。這是「偵測結果已產生後到 JPEG ready」的區段，不是完整推論時間。

### 每次執行明細

Box 格式為 `(x1,y1,x2,y2)`；寬高按 `x2-x1`、`y2-y1` 計算。

| File | Faces | Original box / WxH | Expanded box / crop WxH | Crop pixels | JPEG bytes | CRC32 | JPEG bytes/pixel | Raw RGB565 ÷ JPEG | 後處理至 JPEG |
|---|---:|---|---|---:|---:|---|---:|---:|---:|
| `1detected.log` | 1 | `(115,48,199,161)` / 84×113 | `(104,33,210,176)` / 106×143 | 15,158 | 3,583 | `caf4a5e4` | 0.236 | 8.46× | 16 ms |
| `2detected.log` | 1 | `(124,47,205,172)` / 81×125 | `(113,31,216,188)` / 103×157 | 16,171 | 3,910 | `445dd523` | 0.242 | 8.27× | 16 ms |
| `3detected.log` | 1 | `(131,90,188,173)` / 57×83 | `(123,79,196,184)` / 73×105 | 7,665 | 2,041 | `179fcc87` | 0.266 | 7.51× | 9 ms |
| `4detected.log` | 1 | `(156,157,184,193)` / 28×36 | `(152,152,188,198)` / 36×46 | 1,656 | 1,091 | `bdf7e359` | 0.659 | 3.04× | 3 ms |
| `5detected.log` | 1 | `(162,55,219,141)` / 57×86 | `(154,44,227,152)` / 73×108 | 7,884 | 2,135 | `df7ed894` | 0.271 | 7.39× | 9 ms |
| `6detected.log` | 2 | `(194,80,232,123)` / 38×43 | `(189,74,237,129)` / 48×55 | 2,640 | 1,373 | `db195ff4` | 0.520 | 3.85× | 4 ms |

所有 expanded box 的幾何寬高都與 `JPEG_READY width/height` 完全一致。每邊擴張量因像素取整而略有差異，總寬高增加約 25.6%–28.6%，與目標「每邊約 12.5%，總計約 25%」相符。

這 6 個 expanded box 全部位於 320×240 影像內，沒有任何一筆真的碰到邊界，因此無法用現有資料驗證 clamp 行為。雙臉案例只保留被選中的 box，未保留兩個 raw boxes 的完整面積，無法獨立重算「最大臉一定被選中」。

`4detected.log` 額外保留完成時 heap：internal free 248,323、internal largest 112,640、PSRAM free 7,011,036、PSRAM largest 6,946,816。沒有對應的 before 值，無法由這一點證明 buffer recovery 或沒有 leak。

### 缺口

- 沒有任何 `NO_FACE` log；模板 no-face 列未填。
- 沒有 edge-face/clamp 案例。
- 計畫要求單臉、無臉、多人各 10 次，目前只有 6 次 face-positive 記錄。
- log 沒有 `[JPEG_B64]` 與 `[JPEG_B64_END]`，也沒有輸出的 JPEG 檔；模板 PC decode 欄全空，因此不能直接重做 PC JPEG decode 驗證。
- 模板的 buffer recovery、PASS/FAIL、Hardware status 都未填。
- 模板備註指出倒置人臉必須更靠近鏡頭，但沒有距離、成功率或測試次數，不能量化方向敏感度。

## 7. Phase 04：Shared Binary Protocol

來源：`results/phase04/actual/RESULT_TEMPLATE.md`、`board.log`

### Host tests

結果模板保留的命令輸出為：13 tests passed，總時間 0.06 s。`tests/phase04/run_test.ps1` 指向兩個測試檔，對應的 13 個 test node 為：

1. serialize/deserialize
2. partial packet
3. invalid magic
4. unsupported version
5. invalid length
6. CRC mismatch
7. unknown message type
8. out-of-order reassembly and duplicate counting
9. missing chunk
10. reassembly CRC failure
11. reassembly timeout
12. YAML matches Python
13. C++ and Swift contain protocol constants

因此模板列出的 serialize、partial、invalid magic/version/length/type、CRC、duplicate/missing/out-of-order、reassembly/timeout、三端協定一致性都有對應通過的自動測試。

### Board smoke

板端 log：

- app_main 在 boot 後 600 ms 被呼叫。
- 603 ms 輸出 `valid=0 invalid_magic=2 result=PASS`。
- 609 ms 從 app_main 返回。

這表示 firmware 端接受 valid packet 的 result code 0，對 invalid magic 產生 code 2，並自行判定 smoke test PASS。

### 缺口

- Markdown 表格各列 PASS/FAIL 仍空白，Host status 與 Hardware smoke status 也未選定。
- pytest 輸出只有整體 dots 與總數，沒有 JUnit 或逐 node 原始輸出；上面的 13 項映射是依目前 `run_test.ps1` 與 test definitions 還原。

## 8. Phase 05：NimBLE GATT and Link Quality

來源：`results/phase05/actual/ble_metrics_*.csv`、`RESULT_TEMPLATE.md`

### 資料範圍與完整性

7 個 CSV 共 173 rows、每列 22 欄。總資料量 9,200,000 bytes（8.77 MiB），總 chunk 數 19,162，記錄的 transfer duration 合計 256.537 s。

全體資料的 row-level 平均 throughput 為 361.452 kbps，中位數 417.399 kbps；按總 bytes ÷ 總 duration 計算的加權 throughput 為 286.898 kbps。這三個數值的權重不同，不能互換：row mean 對每個 request 等權，加權值則對傳輸時間與資料量加權。

CSV 時間範圍為 2026-08-03 12:40:20–13:13:25 UTC，即台北時間 20:40:20–21:13:25。

### 資料品質檢查

每個 CSV 都符合以下結果：

- 22 個欄位齊全，沒有 null cell。
- 沒有完全重複的 row。
- timestamp 單調遞增。
- request ID 與 image ID 在各自檔案內唯一、連續，而且每列 `request_id == image_id`。
- request ID 會在不同檔案重用，通常從 100 重新開始；跨檔合併時必須使用「檔名／批次 + request ID」作為鍵。
- 所有 bytes、duration、throughput 都大於 0。
- 所有 MTU 都是 517。
- `application_throughput_kbps` 與 `image_bytes × 8 ÷ transfer_duration_ms` 的最大差異僅約 `1.7e-13`，屬浮點誤差，欄位計算一致。
- 20,000 / 50,000 / 100,000 bytes 一律分成 42 / 104 / 208 chunks；平均每 chunk 承載約 476.19 / 480.77 / 480.77 bytes，包含末尾不足一整 chunk 的影響。
- 所有列都是 `transfer_success=True`。
- 所有列的 missing sequence、duplicated sequence、CRC failure、disconnect、retry、queue congestion 都是 0。
- 所有列的 `reconnect_count=1`。此欄看起來是連線生命週期的累積 snapshot，不能把 173 列相加解讀成 173 次 reconnect。
- 每個檔案內的 RSSI current/min/max/mean 都是同一個常數，`rssi_stddev_dbm=0`。因此現有資料沒有保留 run 內 RSSI 波動，stddev 欄不能用來比較穩定性。

### 批次總覽

「條件」只依檔名描述，不額外推測實驗環境。`74_f`、`85_f` 的 `_f` 在 repo 內沒有定義，而且其中所有 row 仍是成功傳輸，不能把 `_f` 當成 failure。

| Batch | UTC window | Rows | 20k/50k/100k | RSSI | Success | Throughput mean | Median | Min–max | CV | Weighted throughput |
|---|---|---:|---|---:|---:|---:|---:|---:|---:|---:|
| `50cm` | 12:40:20–12:41:20 | 30 | 10/10/10 | -32 dBm | 30/30 | 495.8 | 491.4 | 450.2–610.0 | 6.1% | 488.6 kbps |
| `2m` | 12:46:25–12:47:25 | 30 | 10/10/10 | -55 dBm | 30/30 | 478.1 | 491.0 | 379.8–514.5 | 7.8% | 480.2 kbps |
| `wall` | 12:56:13–12:58:03 | 30 | 10/10/10 | -72 dBm | 30/30 | 219.7 | 195.0 | 116.7–408.0 | 38.4% | 174.8 kbps |
| `74` | 12:58:55–13:00:36 | 30 | 10/10/10 | -74 dBm | 30/30 | 218.5 | 205.4 | 133.6–410.5 | 28.4% | 196.8 kbps |
| `85_f` | 13:02:15–13:02:54 | 18 | 10/8/0 | -85 dBm | 18/18 | 266.5 | 262.9 | 128.0–483.3 | 37.3% | 240.8 kbps |
| `74_f` | 13:09:51–13:10:00 | 5 | 5/0/0 | -74 dBm | 5/5 | 245.1 | 156.9 | 95.1–465.8 | 66.4% | 172.4 kbps |
| `in_bag` | 13:12:24–13:13:25 | 30 | 10/10/10 | -46 dBm | 30/30 | 471.4 | 476.4 | 366.1–534.2 | 7.5% | 470.7 kbps |

單位：throughput 為 kbps。CV 是 throughput 的 sample standard deviation ÷ mean。不同 payload size 混合後的 batch duration 平均沒有直接比較意義，因此主要用 throughput 描述批次。

### 依 payload size 明細

| Batch | Bytes | n | Mean duration | Median duration | Duration min–max | Mean throughput | Median throughput | Throughput stddev | Throughput min–max | Weighted throughput |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| `50cm` | 20,000 | 10 | 314.9 ms | 315.2 ms | 262.3–355.4 | 511.6 | 507.7 | 46.4 | 450.2–610.0 | 508.0 |
| `50cm` | 50,000 | 10 | 813.2 ms | 801.0 ms | 786.8–845.1 | 492.3 | 499.4 | 14.5 | 473.3–508.4 | 491.9 |
| `50cm` | 100,000 | 10 | 1,655.4 ms | 1,677.7 ms | 1,582.9–1,682.9 | 483.5 | 476.9 | 11.9 | 475.4–505.4 | 483.3 |
| `2m` | 20,000 | 10 | 347.7 ms | 322.3 ms | 311.0–421.3 | 466.7 | 496.4 | 55.4 | 379.8–514.5 | 460.2 |
| `2m` | 50,000 | 10 | 830.4 ms | 791.4 ms | 786.8–950.2 | 484.0 | 505.5 | 33.9 | 421.0–508.4 | 481.7 |
| `2m` | 100,000 | 10 | 1,654.4 ms | 1,642.4 ms | 1,626.8–1,733.5 | 483.8 | 487.1 | 10.0 | 461.5–491.8 | 483.6 |
| `in_bag` | 20,000 | 10 | 343.6 ms | 328.1 ms | 299.5–437.0 | 472.3 | 487.7 | 56.6 | 366.1–534.2 | 465.7 |
| `in_bag` | 50,000 | 10 | 857.3 ms | 844.4 ms | 796.2–967.2 | 467.7 | 473.7 | 23.7 | 413.6–502.4 | 466.6 |
| `in_bag` | 100,000 | 10 | 1,688.2 ms | 1,677.9 ms | 1,634.9–1,797.5 | 474.3 | 476.8 | 14.3 | 445.1–489.3 | 473.9 |
| `wall` | 20,000 | 10 | 603.8 ms | 565.1 ms | 392.2–1,000.5 | 293.8 | 288.3 | 94.5 | 159.9–408.0 | 265.0 |
| `wall` | 50,000 | 10 | 2,178.8 ms | 1,931.4 ms | 1,492.7–3,427.4 | 201.0 | 210.0 | 58.3 | 116.7–268.0 | 183.6 |
| `wall` | 100,000 | 10 | 4,998.7 ms | 4,700.6 ms | 3,991.2–6,304.5 | 164.3 | 170.4 | 27.4 | 126.9–200.4 | 160.0 |
| `74` | 20,000 | 10 | 712.7 ms | 687.1 ms | 389.8–1,197.5 | 253.3 | 232.9 | 90.0 | 133.6–410.5 | 224.5 |
| `74` | 50,000 | 10 | 1,982.7 ms | 1,926.2 ms | 1,524.4–2,948.9 | 209.4 | 208.0 | 40.1 | 135.6–262.4 | 201.7 |
| `74` | 100,000 | 10 | 4,214.6 ms | 4,049.3 ms | 3,622.2–5,322.9 | 192.8 | 197.6 | 23.8 | 150.3–220.9 | 189.8 |
| `85_f` | 20,000 | 10 | 711.6 ms | 741.2 ms | 331.1–1,249.9 | 270.5 | 220.8 | 126.9 | 128.0–483.3 | 224.9 |
| `85_f` | 50,000 | 8 | 1,602.4 ms | 1,469.4 ms | 1,154.1–2,355.4 | 261.5 | 272.2 | 57.2 | 169.8–346.6 | 249.6 |
| `74_f` | 20,000 | 5 | 927.8 ms | 1,019.6 ms | 343.5–1,682.8 | 245.1 | 156.9 | 162.8 | 95.1–465.8 | 172.4 |

Throughput 欄位單位均為 kbps。

### 相對 `50cm` 的 mean throughput 差異

| Batch | 20 kB | 50 kB | 100 kB |
|---|---:|---:|---:|
| `2m` | -8.79% | -1.68% | +0.05% |
| `in_bag` | -7.68% | -4.99% | -1.91% |
| `wall` | -42.57% | -59.18% | -66.01% |
| `74` | -50.50% | -57.45% | -60.12% |
| `85_f` | -47.12% | -46.89% | 無資料 |
| `74_f` | -52.10% | 無資料 | 無資料 |

`50cm`、`2m` 與 `in_bag` 的完整批次平均吞吐量接近，且 CV 約 6%–8%。`wall` 與 `74` 的平均吞吐量明顯較低，變異也較高。`wall` 的 100 kB mean throughput 只有 164.3 kbps，mean duration 接近 5 秒，是完整批次中最弱的 size-condition 組合。

RSSI 與吞吐量大致有關，但不能由這批資料建立單調模型：例如 `85_f` 的 RSSI 為 -85 dBm，部分 payload 的 mean throughput 卻高於 `74` 與 `wall`。原因可能包含條件差異、batch effect、RSSI 更新粒度或小樣本；原始檔沒有足夠環境 metadata 可分離這些因素。

### 極值

- 全部資料的最高單筆 throughput：`50cm` request 102、20 kB、262.288 ms、610.016 kbps。
- 全部資料的最低單筆 throughput：`74_f` request 101、20 kB、1,682.765 ms、95.082 kbps。
- 完整 30-row 批次中最低單筆 throughput：`wall` request 119、50 kB、3,427.363 ms、116.708 kbps。
- 最長單筆 duration：`wall` 的 100 kB request，最長 6,304.493 ms。

### 相對測試計畫的缺口

- 計畫最低要求為 3 distances × 3 environments × 3 sizes × 10 repeats = 270 transfers；現有 173 筆是 64.1%。
- CSV 本身沒有 `distance`、`environment`、`batch_id`、tester、PC adapter、OS 或 firmware build ID 欄位。
- 明確距離檔名只有 `50cm`、`2m`，沒有明確 `5m` 檔案。
- `wall`、`in_bag` 是環境式檔名，但沒有距離；`74`、`74_f`、`85_f` 是訊號式檔名，沒有距離與環境定義。
- `74_f` 只有 5 筆 20 kB；`85_f` 只有 10 筆 20 kB、8 筆 50 kB、沒有 100 kB。
- 沒有 Phase 05 ESP serial log 或環境筆記，無法驗證 advertising、subscription、disconnect mid-transfer、重新廣播、未訂閱、low/changed MTU、buffer recovery。
- 所有資料都是成功案例；雖然目前沒有 corrupt success，卻也沒有真實 failure/recovery row 可驗證錯誤路徑。
- 模板只留一列空白示例，未將 CSV 統計回填，也未填 Hardware status。

## 9. Phase 06：InsightFace End-to-End

來源：`results/phase06/actual/full.log`、`full_PC.log`、`RESULT_TEMPLATE.md`

### PC 端環境

- InsightFace model family：`buffalo_l`。
- Execution provider：CPUExecutionProvider。
- 載入模型：`1k3d68.onnx`、`2d106det.onnx`、`det_10g.onnx`、`genderage.onnx`、`w600k_r50.onnx`。
- Detection size：640×640。
- PC log 有一個 `FutureWarning`：`face_align.py` 使用的 `estimate` API 已 deprecated，預告未來版本移除。這不影響此次紀錄的成功流程，但屬升級風險。
- Repo 範例設定與 CLI default 的 recognition threshold 是 0.45；結果模板本身沒有填 threshold，因此只能把 0.45 視為程式預設，不保證當次命令沒有 override。

### BLE 初始化

- 連線前 quality log 的 MTU 是 23。
- boot 後約 31.1 s negotiated MTU 變成 517，之後連線 handle 1。
- Event/Image notify handle 18、21 啟用；handle 8 顯示 notify 0。
- 後續 quality log 維持 disconnect 0、reconnect 1、failure 0、retry 0、congestion 0。

### 四個 request

Status code 依 protocol 定義：0=`OK`、1=`UNKNOWN`、2=`NO_FACE`、3=`FAILED`。

| Image | Trigger / request | Face result | JPEG | BLE | Recognition | State path / cycle |
|---:|---|---|---|---|---|---|
| 1 | PC `START_CAPTURE`, request 1 | 1 face | 7,373 bytes, Q85 | 16 chunks, 139.325 ms, 423.355 kbps, RSSI -60 dBm | status 0 OK；P001；similarity 0.6616；processing 349 ms | 完整 8 段 path，727 ms，回 IDLE |
| 2 | External button, request 2,147,483,649 | 1 face | 7,855 bytes, Q85 | 17 chunks, 147.048 ms, 427.343 kbps, RSSI -58 dBm | status 0 OK；P001；similarity 0.6345；processing 252 ms | 完整 8 段 path，678 ms，回 IDLE |
| 3 | External button, request 2,147,483,650 | 1 face | 11,547 bytes, Q85 | 24 chunks, 201.117 ms, 459.315 kbps, RSSI -52 dBm | status 1 UNKNOWN；similarity 0.2216；processing 284 ms | 完整 8 段 path，806 ms，回 IDLE |
| 4 | External button, request 2,147,483,651 | 0 faces | 無 JPEG，符合 no-face path | 無 image transfer | PC log 收到 `NO_FACE`，image 4 | `IDLE→CAPTURING→DETECTING→NO_FACE→IDLE`，176 ms |

為降低生物辨識資料擴散，本整理只保留原 log 已含的 stable ID，不複製影像、embedding 或額外個資。原 log 還含一個單字母 name 欄位，本表省略。

### 完整辨識 path 的狀態時間

| Image | CAPTURING | DETECTING | CROPPING | ENCODING | TRANSMITTING state | WAITING_RESULT | COMPLETED→IDLE | Total |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 9 ms | 156 ms | 3 ms | 47 ms | 221 ms | 284 ms | 7 ms | 727 ms |
| 2 | 16 ms | 150 ms | 4 ms | 50 ms | 229 ms | 222 ms | 7 ms | 678 ms |
| 3 | 16 ms | 157 ms | 7 ms | 70 ms | 284 ms | 265 ms | 7 ms | 806 ms |
| Mean | 13.7 ms | 154.3 ms | 4.7 ms | 55.7 ms | 244.7 ms | 257.0 ms | 7.0 ms | 737.0 ms |

`TRANSMITTING` state dwell 比 `ble_transfer_ms` 多，因為狀態區間還包含開始／結束通知及量測輸出；兩者不是同一個 timer。

3 個 JPEG 的平均大小 8,925 bytes，平均 transfer duration 162.497 ms，平均 application throughput 436.671 kbps，範圍 423.355–459.315 kbps。各次 RSSI min=max=mean=current、stddev=0，與 Phase 05 一樣沒有保留 run 內 RSSI 變化。

若當次使用預設 threshold 0.45，兩個 OK similarity 分別高於 threshold 0.2116、0.1845；UNKNOWN similarity 低於 threshold 0.2284。分類方向與 threshold 一致，但因結果模板沒有填 expected identity 或當次 threshold，不能單靠這些分數證明 ground-truth identification accuracy。

### PC/ESP 對照

- PC log 依序記錄 request 1、2,147,483,649、2,147,483,650、2,147,483,651 被接受，與 ESP 端一個 BLE_CLIENT 加三個連續 external-button flow 對得上。
- PC 的 `CAPTURE_ACCEPTED` 行對前四個 request 都顯示 `image_id=0`；只有 `NO_FACE` 行明確顯示 request 2,147,483,651 對 image 4。這可能是 acceptance event 尚未帶入 assigned image ID 的協定行為，但目前結果資料不足以逐筆用 PC log 的 image ID 直接 join。
- ESP 的 image 1–3 都完成 detection、crop、encode、transmit、wait result、completed、idle。
- image 4 在 device 偵測到 0 faces 後直接通知 NO_FACE 並回 IDLE，沒有多餘 JPEG 或 recognition result。
- PC 能產生 recognition result 強烈支持 JPEG 已被接收、解碼並進入 InsightFace，但結果表 JPEG decode 欄與 PC log 都沒有一行明確的 decode-success assertion。

### 缺口

- 計畫要求 external button 10 次與 PC START_CAPTURE 10 次；現有資料只有 external 3 次、PC 1 次。
- 已知／未知／無臉各有代表性樣本，但沒有 multiple-face、stale-result、disconnect/reconnect mid-flow 或 concurrent capture 案例。
- 模板的 model/threshold/database schema/build IDs/tester/date 全空。
- 模板 expected identity、JPEG decode、ESP returned IDLE、PASS/FAIL 沒有回填；IDLE 只能從 serial log 還原。
- 沒有 BLE CSV、DB metadata snapshot 或 database schema version 放在 Phase 06 actual 目錄。
- 結果中的 P001 是否為 ground truth 不能由 log 本身確認。

## 10. 跨 Phase 觀察

### 已有的強證據

- 六個 firmware target 都能在鎖定 dependency 下成功 build。
- Phase 01 實板能辨識 OV3660、初始化 8 MB PSRAM、擷取合法 RGB565 QVGA frame，且單次 frame return 前後 heap 不變。
- Phase 02 的 request namespace、BUSY rejection、duplicate-after-complete 與完整 state return 有一致日誌。
- Phase 03 的 expanded crop geometry 與 JPEG metadata 六次都一致。
- Phase 04 的 required host protocol suite 13/13 通過。
- Phase 05 的現有 173 筆成功資料沒有任何 sequence/CRC/queue/retry 錯誤，資料表本身結構完整。
- Phase 06 展示 PC trigger、external trigger、known、UNKNOWN、NO_FACE 五種核心能力中的四種流程組合，且都回 IDLE。

### 目前最影響正式驗收的問題

1. **結果決策未落盤**：多數模板仍是空白，成功 log 與正式 PASS/FAIL 沒有被測試者簽認連結。
2. **重複次數不足**：Phase 01、03、05、06 明顯低於各自 plan 的最低 runs。
3. **情境 metadata 不足**：Phase 05 沒有 distance/environment 欄；Phase 06 沒有 expected identity、實際 threshold、build ID。
4. **負向／恢復路徑不足**：Phase 05 沒有失敗與 recovery row；Phase 06 沒有 stale result、disconnect、concurrency。
5. **可重現性不足**：多數 serial log 只有 boot-relative time，缺 tester、board serial、port、firmware SHA、測試時間。
6. **部分命名易誤讀**：Phase 03 的 `Ndetected.log` 不是 face count；Phase 05 `_f` 沒有定義且資料仍全成功。
7. **RSSI 統計退化**：Phase 05/06 每列 min=max=mean=current 且 stddev=0，無法分析傳輸期間的訊號波動。
8. **ID 跨批重用**：Phase 05 每個 CSV 重新使用 100 起始的 ID，匯總資料不能只以 request ID 當主鍵。

### 建議補測優先序

1. 先補齊每個模板的 tester/date/board/adapter/build SHA、正式 status 與原始 log 路徑。
2. Phase 05 在 CSV 新增 `batch_id,distance_m,environment,tester,adapter,firmware_sha`，按 9 個 distance/environment cell 補到每 size 至少 10 次。
3. Phase 06 補足 10 次 external 與 10 次 PC trigger，並加入 multiple face、stale result、disconnect、concurrent request。
4. Phase 03 補 10 次 no-face、多人臉 raw boxes、edge/clamp 與可實際還原的 device JPEG decode evidence。
5. Phase 01 補 10 次 power-cycle CSV，用 boot 序號追蹤 internal/PSRAM free 與 largest block trend。
6. 調整 RSSI 收集，使長傳輸能保留多個 sample，否則移除沒有資訊量的 stddev 或註明 sample count=1。

## 11. 原始來源索引

| Phase | 原始資料 |
|---|---|
| 共用 | `results/LOCAL_VERIFICATION.md` |
| 01 | `results/phase01/actual/full_log`、`RESULT_TEMPLATE.md` |
| 02 | `results/phase02/actual/full_log_init`、`full_log_normal`、`full_log_short`、`full_log_hold` |
| 03 | `results/phase03/actual/1detected.log` 至 `6detected.log`、`RESULT_TEMPLATE.md` |
| 04 | `results/phase04/actual/board.log`、`RESULT_TEMPLATE.md` |
| 05 | `results/phase05/actual/ble_metrics_50cm.csv`、`ble_metrics_2m.csv`、`ble_metrics_wall.csv`、`ble_metrics_74.csv`、`ble_metrics_85_f.csv`、`ble_metrics_74_f.csv`、`ble_metrics_in_bag.csv`、`RESULT_TEMPLATE.md` |
| 06 | `results/phase06/actual/full.log`、`full_PC.log`、`RESULT_TEMPLATE.md` |

衍生統計以原始欄位為準；沒有從檔名推導未記錄的測試條件，也沒有把 build success 等同硬體 PASS。
