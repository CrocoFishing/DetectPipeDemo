你是一位熟悉 ESP32-S3、ESP-IDF 6.x、FreeRTOS、esp32-camera、ESP-DL、ESP-WHO、NimBLE、BLE GATT、Python、Bleak、InsightFace 與嵌入式影像處理的資深工程師。

請在目前工作目錄中直接建立一個完整、可編譯、可分階段測試的 Demo repository。

不要只提供架構說明或零散程式碼。請一次完成 Phase 1 到 Phase 6 的所有程式、設定、測試程式、測試說明與結果紀錄範本。不需要等待我逐一確認各 Phase。

我會自行進行燒錄、實體硬體操作與測試，因此你必須提供清楚、可重複執行的測試程序，但不要宣稱未實際執行的硬體測試已經通過。

# 一、固定需求

## 硬體

- 開發板：Seeed Studio XIAO ESP32-S3 Sense
- SoC：ESP32-S3
- Camera sensor：OV3660
- PSRAM：使用開發板內建 PSRAM
- Flash：使用開發板內建 Flash
- 不使用螢幕
- 不使用麥克風
- 不使用語音播放
- 不使用 SD card 作為必要功能
- 不增加額外 UART module 作為 Demo 必要條件
- 開發板端觸發預設使用 BOOT button／GPIO0
- GPIO0 僅在系統完成啟動後作為按鈕使用
- 必須實作 debounce
- 必須避免開機時按住 GPIO0 所造成的 boot mode 問題

所有 camera pin、button pin、LED pin 與硬體設定必須集中在：

```text
components/board_support/include/board_config.hpp
```

不得將硬體 pin 散落寫死於不同模組。

## 開發環境

- Framework：ESP-IDF
- Target：esp32s3
- 主要語言：C++
- 必要時可使用 C API wrapper
- ESP-IDF 版本：使用目前安裝的 ESP-IDF v6.0
- 使用現有 ESP-IDF PowerShell profile
- ESP-DL、ESP-WHO 和其他 managed component 必須鎖定明確版本或 commit
- 不得將浮動 master 當作可重現依賴

請建立：

```text
DEPENDENCIES.md
```

其中記錄：

- ESP-IDF version
- ESP-DL version
- ESP-WHO version
- component version
- commit hash
- Python version
- Python package version
- build command
- 已知相容性限制

# 二、功能資料流

ESP32-S3 執行：

```text
IDLE
→ 收到 CaptureRequest
→ Camera capture
→ Face detection
→ 選擇最大臉
→ Bounding box 外擴 25%
→ Clamp 至原始影像範圍
→ Crop
→ JPEG encode
→ BLE 分段傳送
→ 等待 PC 辨識結果
→ 輸出結果
→ 回到 IDLE
```

PC 執行：

```text
連線 ESP32-S3
→ 發送 CaptureRequest，或等待開發板按鈕觸發
→ 接收 JPEG chunks
→ 驗證 sequence 與 CRC32
→ 重組 JPEG
→ InsightFace 提取 embedding
→ 與註冊資料庫比對
→ 將辨識結果透過 BLE 傳回 ESP32-S3
```

# 三、重要術語

請嚴格區分：

- Face detection：找出人臉 bounding box
- Face recognition：判斷人物身分

ESP32-S3 只執行 face detection。

PC 使用 InsightFace 執行 face recognition。

程式、log、文件、class name 和測試名稱不得混淆 detection 與 recognition。

# 四、固定人臉偵測方案

只能使用：

```text
espdet_pico_224_224_face_s8_s3
```

不得：

- 比較其他 detector
- 使用 YOLO
- 使用 SCRFD 作為 ESP32-S3 detector
- 使用 RetinaFace 作為 ESP32-S3 detector
- 轉換其他模型
- 重新訓練模型
- 測試不同 detector accuracy
- 測試不同 margin
- 建立模型 benchmark 比較表

固定處理規則：

1. 將相機畫面轉成模型要求的 224×224 輸入。
2. 執行 `espdet_pico_224_224_face_s8_s3`。
3. 將模型座標映射回原始相機 frame。
4. 過濾低於設定 confidence threshold 的結果。
5. 若有多張人臉，選擇 bounding-box 面積最大者。
6. 在四個方向外擴 25%。
7. 外擴方式必須明確定義：
   - 左右各增加原始 bounding-box 寬度的 12.5%。
   - 上下各增加原始 bounding-box 高度的 12.5%。
   - 最終總寬與總高約為原 bounding box 的 125%。
8. 將座標 clamp 至原始影像範圍。
9. 裁切臉部。
10. 將裁切結果編碼為 JPEG。
11. JPEG quality 必須集中設定，預設為 85。
12. 將 JPEG 傳送至 PC。

請不要把「margin 25%」實作成四個方向各增加 25%，避免最後寬高變成 150%。

模型與 margin 不需要進行 accuracy、性能比較或參數實驗。

但是必須保留最低限度整合檢查：

- 模型成功載入
- detector 回傳格式合法
- bounding box 不越界
- crop width 和 height 大於零
- JPEG encode 成功
- PC 能解碼 JPEG
- no-face 時系統可安全回到 IDLE

# 五、觸發來源

必須支援兩種觸發方式。

## Trigger A：開發板外接按鈕發起

使用外接於 D1／GPIO2 與 GND 之間的瞬時按鈕。

要求：

1. 系統初始化完成後才啟用 GPIO interrupt。
2. GPIO2 設為 input with internal pull-up。
3. 按下按鈕時產生 falling-edge interrupt。
4. GPIO ISR 不得執行：
   - Camera capture
   - Face detection
   - Memory allocation
   - JPEG encoding
   - BLE operation
   - Log formatting
5. ISR 僅可：
   - 呼叫 `vTaskNotifyGiveFromISR()`；或
   - 將最小事件寫入 ISR-safe queue。
6. Trigger task 收到事件後執行 40 ms software debounce。
7. Debounce 後重新讀取 GPIO2：
   - 若仍為 low，判定為有效按壓。
   - 若已為 high，視為雜訊或按鍵彈跳。
8. 一次完整按下與放開只能產生一個 CaptureRequest。
9. 必須等待按鈕放開後，才允許下一次按壓。
10. 長按不得重複產生 CaptureRequest。
11. 記錄 trigger source：

```text
EXTERNAL_BUTTON
```

12. 若系統不是 IDLE，不得排入新的影像處理工作，必須產生 BUSY event。
13. 按鈕事件與 BLE START_CAPTURE 必須使用同一套 CaptureRequest pipeline。

CaptureRequest 建議格式：

```cpp
enum class TriggerSource : uint8_t {
    ExternalButton = 1,
    BleClient = 2,
};

struct CaptureRequest {
    uint32_t request_id;
    TriggerSource trigger_source;
    int64_t created_at_us;
};
```

外接按鈕產生的 request ID 應由 ESP32-S3 自行生成，並與 BLE client 提供的 request ID 區隔。可使用不同的最高位或不同的 ID namespace，例如：

```text
ESP32-generated request ID：最高位為 1
Client-generated request ID：最高位為 0
```

## Trigger B：客戶端發起

PC 或未來手機透過 BLE Control Characteristic 發送：

```text
START_CAPTURE
```

必須包含：

- protocol version
- request_id
- command
- flags
- timestamp 或保留欄位

記錄 trigger source 為：

```text
BLE_CLIENT
```

## Trigger arbitration

同一時間只允許一個 capture pipeline。

若目前不是 IDLE，收到新 trigger 時必須回傳：

```text
BUSY
request_id
active_image_id
current_state
```

不得：

- 同時拍攝兩張影像
- 重複執行 detector
- 覆寫尚未傳完的 JPEG buffer
- 因連續按鍵或重複 BLE command 建立無限 queue

## 硬體觸發按鈕

保留開發板原有的 BOOT button，不得將 BOOT／GPIO0 作為應用程式的一般觸發按鈕。

外接觸發按鈕固定使用：

```text
XIAO pin：D1
ESP32-S3 GPIO：GPIO2
```

接線方式：

```text
D1／GPIO2 ─── momentary push button ─── GND
```

GPIO 設定：

```text
Direction：Input
Pull mode：Internal pull-up
Active level：Low
Interrupt edge：Falling edge
Software debounce：40 ms
```

集中設定於：

```text
components/board_support/include/board_config.hpp
```

建議定義：

```cpp
#pragma once

#include "driver/gpio.h"

namespace board {

inline constexpr gpio_num_t EXTERNAL_TRIGGER_GPIO = GPIO_NUM_2;
inline constexpr int EXTERNAL_TRIGGER_ACTIVE_LEVEL = 0;
inline constexpr uint32_t EXTERNAL_TRIGGER_DEBOUNCE_MS = 40;

}  // namespace board
```

選擇 GPIO2 的目的：

- 不占用 BOOT／GPIO0。
- 不使用 ESP32-S3 strapping pin。
- 不與 OV3660 camera pins 衝突。
- 不與 Sense expansion board 的 microSD pins 衝突。
- 不與數位麥克風 pins 衝突。
- 保留 D4／D5 作為未來 I2C。
- 保留 D6／D7 作為 UART。
- 保留 D8～D10 作為 SPI。

不得改用下列腳位作為預設外接觸發按鈕：

```text
GPIO0：
BOOT pin，會影響下載與開機模式。

GPIO3：
ESP32-S3 strapping pin，並且是 Sense microSD CS。

GPIO7、GPIO8、GPIO9：
Sense microSD 使用的 SPI pins。

GPIO10～GPIO18、GPIO38～GPIO40、GPIO47、GPIO48：
OV3660 camera 使用。

GPIO19、GPIO20：
原生 USB／USB Serial-JTAG 使用。

GPIO41、GPIO42：
Sense 數位麥克風使用。

GPIO43、GPIO44：
保留作為 UART TX／RX。
```


# 六、狀態機

至少包含：

```text
INITIALIZING
IDLE
CAPTURING
DETECTING
CROPPING
ENCODING
TRANSMITTING
WAITING_RESULT
COMPLETED
NO_FACE
ERROR_RECOVERY
```

每個狀態必須定義：

- 進入條件
- 執行動作
- timeout
- 成功轉移
- 失敗轉移
- 可否接受新的 trigger
- recovery 方法

狀態改變時輸出：

```text
[STATE] from=IDLE to=CAPTURING image_id=12 trigger=BLE_CLIENT
```

# 七、FreeRTOS 架構

至少規劃以下模組：

```text
SystemController
TriggerService
CameraService
FaceDetectionService
ImageCropService
JpegEncoder
BleTransport
ApplicationProtocol
RecognitionResultHandler
MetricsService
```

建議 task：

```text
system_controller_task
camera_pipeline_task
ble_event_task
ble_tx_task
metrics_task
```

不要為每一個很小的動作建立獨立 task。

必須明確處理：

- camera frame buffer ownership
- crop buffer ownership
- JPEG buffer ownership
- queue 中傳 pointer 或 value 的原則
- buffer 釋放責任
- timeout
- backpressure
- BLE 尚未傳送完成時禁止再次拍照
- 斷線時 buffer 如何回收
- watchdog
- PSRAM allocation
- internal RAM allocation
- DMA-capable buffer

大型影像 buffer 不得放在 task stack。

避免反覆配置與釋放大型記憶體。優先建立可重用 buffer 或 buffer pool。

# 八、Phase 1～6

所有 Phase 都必須一次實作完成，但每個 Phase 必須可以獨立 build、flash 和測試。

不要複製六份完整程式碼。共用功能放在 `components/`，各 Phase 只建立薄的測試 application。

建議結構：

```text
firmware/
├── components/
│   ├── board_support/
│   ├── camera_service/
│   ├── face_detection/
│   ├── image_processing/
│   ├── application_protocol/
│   ├── ble_transport/
│   ├── system_controller/
│   └── metrics/
├── apps/
│   ├── phase01_camera/
│   ├── phase02_trigger/
│   ├── phase03_face_pipeline/
│   ├── phase04_protocol/
│   ├── phase05_ble/
│   └── phase06_end_to_end/
└── tools/
```

## Phase 1：環境與相機

實作：

- ESP-IDF project
- XIAO ESP32-S3 Sense board config
- OV3660 初始化
- PSRAM 檢查
- Camera capture
- frame metadata log
- camera frame buffer 正確歸還
- heap 與 PSRAM log

提供：

- 測試程式
- build command
- flash command
- monitor command
- 測試項目
- 使用說明
- 預期輸出
- pass/fail 條件
- 結果紀錄表

## Phase 2：D1／GPIO2 外接按鈕 trigger、BLE client trigger 與狀態機

實作：

- GPIO2 按鈕 trigger
- debounce
- mock client trigger
- CaptureRequest queue
- trigger source
- busy rejection
- state machine
- duplicate request_id rejection

此 Phase 可以先使用 mock camera pipeline，不必執行完整 detector。

## Phase 3：固定人臉處理 Pipeline

整合：

- OV3660 capture
- `espdet_pico_224_224_face_s8_s3`
- 最大臉選擇
- 25% total margin
- boundary clamp
- crop
- JPEG encode

不進行：

- detector 比較
- margin 比較
- accuracy benchmark
- 不同解析度 benchmark

只提供必要 smoke test：

- 有臉
- 無臉
- 多臉時選最大臉
- JPEG 可解碼
- buffer 可正常回收

## Phase 4：共用應用層協定

本 Phase 不要求 D6/D7 實體 UART 硬體驗證。

不要要求 USB-to-UART adapter。

建立 transport-independent binary protocol，並在 PC 端使用 unit test、memory stream 或檔案 stream 驗證。

至少定義：

```text
Magic
Protocol version
Message type
Flags
Request ID
Image ID
Payload length
Chunk index
Total chunks
CRC32
Payload
```

訊息至少包含：

```text
HELLO
HELLO_ACK
START_CAPTURE
CAPTURE_ACCEPTED
BUSY
STATUS
IMAGE_BEGIN
IMAGE_CHUNK
IMAGE_END
RECOGNITION_RESULT
NO_FACE
ERROR
PING
PONG
```

測試：

- serialize／deserialize
- partial packet
- invalid magic
- unsupported version
- invalid length
- CRC mismatch
- duplicated chunk
- missing chunk
- out-of-order chunk
- image reassembly
- timeout
- unknown message type

Phase 5 的 BLE 必須直接重用此協定，不得重新建立另一套不相容格式。

## Phase 5：BLE GATT 與連線品質

優先使用 NimBLE，除非目前 ESP-IDF／ESP-WHO 相依版本有具體理由必須使用 Bluedroid。若改用 Bluedroid，必須寫入 ADR。

設計自訂 GATT Service。

至少包含：

### Control Characteristic

用途：

- PC／手機寫入 command

Properties：

```text
Write
Write Without Response
```

### Event Characteristic

用途：

- ESP32-S3 回報狀態與控制事件

Properties：

```text
Notify
Read
```

### Image Data Characteristic

用途：

- ESP32-S3 傳送 image chunks

Properties：

```text
Notify
```

### Recognition Result Characteristic

用途：

- PC／手機回傳辨識結果

Properties：

```text
Write
```

### Device Information Characteristic

用途：

- protocol version
- firmware version
- model name
- build ID
- maximum image size
- supported feature flags

Properties：

```text
Read
```

BLE 必須處理：

- advertising
- connect
- disconnect
- reconnect
- subscribe
- negotiated MTU
- chunk payload 計算
- sequence number
- image CRC32
- timeout
- congestion
- peer 未訂閱 notify
- 傳送途中斷線
- stale recognition result
- incorrect image_id
- application-level transfer completion

不要假設固定 MTU。

實際 chunk payload 必須根據 negotiated MTU 與協定 header 動態計算。

### BLE 連線品質

加入 RSSI 與 BLE 傳輸品質紀錄。

ESP32-S3 端與 PC 端在 API 可取得時都要記錄：

- RSSI
- negotiated MTU
- image bytes
- chunk count
- transfer duration
- application throughput
- missing sequence count
- duplicated sequence count
- CRC failure count
- disconnect count
- reconnect count
- transfer success/failure
- retry count
- queue congestion count

RSSI 不要每個 packet 都讀取。

預設每秒最多讀取一次，並記錄：

```text
rssi_current_dbm
rssi_min_dbm
rssi_max_dbm
rssi_mean_dbm
rssi_stddev_dbm
```

提供測試矩陣：

```text
距離：0.5 m、2 m、5 m
環境：無遮蔽、人體遮蔽、隔一道牆
影像大小：20 KB、50 KB、100 KB
重複次數：每個條件至少 10 次
```

這些測試由我手動執行。

請提供：

- 測試用 firmware app
- PC BLE test script
- 操作步驟
- 預期結果
- CSV output
- Markdown 結果模板
- pass/fail 判定
- 常見故障排除

## Phase 6：InsightFace 與 End-to-End

建立 Python PC client。

使用：

- Python
- Bleak
- InsightFace
- ONNX Runtime
- SQLite
- NumPy
- OpenCV
- pytest

建立 CLI：

```text
python -m pc_client register --person-id P001 --name "Alice" --image alice1.jpg
python -m pc_client register --person-id P001 --name "Alice" --image alice2.jpg
python -m pc_client list
python -m pc_client remove --person-id P001
python -m pc_client recognize-file --image test.jpg
python -m pc_client scan
python -m pc_client run --device-name ESP32S3-FACE-DEMO
python -m pc_client ble-test --device-name ESP32S3-FACE-DEMO
```

### 人臉註冊

註冊流程：

1. 讀取輸入照片。
2. 使用 InsightFace 偵測人臉與 landmark。
3. 預設要求剛好一張人臉。
4. 沒有臉時拒絕註冊。
5. 多張臉時預設拒絕註冊。
6. 提供可選參數允許選擇最大臉。
7. 執行 alignment。
8. 提取 embedding。
9. L2 normalize。
10. 儲存至 SQLite。
11. 一個 person 可有多個 embedding sample。
12. 儲存模型名稱與 embedding dimension。
13. 防止使用不相容模型產生的 embedding 混在同一資料庫。

SQLite 至少包含：

```text
persons
face_samples
settings
schema_version
```

原始註冊照片是否保存必須由設定決定，預設不保存。

### 人臉辨識

收到 ESP32-S3 傳送的 cropped JPEG 後：

1. 解碼 JPEG。
2. 使用 InsightFace 在 crop 中重新偵測 landmark。
3. 執行 alignment。
4. 提取 normalized embedding。
5. 使用 cosine similarity 比對資料庫。
6. threshold 必須可設定。
7. 回傳 top-1 結果。
8. 低於 threshold 時回傳 UNKNOWN。
9. 將結果透過 Recognition Result Characteristic 寫回 ESP32-S3。

結果至少包含：

```text
protocol_version
request_id
image_id
status
person_id
person_name
similarity
processing_time_ms
```

不得只使用人名作為唯一 ID。

### End-to-End 測試

測試兩個方向：

```text
A. 開發板按鈕發起
B. PC 發送 START_CAPTURE
```

兩者都必須完成：

```text
trigger
→ capture
→ detection
→ crop
→ JPEG
→ BLE transfer
→ InsightFace
→ result response
→ ESP32-S3 log
```

# 九、BLE Protocol 文件

額外產生：

```text
docs/BLE_PROTOCOL.md
protocol/ble_protocol.yaml
clients/ios/BLEProtocol.swift
```

## BLE_PROTOCOL.md

必須完整描述：

- service UUID
- characteristic UUID
- properties
- permissions
- command
- event
- packet layout
- integer endian
- string encoding
- maximum length
- optional field
- protocol version
- backward compatibility
- MTU handling
- chunk calculation
- state diagram
- timing diagram
- request_id
- image_id
- CRC32
- timeout
- disconnect behavior
- reconnect behavior
- error code
- security requirement
- sample packet
- PC flow
- iOS flow

## ble_protocol.yaml

它是 BLE protocol 的 single source of truth。

至少包含：

```text
protocol version
UUID
message type
command code
status code
error code
field
field type
byte order
field length
timeout
feature flag
```

韌體、Python 與 Swift 中的常數必須與此檔案一致。

若不建立 code generator，至少建立 consistency test，檢查三端 UUID、message code 與 error code 是否相同。

## BLEProtocol.swift

建立可直接加入未來 iOS Xcode project 的 Swift source file。

至少包含：

```text
CBUUID constants
MessageType enum
Command enum
StatusCode enum
ErrorCode enum
PacketHeader
RecognitionResult
packet encoder
packet decoder
little-endian helpers
CRC32
image chunk assembler
protocol validation error
```

不要建立完整 iOS UI。

Swift 檔案必須只依賴 Foundation 與 CoreBluetooth。

# 十、測試與結果紀錄

每一個 Phase 都建立：

```text
tests/phase01/
tests/phase02/
tests/phase03/
tests/phase04/
tests/phase05/
tests/phase06/
```

每個目錄至少包含：

```text
README.md
TEST_PLAN.md
RESULT_TEMPLATE.md
expected_output.txt
```

若適用，另外包含：

```text
run_test.ps1
run_test.py
pytest test
sample config
```

每個 TEST_PLAN 必須包含：

- 測試目的
- 前置條件
- 使用硬體
- 使用韌體
- build command
- flash command
- 執行步驟
- 收集資料
- 預期結果
- pass criteria
- fail criteria
- recovery
- 結果檔案位置

建立統一結果目錄：

```text
results/
├── phase01/
├── phase02/
├── phase03/
├── phase04/
├── phase05/
└── phase06/
```

測試程式應輸出 CSV 或 JSONL，避免只能人工閱讀 serial log。

統一 metrics 格式：

```text
[METRIC] phase=5 image_id=12 metric=ble_transfer_ms value=842 unit=ms
```

# 十一、Repository 結構

請建立類似：

```text
project/
├── README.md
├── ARCHITECTURE.md
├── DEPENDENCIES.md
├── DECISIONS.md
├── CMakeLists.txt
├── firmware/
│   ├── components/
│   └── apps/
├── pc_client/
│   ├── pyproject.toml
│   ├── requirements.lock
│   ├── src/
│   └── tests/
├── protocol/
│   └── ble_protocol.yaml
├── clients/
│   └── ios/
│       └── BLEProtocol.swift
├── docs/
│   ├── BLE_PROTOCOL.md
│   ├── TESTING.md
│   ├── TROUBLESHOOTING.md
│   └── PRIVACY.md
├── tests/
├── results/
└── tools/
    ├── idf.ps1
    ├── build_all.ps1
    └── create_python_venv.ps1
```

# 十二、Build 與環境操作

先檢查：

```powershell
idf.py --version
python --version
git --version
```

若目前 shell 沒有 ESP-IDF 環境，使用：

```powershell
. 'C:\Espressif\tools\Microsoft.v6.0.PowerShell_profile.ps1'
```

但不要假設 activation 能跨多次 shell command 永久保留。

請透過 `tools/idf.ps1` 封裝。

Python 端優先直接呼叫：

```powershell
.\pc_client\.venv\Scripts\python.exe
```

不要依賴每次先執行 Activate.ps1。

必須嘗試：

- configure
- build all firmware apps
- Python lint
- Python type check
- Python unit test
- Swift syntax consistency檢查，若本機沒有 Swift compiler則明確記錄為未執行
- protocol consistency test

沒有實體開發板時：

- 不要宣稱 flash 成功
- 不要宣稱 camera 成功
- 不要宣稱 BLE 成功
- 不要虛構 RSSI 或 throughput
- 建立人工測試步驟與空白結果模板

# 十三、錯誤處理

至少處理：

- Camera initialization failure
- PSRAM unavailable
- Frame capture failure
- Detector load failure
- No face
- Invalid bounding box
- Crop allocation failure
- JPEG encode failure
- BLE not connected
- Notify not subscribed
- BLE congestion
- BLE disconnect during transfer
- Image timeout
- Chunk missing
- CRC mismatch
- Unsupported protocol version
- Duplicate request
- Busy
- Recognition timeout
- Stale result
- Incorrect image_id
- InsightFace no face
- InsightFace multiple faces
- Empty database
- Database model mismatch
- Python BLE adapter unavailable

錯誤必須分為：

```text
recoverable
retryable
connection-reset-required
module-reinitialize-required
fatal
```

# 十四、安全與隱私

Demo 至少文件化：

- 是否要求 BLE pairing
- 是否要求 encryption
- 未配對 client 是否可寫入 Recognition Result
- 如何避免其他 client 偽造辨識結果
- 是否保存收到的 JPEG
- 是否保存註冊原始照片
- log 是否輸出人名
- SQLite 是否包含個人資料
- Demo 與未來正式產品的差異
- InsightFace pretrained model license 注意事項

Demo 可以提供開發模式，但安全設定不得散落在程式中，必須集中於設定檔。

# 十五、完成標準

Repository 完成時必須滿足：

1. Phase 1 到 Phase 6 的程式均已建立。
2. 每個 Phase 可獨立 build。
3. 共用程式放在 components，不複製整份邏輯。
4. ESP32-S3 端只使用指定 detector。
5. 固定使用總 margin 25%。
6. 支援開發板與 BLE client 兩種 trigger。
7. 不包含螢幕、麥克風或語音。
8. 不把實體 UART 測試列為必要驗收。
9. Phase 4 完成共用封包協定與 host unit test。
10. Phase 5 完成 BLE 與 RSSI 測試工具。
11. Phase 6 完成 InsightFace 註冊與辨識流程。
12. BLE protocol 有 Markdown、YAML 與 Swift 三種輸出。
13. 每個 Phase 有測試步驟與結果模板。
14. 所有可在本機完成的 build 與 unit test 都實際執行。
15. 所有未執行的硬體測試均清楚標示。
16. README 提供由 Phase 1 逐步測到 Phase 6 的完整順序。

完成後請輸出：

- 建立及修改的檔案列表
- 架構摘要
- 實際執行過的命令
- build 結果
- unit test 結果
- 未執行的硬體測試
- 已知風險
- 我應該從 Phase 1 開始執行的第一組命令