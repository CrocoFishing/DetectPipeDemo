# iOS：BLE 啟動 Wi-Fi SD 全量匯出

`clients/ios` 提供 iOS 16+ 的 Swift 套件與前景整合範例。App 先透過既有 BLE 連線啟動匯出，ESP32 等既有 Event／capture 自然結束後暫停錄音與人臉偵測，開啟單一 client 的 WPA2 SoftAP，iOS 加入後以 HTTP 下載所有一般檔案。下載完成、取消或閒置逾時後，裝置關閉熱點並恢復原流程。

完整二進位定義以 [BLE_PROTOCOL.md](BLE_PROTOCOL.md) 及 `protocol/ble_protocol.yaml` 為準；Event 欄位及歷史資料匯入請見 [EVENT_DATA_MODEL.md](EVENT_DATA_MODEL.md)。匯出保留原始相對目錄與 bytes，包含既有 WAV／CSV、未知檔案與子目錄。傳輸不刪除 SD 原始資料。

## 加入 App

以 local Swift Package 加入 `clients/ios`，產品為 `EventPipeSDExport`；或將 `BLEProtocol.swift`、`SDExportBLELink.swift`、`SDExportClient.swift`、`EventDataTypes.swift` 加入既有 App target。`SDExportExample.swift` 是需接到 App BLE owner／UI 的參考程式，沒有完整 App、掃描配對頁或 Xcode App project。

啟用 **Hotspot Configuration** capability，entitlement：

```xml
<key>com.apple.developer.networking.HotspotConfiguration</key>
<true/>
```

在 App `Info.plist` 加入說明字串及固定裝置 IP 的 ATS 設定；正式 App 可透過 `InfoPlist.strings` 在地化說明。

```xml
<key>NSBluetoothAlwaysUsageDescription</key>
<string>透過藍牙設定錄音裝置，並啟動 SD 資料下載。</string>
<key>NSLocalNetworkUsageDescription</key>
<string>連接錄音裝置的 Wi-Fi，下載 SD 卡上的錄音與事件資料。</string>
<key>NSAppTransportSecurity</key>
<dict>
    <key>NSAllowsLocalNetworking</key>
    <true/>
    <key>NSExceptionDomains</key>
    <dict>
        <key>192.168.4.1</key>
        <dict>
            <key>NSExceptionAllowsInsecureHTTPLoads</key>
            <true/>
        </dict>
    </dict>
</dict>
```

Apple 要求 App 取得使用者同意才能套用 Wi-Fi 設定；`NEHotspotConfigurationManager.apply` 可能顯示系統加入提示。區域網路存取另有系統授權。上述設定對應固定私有 IP 的 HTTP，不需要全面停用 ATS。iOS 17 起 numeric IP 的 ATS 行為有變，請保留這個 IP exception 並在最低及現行支援 iOS 版本驗證。[Apple Wi-Fi configuration](https://developer.apple.com/documentation/networkextension/wi-fi-configuration)、[Local Network Privacy](https://developer.apple.com/documentation/technotes/tn3179-understanding-local-network-privacy)、[NSAllowsLocalNetworking](https://developer.apple.com/documentation/bundleresources/information-property-list/nsapptransportsecurity/nsallowslocalnetworking)。

套件透過已知 SSID 加入及已知 IP 通訊，沒有讀取系統當前 SSID、Bonjour 搜尋或 multicast；整合範例沿用 App 已連線的 `CBPeripheral`。

## BLE 與前景控制

`SDExportBLELink` **不指派** `CBPeripheral.delegate`，也不建立第二個 `CBCentralManager`。既有 owner 在 main queue 上，將以下 delegate callbacks 轉送到同一個 link；Event/image/result 的原有處理仍由 App 接續處理：

| App callback | link method |
|---|---|
| `didDiscoverServices` | `didDiscoverServices(error:)` |
| `didDiscoverCharacteristicsFor` | `didDiscoverCharacteristics(for:error:)` |
| `didUpdateNotificationStateFor` | `didUpdateNotificationState(for:error:)` |
| `didUpdateValueFor` | `didUpdateValue(for:error:)` |
| `didWriteValueFor` | `didWriteValue(for:error:)` |
| `didModifyServices` | `didModifyServices(_:)` |
| central 的 `didDisconnectPeripheral` | `disconnected()` |

link 在呼叫時發現 service／characteristics，讀 Device Information 檢查 `WIFI_SD_EXPORT=0x40`，訂閱 Event，使用 Control `.withResponse`。`didModifyServices` 使舊 characteristic 失效，後續操作重新探索；不要繼續持有失效 handle。包內新操作使用非零且 bit31 清除的 request ID；App 應在同一 link 上序列化匯出操作，並避免與其他 Control writer 同時寫入。若 App 自行分配其他 client request ID，可將匯出整合進同一序列化 owner。

基本用法：

```swift
let link = SDExportBLELink(peripheral: connectedPeripheral)
let coordinator = SDExportCoordinator(link: link)
// App 的 delegate 先安裝上述轉送。
let task = Task { @MainActor in
    let result = try await coordinator.exportAll(to: stablePerDeviceDirectory) { update in
        Task { @MainActor in /* 更新下載 UI */ }
    }
    // result.filesDirectory 是原始 SD 檔案；manifestURL 保存這次完整清單。
    // 只有 deviceRestored=true 才可顯示「錄音已恢復」。
}
// 使用者取消或 UIApplication.didEnterBackgroundNotification：
task.cancel()
```

`SDExportExample.swift` 示範這些接點與每台裝置的持久目錄。App 的現有 BLE owner 應在匯出期間保持有效；不要讓 delegate／link 被釋放。這個前景流程使用 `joinOnce=true`；App 在背景超過 15 秒時，系統可能移除暫時網路設定。背景時取消目前 Task，保留下載資料；再次回到前景後呼叫同一流程重新匯出。套件不承諾背景持續傳輸。[Apple joinOnce](https://developer.apple.com/documentation/networkextension/nehotspotconfiguration/joinonce)。

流程順序：

1. START → PREPARING。最多等待 30 秒讓已接受 Event／capture 自然完成，期間禁止新工作；逾時回報錯誤並恢復 admission。
2. SD metadata 全部落盤、錄音資源安全暫停、HTTP／AP 可用後回報 READY。link 每秒 GET_STATUS 補救遺失通知；準備整體等待上限 50 秒。
3. 讀取唯讀 **SD Export Info** characteristic（`...1006`）的完整 JSON。CoreBluetooth `readValue` 處理長讀取；不要只截取第一個 ATT fragment。資料最多 512 bytes，在該 session 內不變。
4. iOS 套用臨時 WPA2 設定，取得使用者同意；HTTP authenticated status 成功才視為實際連上正確裝置。
5. 下載並驗證完整 manifest，再逐檔下載／續傳／校驗。下載過程每 20 秒 authenticated status heartbeat，涵蓋本機雜湊耗時。
6. 所有檔案驗證後 POST complete。伺服器先回覆 accepted，再關閉 HTTP／AP並恢復音訊。套件以 BLE 等待最多 20 秒確認 COMPLETED；資料已被接受但 BLE 無法確認恢復時，結果的 `deviceRestored=false`。

BLE 中斷不會立即使有效 HTTP session 失效。App 可完成當前 HTTP 下載；若無法明確取消，裝置會在 120 秒沒有有效活動後清理並恢復。裝置重開機後 session／token 失效，需重新 START。任何失敗或取消都保留本機已驗證檔案與部分下載；以相同 per-device directory 重試。

## 連線資訊與 HTTP 契約 v1

Info JSON 範例（帳密與 token 是占位值，實際由裝置每個 session 重新產生）：

```json
{"format_version":1,"session_id":"0123456789ABCDEF","ssid":"ESP32-SD-12345678","passphrase":"random-session-password","base_url":"http://192.168.4.1","token":"0123456789abcdef0123456789abcdef"}
```

`session_id` 是非零 16 位大寫 hex，與 Event ID 無關。所有 HTTP request 均需：

```http
Authorization: Bearer <token>
X-Export-Session: <16HEX session_id>
Accept-Encoding: identity
```

token／密碼只留在記憶體，不存入 journal、manifest 或 UI log。SSID／token／endpoint 由同一次 BLE session 取得；不要接受任意傳入的下載 host。套件限定 `http://192.168.4.1:80`。

| Endpoint | 方法／內容 |
|---|---|
| `/v1/export/status` | GET；JSON 含 `format_version`, `session_id`, numeric `state/error`, `manifest_ready`, `file_count`, `total_bytes`, `manifest_sha256`。 |
| `/v1/export/manifest` | GET；chunked UTF-8 NDJSON，逐檔 SHA-256／大小清單，具 terminal record。 |
| `/v1/export/file?path=<URL-encoded relative path>` | GET；原始檔 bytes，Content-Length／ETag，支援單段 `Range: bytes=<offset>-` 與 `If-Range`。 |
| `/v1/export/complete` | POST JSON；client 已驗證全部清單與檔案後提交。 |

manifest 每個 JSON record 以一個 LF 結束，record 類型：

| `type` | 必要欄位 |
|---|---|
| `header` | `format_version:1`, `session_id:"16HEX"`；第一列且僅一次。 |
| `progress` | `path`, `bytes_hashed`；長檔案雜湊每最多 2 秒的進度，可多次出現。 |
| `file` | `path`, `size`（unsigned 64-bit bytes）, `sha256`（64 位小寫 hex）, `etag`。 |
| `end` | `file_count`, `total_bytes`, `manifest_sha256`；最後一列且僅一次。 |

`manifest_sha256` 是**所有 file record 原始 UTF-8 bytes（每列含尾端 LF）依收到順序串接**的 SHA-256。header／progress／end 不計入；不要重新序列化 JSON 後計算。必須驗證 header session、所有路徑、file count、total bytes、digest，並收到完整 end；中途 HTTP EOF 不代表清單完整。空 SD 的 file count／total bytes 都為 0，digest 是空 bytes 的 SHA-256。

`etag` JSON 值包含 HTTP 雙引號，例如 `"\"abc123\""`。將解碼後值（`"abc123"`）原樣放入 `If-Range`，不要再次加引號。ETag 作用於該 session 與路徑；跨 session 重新取得新 ETag，再按 **path + size + SHA-256** 決定是否可續用既有內容。

續傳回應：206 必須有精確的 `Content-Range: bytes <offset>-<size-1>/<size>`、相同 ETag、正確剩餘大小。If-Range 不符時伺服器回完整 200；client 截斷舊部分檔案並從 0 寫入。401 表示認證失敗，409 表示 session 不符，416 表示 Range 不合法；這些情況不可把回應當成檔案存入。每個檔案最終皆比對 SHA-256，驗證成功後才換成正式檔名。

POST complete body：

```json
{"session_id":"0123456789ABCDEF","manifest_sha256":"<64 lowercase hex from end>","file_count":4,"total_bytes":12345}
```

成功為 HTTP 200 JSON `{"accepted":true,"session_id":"0123456789ABCDEF"}`。伺服器啟動或檔案 HTTP 傳送完畢都不代表 client 已完整接收；complete 是 client 驗證完畢的明確確認。若 AP 關閉時回覆遺失，套件接受同 session 的 BLE COMPLETED 作確認。

## 本機檔案與續傳

同一台裝置每次使用相同 App 私有目錄：

```text
<per-device directory>/files/<原始 SD 相對路徑>
<per-device directory>/.sd-export/manifest.ndjson
<per-device directory>/.sd-export/<path-hash>.part
<per-device directory>/.sd-export/<path-hash>.json
```

journal 僅保存 path／size／SHA-256，實際 offset 取部分檔案長度。重新 START 後，只有內容 identity 一致才沿用 partial；已完成檔案也實際重新雜湊確認，不能只信任 journal。CSV 通常在恢復錄音後增長，內容不符時完整重取；未變的舊 WAV 可略過或續傳。

`files` 保留既有本機資料；**這次 manifest 才是當前快照清單**。若來源卡曾刪檔，舊本機檔案可能仍在目錄，匯入 App 時按 manifest 選取；不要把未列入的歷史快取誤認為此次資料。內部 `.sd-export` 與 raw files 分開，避免 SD 上同名檔案覆寫 journal。所有 path 都驗證為相對路徑，禁止穿越、絕對路徑、空／dot components、backslash、控制字元及大小寫／Unicode 正規化後的目的檔名碰撞。

套件逐串流寫入原始 bytes，單檔使用固定 64 KiB buffer，本機 SHA-256 以 512 KiB 片段計算；manifest 的檔案 metadata 留在記憶體，沒有一次載入所有 SD 檔案內容。網路暫時錯誤每檔最多重試 3 次，保留已寫入部分；更長中斷由下一次前景操作重新取得 session。

## 驗證與已知限制

韌體限制相對路徑最多 1024 個 UTF-8 bytes、16 層目錄、單一檔名最多 255 個 UTF-8 bytes（目前 FatFS 的輸出 buffer）。遇到不支援的名稱、短檔名 fallback 或讀取錯誤會讓整次匯出失敗，避免漏檔／改名。原生 FatFS 讀取可處理超過 2 GiB 的 FAT32 檔案；檔案大小仍受卡片檔案系統上限限制。

`clients/ios/Tests/SDExportContractTests.swift` 涵蓋 exact NDJSON digest、progress 排除、空卡、缺少 end／LF、錯誤 digest／session、檔名碰撞／穿越與 Event ID 精度。使用 macOS／Xcode 在 iOS Simulator 執行套件 tests，並將 `SDExportExample.swift` 加入 App target 檢查 delegate 整合。從 `clients/ios` package 目錄執行（scheme 以 `-list` 的實際輸出為準，UUID 換成本機可用 simulator）：

```sh
xcodebuild -list
xcrun simctl list devices available
xcodebuild -scheme EventPipeSDExport -destination 'generic/platform=iOS Simulator' build
xcodebuild -scheme EventPipeSDExport -destination 'platform=iOS Simulator,id=<SIMULATOR-UUID>' test
```

也可在 Xcode 開啟這個 package，選 iOS Simulator 後執行 Test。一般 macOS destination 的 `swift test` 無法驗證這個含 UIKit／NEHotspotConfiguration 的 iOS target。

Windows 上的 Python/C++ protocol tests 不能證明 Swift 編譯或 iPhone 的 Wi-Fi 加入成功。真機驗收需覆蓋：iOS 實際 MTU、長讀取、系統拒絕加入／區網授權、無網際網路的 SoftAP、空卡／多層目錄／大 WAV、manifest 中斷、下載途中 BLE／Wi-Fi 中斷、離開前景與 App 重啟、改變 CSV 後重試、完成／取消後再次錄音，以及音訊恢復失敗的顯示。沒有承諾固定傳輸速率；ESP32-S3 的 SoftAP 與 BLE 共用射頻，需真機量測。[Espressif coexistence](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/coexist.html)。
