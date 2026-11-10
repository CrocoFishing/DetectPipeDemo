#include "wifi_sd_export_service.hpp"
#include "sd_export_core.hpp"

#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "ff.h"
#include "lwip/sockets.h"
#include "psa/crypto.h"

namespace demo {
namespace {
constexpr char TAG[] = "wifi_sd_export";
constexpr int64_t IDLE_TIMEOUT_US = 120'000'000;
constexpr int64_t PROGRESS_INTERVAL_US = 2'000'000;
constexpr size_t IO_BUFFER_SIZE = 16 * 1024;
using protocol::ErrorCode;
using protocol::MessageType;
using protocol::SdExportState;
using protocol::SdExportStatusPayload;

void log_memory(const char* phase) {
    const uint32_t internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const uint32_t dma = MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA;
    ESP_LOGI(TAG, "heap phase=%s internal_free=%u internal_largest=%u dma_free=%u dma_largest=%u",
        phase, static_cast<unsigned>(heap_caps_get_free_size(internal)),
        static_cast<unsigned>(heap_caps_get_largest_free_block(internal)),
        static_cast<unsigned>(heap_caps_get_free_size(dma)),
        static_cast<unsigned>(heap_caps_get_largest_free_block(dma)));
}

std::string hex_bytes(const uint8_t* bytes, size_t length) {
    constexpr char alphabet[] = "0123456789abcdef";
    std::string result(length * 2, '0');
    for (size_t i = 0; i < length; ++i) {
        result[i * 2] = alphabet[bytes[i] >> 4];
        result[i * 2 + 1] = alphabet[bytes[i] & 15];
    }
    return result;
}
std::string session_text(uint64_t id) {
    char text[17]; std::snprintf(text, sizeof(text), "%016" PRIX64, id);
    return text;
}
bool active_state(SdExportState state) {
    return state == SdExportState::Preparing || state == SdExportState::Ready ||
        state == SdExportState::Transferring || state == SdExportState::Restoring;
}
bool constant_equal(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned diff = 0;
    for (size_t i = 0; i < a.size(); ++i) diff |= static_cast<uint8_t>(a[i] ^ b[i]);
    return diff == 0;
}
esp_err_t response(httpd_req_t* request, const char* status, const std::string& body) {
    httpd_resp_set_status(request, status);
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, body.data(), static_cast<ssize_t>(body.size()));
}
std::string request_header(httpd_req_t* request, const char* name, size_t maximum) {
    size_t size = httpd_req_get_hdr_value_len(request, name);
    if (!size || size > maximum) return {};
    std::string text(size + 1, '\0');
    if (httpd_req_get_hdr_value_str(request, name, text.data(), text.size()) != ESP_OK) return {};
    text.resize(size); return text;
}
// Native FatFS FSIZE_t is unsigned32 for FAT32. POSIX off_t is signed32 in this
// IDF build, so fopen/stat/fseeko cannot describe the upper half of a FAT32file.
static_assert(std::numeric_limits<FSIZE_t>::max() >= UINT32_MAX, "FAT32 export requires unsigned 32-bit offsets");
static_assert(FF_LFN_UNICODE == 2, "SD export requires UTF-8 FatFS APIs");
struct FileCloser {
    FIL value{}; bool opened{false};
    ~FileCloser() { if (opened) (void)f_close(&value); }
};
struct DirCloser {
    FF_DIR value{}; bool opened{false};
    ~DirCloser() { if (opened) (void)f_closedir(&value); }
};
struct Sha256 {
    psa_hash_operation_t context = PSA_HASH_OPERATION_INIT;
    bool good{true};
    Sha256() { good = psa_crypto_init() == PSA_SUCCESS && psa_hash_setup(&context, PSA_ALG_SHA_256) == PSA_SUCCESS; }
    ~Sha256() { (void)psa_hash_abort(&context); }
    bool update(const void* data, size_t size) {
        good = good && psa_hash_update(&context, static_cast<const uint8_t*>(data), size) == PSA_SUCCESS;
        return good;
    }
    std::string finish() {
        uint8_t hash[32];
        size_t length = 0;
        if (!good || psa_hash_finish(&context, hash, sizeof(hash), &length) != PSA_SUCCESS || length != sizeof(hash)) return {};
        return hex_bytes(hash, sizeof(hash));
    }
};
}  // namespace

struct WifiSdExportService::State {
    struct Command { MessageType type; uint32_t request_id; uint64_t session_id; };
    WifiSdExportHooks hooks;
    QueueHandle_t commands{nullptr};
    TaskHandle_t worker{nullptr};
    mutable portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
    SdExportStatusPayload status{};
    uint32_t start_request{0};
    int64_t last_activity{0};
    char info[512]{};
    size_t info_length{0};
    std::atomic<bool> cancel_requested{false}, stop_readers{false}, complete_requested{false};
    std::atomic<bool> announce_pending{false};
    std::atomic<uint16_t> fatal_error{0};
    std::atomic<int> active_socket{-1};
    bool preparation_attempted{false}, resume_pending{false};
    bool network_stop_pending{false};
    SdExportState pending_terminal{SdExportState::Failed};
    ErrorCode pending_error{ErrorCode::None};
    bool wifi_initialized{false}, wifi_started{false};
    esp_netif_t* netif{nullptr};
    httpd_handle_t server{nullptr};
    std::string ssid, passphrase, token;
    uint8_t* io_buffer{nullptr};
    bool manifest_ready{false}, receipt_accepted{false};
    uint64_t file_count{0}, total_bytes{0};
    std::string manifest_hash;
    char drive[8]{};
    FILINFO directory_entry{};

    std::string native_path(const std::string& relative) const { return std::string(drive) + '/' + relative; }

    SdExportStatusPayload snapshot() const {
        portENTER_CRITICAL(&lock); auto value = status; portEXIT_CRITICAL(&lock); return value;
    }
    void activity() {
        const int64_t now = esp_timer_get_time();
        portENTER_CRITICAL(&lock); last_activity = now; portEXIT_CRITICAL(&lock);
    }
    void change(SdExportState state, ErrorCode error = ErrorCode::None) {
        portENTER_CRITICAL(&lock);
        status.state = state; status.error = error;
        if (state != SdExportState::Ready && state != SdExportState::Transferring) info_length = 0;
        portEXIT_CRITICAL(&lock);
        announce_pending.store(true);
    }
    void notify(uint32_t request_id, ErrorCode error = ErrorCode::None) {
        auto value = snapshot();
        if (error != ErrorCode::None) value.error = error;
        (void)hooks.notify(hooks.context, request_id, value);
    }
    void announce() {
        if (announce_pending.exchange(false)) notify(start_request);
    }
    static bool cancelled(void* context) {
        return static_cast<State*>(context)->cancel_requested.load();
    }
    bool interrupted() const { return cancel_requested.load() || stop_readers.load(); }
    void fail(ErrorCode error) {
        uint16_t none = 0;
        fatal_error.compare_exchange_strong(none, static_cast<uint16_t>(error));
        stop_readers.store(true);
    }
    void mark_transfer() {
        portENTER_CRITICAL(&lock);
        if (status.state == SdExportState::Ready) {
            status.state = SdExportState::Transferring;
            announce_pending.store(true);
        }
        portEXIT_CRITICAL(&lock);
    }
    bool authorize(httpd_req_t* request) {
        if (interrupted()) { response(request, "409 Conflict", "{\"error\":\"session_closing\"}"); return false; }
        if (!constant_equal(request_header(request, "Authorization", 80), "Bearer " + token)) {
            response(request, "401 Unauthorized", "{\"error\":\"unauthorized\"}"); return false;
        }
        if (request_header(request, "X-Export-Session", 16) != session_text(snapshot().session_id)) {
            response(request, "409 Conflict", "{\"error\":\"invalid_session\"}"); return false;
        }
        activity(); return true;
    }
    std::string etag(const std::string& path) const {
        Sha256 hash;
        auto id = session_text(snapshot().session_id);
        hash.update(id.data(), id.size()); hash.update("/", 1); hash.update(path.data(), path.size());
        return '"' + hash.finish() + '"';
    }

    esp_err_t start_network() {
        if (esp_netif_init() != ESP_OK) return ESP_FAIL;
        esp_err_t error = esp_event_loop_create_default();
        if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) return error;
        // Unlike create_default_wifi_ap(), these APIs return allocation errors
        // instead of aborting when the integration is under memory pressure.
        esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_WIFI_AP();
        netif = esp_netif_new(&netif_config);
        if (!netif) return ESP_ERR_NO_MEM;
        error = esp_netif_attach_wifi_ap(netif);
        if (error != ESP_OK) return error;
        error = esp_wifi_set_default_wifi_ap_handlers();
        if (error != ESP_OK) return error;
        wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
        error = esp_wifi_init(&init);
        if (error != ESP_OK) return error;
        wifi_initialized = true;
        if ((error = esp_wifi_set_storage(WIFI_STORAGE_RAM)) != ESP_OK ||
            (error = esp_wifi_set_mode(WIFI_MODE_AP)) != ESP_OK) return error;
        wifi_config_t config{};
        std::memcpy(config.ap.ssid, ssid.data(), ssid.size());
        config.ap.ssid_len = static_cast<uint8_t>(ssid.size());
        std::memcpy(config.ap.password, passphrase.data(), passphrase.size());
        config.ap.channel = 6; config.ap.max_connection = 1;
        config.ap.authmode = WIFI_AUTH_WPA2_PSK;
        config.ap.pmf_cfg.capable = true; config.ap.pmf_cfg.required = false;
        if ((error = esp_wifi_set_config(WIFI_IF_AP, &config)) != ESP_OK ||
            (error = esp_wifi_start()) != ESP_OK) return error;
        wifi_started = true;
        io_buffer = static_cast<uint8_t*>(heap_caps_malloc(IO_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!io_buffer) io_buffer = static_cast<uint8_t*>(heap_caps_malloc(IO_BUFFER_SIZE, MALLOC_CAP_8BIT));
        if (!io_buffer) return ESP_ERR_NO_MEM;
        httpd_config_t http = HTTPD_DEFAULT_CONFIG();
        http.stack_size = 8192; http.max_open_sockets = 2;
#if defined(CONFIG_SPIRAM) && defined(CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM)
        // This task only serves the paused SD snapshot; it never writes flash.
        // Keep its stack (including the FatFS FIL sector cache) out of the
        // fragmented internal heap needed by SD's DMA bounce allocations.
        http.task_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
#endif
        http.max_uri_handlers = 5; http.max_uri_len = sd_export::MAX_PATH_BYTES * 3 + 128;
        http.max_req_hdr_len = 1024; http.recv_wait_timeout = 2; http.send_wait_timeout = 2;
        http.lru_purge_enable = true;
        if ((error = httpd_start(&server, &http)) != ESP_OK) {
            ESP_LOGE(TAG, "HTTP start failed: %s", esp_err_to_name(error));
            log_memory("http_start_failed");
            return error;
        }
        struct Route { const char* uri; httpd_method_t method; };
        constexpr Route routes[] = {{"/v1/export/status", HTTP_GET}, {"/v1/export/manifest", HTTP_GET},
            {"/v1/export/file", HTTP_GET}, {"/v1/export/file", HTTP_DELETE}, {"/v1/export/complete", HTTP_POST}};
        for (const auto& route : routes) {
            httpd_uri_t handler{};
            handler.uri = route.uri; handler.method = route.method;
            handler.handler = handle_request; handler.user_ctx = this;
            if ((error = httpd_register_uri_handler(server, &handler)) != ESP_OK) return error;
        }
        log_memory("network_ready");
        return ESP_OK;
    }
    esp_err_t stop_network() {
        stop_readers.store(true);
        int fd = active_socket.load();
        if (fd >= 0) shutdown(fd, SHUT_RDWR);
        // Joining the synchronous HTTP worker closes every FILE/DIR before the
        // recorder is resumed. Readers must never survive snapshot ownership.
        if (server) {
            const esp_err_t error = httpd_stop(server);
            // A failed shutdown still owns its socket, handlers and SD reader.
            // Retain the handle/buffer and retry before restoring recording.
            if (error != ESP_OK) return error;
            server = nullptr;
        }
        active_socket.store(-1);
        if (wifi_started) { (void)esp_wifi_stop(); wifi_started = false; }
        if (wifi_initialized) { (void)esp_wifi_deinit(); wifi_initialized = false; }
        if (netif) { esp_netif_destroy_default_wifi(netif); netif = nullptr; }
        heap_caps_free(io_buffer); io_buffer = nullptr;
        return ESP_OK;
    }
    void cleanup(SdExportState terminal, ErrorCode error) {
        if (!network_stop_pending) {
            pending_terminal = terminal; pending_error = error;
            change(SdExportState::Restoring, error); announce();
        }
        const esp_err_t stopped = stop_network();
        if (stopped != ESP_OK) {
            if (!network_stop_pending) {
                ESP_LOGE(TAG, "HTTP stop failed: %s; retaining snapshot until retry", esp_err_to_name(stopped));
                log_memory("http_stop_failed");
                change(SdExportState::Restoring, ErrorCode::Internal); announce();
            }
            network_stop_pending = true;
            return;
        }
        network_stop_pending = false;
        terminal = pending_terminal; error = pending_error;
        if (preparation_attempted) {
            resume_pending = hooks.resume(hooks.context) != ESP_OK;
            if (resume_pending) { terminal = SdExportState::Failed; error = ErrorCode::AudioResumeFailed; }
            else preparation_attempted = false;
        }
        change(terminal, error); announce();
        // Credentials are unavailable over BLE as soon as restoration starts.
        // Keep receipt counters only for a terminal BLE query; no files change.
        token.clear(); passphrase.clear();
    }
    void start(uint32_t request_id) {
        if (active_state(snapshot().state) || resume_pending) { notify(request_id, ErrorCode::ExportBusy); return; }
        uint64_t id = 0;
        do { esp_fill_random(&id, sizeof(id)); } while (!id || id == snapshot().session_id);
        stop_readers.store(false); complete_requested.store(false);
        fatal_error.store(0); manifest_ready = false; receipt_accepted = false;
        file_count = total_bytes = 0; manifest_hash.clear(); start_request = request_id;
        // Serialize session replacement with the matching-session CANCEL flag.
        // A delayed cancellation for the previous session must not cancel this
        // session between its flag reset and publication of the new ID.
        portENTER_CRITICAL(&lock);
        cancel_requested.store(false); status.session_id = id;
        status.state = SdExportState::Preparing; status.error = ErrorCode::None;
        info_length = 0;
        portEXIT_CRITICAL(&lock);
        announce_pending.store(true); announce();
        preparation_attempted = true;
        esp_err_t error = hooks.prepare(hooks.context, 30'000, cancelled, this);
        if (cancel_requested.load()) { cleanup(SdExportState::Cancelled, ErrorCode::None); return; }
        if (error != ESP_OK) {
            ErrorCode code = error == ESP_ERR_TIMEOUT ? ErrorCode::QuiesceTimeout :
                error == ESP_ERR_NOT_FOUND ? ErrorCode::SdUnavailable : ErrorCode::SdIo;
            cleanup(SdExportState::Failed, code); return;
        }
        if (hooks.storage_drive(hooks.context, drive, sizeof(drive)) != ESP_OK || !drive[0]) {
            cleanup(SdExportState::Failed, ErrorCode::SdUnavailable); return;
        }
        uint8_t random[32]; esp_fill_random(random, sizeof(random)); token = hex_bytes(random, sizeof(random));
        esp_fill_random(random, sizeof(random)); passphrase = hex_bytes(random, 12);
        ssid = "EventPipe-" + hex_bytes(random + 12, 4);
        if ((error = start_network()) != ESP_OK) {
            ESP_LOGE(TAG, "network start failed: %s", esp_err_to_name(error));
            log_memory("network_start_failed");
            cleanup(SdExportState::Failed, ErrorCode::WifiStartFailed); return;
        }
        if (cancel_requested.load()) { cleanup(SdExportState::Cancelled, ErrorCode::None); return; }
        const std::string json = "{\"format_version\":1,\"session_id\":" +
            sd_export::json_string(session_text(id)) + ",\"ssid\":" + sd_export::json_string(ssid) +
            ",\"passphrase\":" + sd_export::json_string(passphrase) +
            ",\"base_url\":\"http://192.168.4.1\",\"token\":" + sd_export::json_string(token) + "}";
        if (json.size() > sizeof(info)) { cleanup(SdExportState::Failed, ErrorCode::Internal); return; }
        portENTER_CRITICAL(&lock);
        std::memcpy(info, json.data(), json.size()); info_length = json.size();
        status.state = SdExportState::Ready; status.error = ErrorCode::None;
        portEXIT_CRITICAL(&lock);
        activity(); announce_pending.store(true); announce();
        ESP_LOGI(TAG, "export ready session=%s", session_text(id).c_str());
    }
    void process(const Command& command) {
        auto current = snapshot();
        if (command.type == MessageType::SdExportStart) { start(command.request_id); return; }
        if (command.type == MessageType::SdExportGetStatus) {
            notify(command.request_id, command.session_id && command.session_id != current.session_id ?
                ErrorCode::InvalidSession : ErrorCode::None); return;
        }
        if (command.session_id != current.session_id || !command.session_id) {
            notify(command.request_id, ErrorCode::InvalidSession); return;
        }
        if (resume_pending) {
            cleanup(SdExportState::Cancelled, ErrorCode::None); notify(command.request_id); return;
        }
        if (active_state(current.state)) cancel_requested.store(true);
        // A terminal session cancellation is idempotent and never changes an
        // already completed or failed receipt into a different terminal.
        notify(command.request_id);
    }
    static void task_entry(void* context) { static_cast<State*>(context)->run(); }
    void run() {
        for (;;) {
            Command command{};
            if (xQueueReceive(commands, &command, pdMS_TO_TICKS(100)) == pdTRUE) process(command);
            announce();
            if (network_stop_pending) {
                cleanup(pending_terminal, pending_error);
                continue;
            }
            auto current = snapshot();
            if (current.state != SdExportState::Ready && current.state != SdExportState::Transferring) continue;
            if (cancel_requested.load()) { cleanup(SdExportState::Cancelled, ErrorCode::None); continue; }
            if (uint16_t error = fatal_error.load()) {
                cleanup(SdExportState::Failed, static_cast<ErrorCode>(error)); continue;
            }
            if (complete_requested.load()) { cleanup(SdExportState::Completed, ErrorCode::None); continue; }
            portENTER_CRITICAL(&lock); int64_t touched = last_activity; portEXIT_CRITICAL(&lock);
            if (esp_timer_get_time() - touched >= IDLE_TIMEOUT_US) cleanup(SdExportState::Failed, ErrorCode::ExportExpired);
        }
    }

    static esp_err_t handle_request(httpd_req_t* request) {
        auto& self = *static_cast<State*>(request->user_ctx);
        self.active_socket.store(httpd_req_to_sockfd(request));
        esp_err_t error = ESP_FAIL;
        if (self.authorize(request)) {
            if (request->method == HTTP_DELETE) error = self.delete_file(request);
            else if (request->method == HTTP_POST) error = self.complete(request);
            else if (std::strncmp(request->uri, "/v1/export/status", 17) == 0) error = self.http_status(request);
            else if (std::strncmp(request->uri, "/v1/export/manifest", 19) == 0) error = self.manifest(request);
            else error = self.file(request);
        }
        self.active_socket.store(-1);
        return error;
    }
    esp_err_t http_status(httpd_req_t* request) {
        const auto value = snapshot();
        return response(request, "200 OK", "{\"format_version\":1,\"session_id\":" +
            sd_export::json_string(session_text(value.session_id)) + ",\"state\":" +
            std::to_string(static_cast<unsigned>(value.state)) + ",\"error\":" +
            std::to_string(static_cast<unsigned>(value.error)) + ",\"manifest_ready\":" +
            (manifest_ready ? "true" : "false") + ",\"file_count\":" + std::to_string(file_count) +
            ",\"total_bytes\":" + std::to_string(total_bytes) + ",\"manifest_sha256\":" +
            sd_export::json_string(manifest_hash) + "}");
    }
    bool chunk(httpd_req_t* request, const std::string& text) {
        if (interrupted() || httpd_resp_send_chunk(request, text.data(), static_cast<ssize_t>(text.size())) != ESP_OK) return false;
        activity(); return true;
    }
    bool read_file(FIL& source, size_t wanted, UINT& count) {
        const FRESULT result = f_read(&source, io_buffer, static_cast<UINT>(wanted), &count);
        if (result == FR_OK && count == wanted) return true;
        ESP_LOGE(TAG, "SD read failed: fatfs=%u requested=%u received=%u",
            static_cast<unsigned>(result), static_cast<unsigned>(wanted), static_cast<unsigned>(count));
        log_memory("sd_read_failed");
        fail(ErrorCode::SdIo);
        return false;
    }
    bool hash_file(httpd_req_t* request, const std::string& path, uint64_t size, std::string& digest) {
        FileCloser file;
        if (f_open(&file.value, native_path(path).c_str(), FA_READ) != FR_OK) { fail(ErrorCode::SdIo); return false; }
        file.opened = true;
        if (static_cast<uint64_t>(f_size(&file.value)) != size) { fail(ErrorCode::SdIo); return false; }
        Sha256 hash; uint64_t read = 0; int64_t progress_at = esp_timer_get_time();
        while (read < size) {
            if (interrupted()) return false;
            const size_t wanted = static_cast<size_t>(size - read > IO_BUFFER_SIZE ? IO_BUFFER_SIZE : size - read);
            UINT bytes = 0;
            if (!read_file(file.value, wanted, bytes)) return false;
            if (!hash.update(io_buffer, bytes)) { fail(ErrorCode::Internal); return false; }
            read += bytes; activity();
            int64_t now = esp_timer_get_time();
            if (now - progress_at >= PROGRESS_INTERVAL_US) {
                if (!chunk(request, "{\"type\":\"progress\",\"path\":" + sd_export::json_string(path) +
                    ",\"bytes_hashed\":" + std::to_string(read) + "}\n")) return false;
                progress_at = now;
            }
            taskYIELD();
        }
        if (static_cast<uint64_t>(f_tell(&file.value)) != size) { fail(ErrorCode::SdIo); return false; }
        digest = hash.finish();
        if (digest.empty()) { fail(ErrorCode::Internal); return false; }
        return true;
    }
    bool walk(httpd_req_t* request, const std::string& relative, size_t depth,
              Sha256& manifest_digest, uint64_t& files, uint64_t& bytes) {
        if (interrupted()) return false;
        DirCloser directory;
        if (f_opendir(&directory.value, native_path(relative).c_str()) != FR_OK) { fail(ErrorCode::SdIo); return false; }
        directory.opened = true;
        for (;;) {
            if (interrupted()) return false;
            if (f_readdir(&directory.value, &directory_entry) != FR_OK) { fail(ErrorCode::SdIo); return false; }
            if (!directory_entry.fname[0]) break;
            const std::string name = directory_entry.fname;
            if (name == "." || name == "..") continue;
#if FF_USE_LFN
            // IDF6 FatFS falls back to an SFN alias if its255-byte UTF-8 LFN
            // output buffer overflows. Refuse that fallback rather than export
            // renamed paths. blk_ofs records whether the entry carried an LFN;
            // a valid LFN always retains a separate nonempty altname.
            if (directory.value.blk_ofs != UINT32_MAX && !directory_entry.altname[0]) {
                fail(ErrorCode::PathUnsupported); return false;
            }
#endif
            const std::string path = relative.empty() ? name : relative + '/' + name;
            if (!sd_export::valid_relative_path(path)) { fail(ErrorCode::PathUnsupported); return false; }
            uint64_t size = static_cast<uint64_t>(directory_entry.fsize);
            const bool is_directory = (directory_entry.fattrib & AM_DIR) != 0;
            activity();
            if (is_directory) {
                if (depth >= sd_export::MAX_DIRECTORY_DEPTH) { fail(ErrorCode::PathUnsupported); return false; }
                if (!walk(request, path, depth + 1, manifest_digest, files, bytes)) return false;
            } else {
                if (files == UINT64_MAX || bytes > UINT64_MAX - size) { fail(ErrorCode::PathUnsupported); return false; }
                std::string hash;
                if (!hash_file(request, path, size, hash)) return false;
                const std::string record = sd_export::file_record(path, size, hash, etag(path));
                if (!manifest_digest.update(record.data(), record.size())) { fail(ErrorCode::Internal); return false; }
                if (!chunk(request, record)) return false;
                ++files; bytes += size;
            }
        }
        return true;
    }
    esp_err_t manifest(httpd_req_t* request) {
        manifest_ready = false;
        mark_transfer();
        httpd_resp_set_type(request, "application/x-ndjson; charset=utf-8");
        httpd_resp_set_hdr(request, "Cache-Control", "no-store");
        if (!chunk(request, "{\"type\":\"header\",\"format_version\":1,\"session_id\":" +
            sd_export::json_string(session_text(snapshot().session_id)) + "}\n")) return ESP_FAIL;
        Sha256 hash; uint64_t files = 0, bytes = 0;
        if (!walk(request, "", 0, hash, files, bytes)) return ESP_FAIL;
        std::string digest = hash.finish();
        if (digest.empty()) { fail(ErrorCode::Internal); return ESP_FAIL; }
        manifest_hash = digest; file_count = files; total_bytes = bytes; manifest_ready = true;
        if (!chunk(request, "{\"type\":\"end\",\"file_count\":" + std::to_string(files) +
            ",\"total_bytes\":" + std::to_string(bytes) + ",\"manifest_sha256\":" +
            sd_export::json_string(digest) + "}\n")) return ESP_FAIL;
        return httpd_resp_send_chunk(request, nullptr, 0);
    }
    bool file_path(httpd_req_t* request, std::string& relative) {
        size_t query_size = httpd_req_get_url_query_len(request);
        if (!query_size || query_size > sd_export::MAX_PATH_BYTES * 3 + 5)
            { response(request, "400 Bad Request", "{\"error\":\"invalid_path\"}"); return false; }
        std::string query(query_size + 1, '\0');
        if (httpd_req_get_url_query_str(request, query.data(), query.size()) != ESP_OK)
            { response(request, "400 Bad Request", "{\"error\":\"invalid_path\"}"); return false; }
        query.resize(query_size);
        if (query.compare(0, 5, "path=") != 0 || query.find('&') != std::string::npos)
            { response(request, "400 Bad Request", "{\"error\":\"invalid_path\"}"); return false; }
        if (!sd_export::decode_path(std::string_view(query).substr(5), relative))
            { response(request, "400 Bad Request", "{\"error\":\"invalid_path\"}"); return false; }
        return true;
    }
    esp_err_t delete_file(httpd_req_t* request) {
        if (complete_requested.load() || receipt_accepted)
            return response(request, "409 Conflict", "{\"error\":\"session_closing\"}");
        if (request->content_len)
            return response(request, "400 Bad Request", "{\"error\":\"unexpected_body\"}");
        std::string relative;
        if (!file_path(request, relative)) return ESP_OK;
        const auto match = request_header(request, "If-Match", 128);
        const std::string absolute = native_path(relative);
        bool unlink_attempted = false;
        const auto result = sd_export::delete_regular_file(relative, match, etag(relative), [&] {
            FILINFO info{};
            const auto stat = f_stat(absolute.c_str(), &info);
            if (stat == FR_NO_FILE || stat == FR_NO_PATH) return sd_export::FileKind::Missing;
            if (stat != FR_OK) return sd_export::FileKind::Error;
            return (info.fattrib & AM_DIR) ? sd_export::FileKind::Directory : sd_export::FileKind::Regular;
        }, [&] {
            unlink_attempted = true;
            return f_unlink(absolute.c_str()) == FR_OK;
        });
        // A failed unlink may have partially changed the filesystem too.
        if (unlink_attempted) {
            manifest_ready = false;
            manifest_hash.clear(); file_count = total_bytes = 0;
        }
        using sd_export::DeleteResult;
        switch (result) {
            case DeleteResult::InvalidPath:
                return response(request, "400 Bad Request", "{\"error\":\"invalid_path\"}");
            case DeleteResult::MatchRequired:
                return response(request, "428 Precondition Required", "{\"error\":\"if_match_required\"}");
            case DeleteResult::MatchFailed:
                return response(request, "412 Precondition Failed", "{\"error\":\"etag_mismatch\"}");
            case DeleteResult::Missing:
                return response(request, "404 Not Found", "{\"error\":\"not_file\"}");
            case DeleteResult::NotFile:
                return response(request, "409 Conflict", "{\"error\":\"not_file\"}");
            case DeleteResult::IoError:
                return response(request, "500 Internal Server Error", "{\"error\":\"sd_io\"}");
            case DeleteResult::Deleted: break;
        }
        // All handlers run on the sole HTTP task while recording is suspended.
        // Invalidate the old receipt: completion now requires a fresh manifest.
        mark_transfer(); activity();
        return response(request, "200 OK", "{\"deleted\":true,\"path\":" +
            sd_export::json_string(relative) + ",\"manifest_invalidated\":true}");
    }
    esp_err_t file(httpd_req_t* request) {
        std::string relative;
        if (!file_path(request, relative)) return ESP_OK;
        const std::string absolute = native_path(relative);
        FILINFO info{};
        FRESULT stat_result = f_stat(absolute.c_str(), &info);
        if (stat_result != FR_OK) {
            if (stat_result == FR_NO_FILE || stat_result == FR_NO_PATH)
                return response(request, "404 Not Found", "{\"error\":\"not_file\"}");
            fail(stat_result == FR_INVALID_NAME ? ErrorCode::PathUnsupported : ErrorCode::SdIo);
            return response(request, "500 Internal Server Error", "{\"error\":\"sd_io\"}");
        }
        if (info.fattrib & AM_DIR) return response(request, "404 Not Found", "{\"error\":\"not_file\"}");
        uint64_t size = static_cast<uint64_t>(info.fsize);
        const std::string tag = etag(relative);
        std::string range_header = request_header(request, "Range", 128);
        if (httpd_req_get_hdr_value_len(request, "Range") > 128)
            return response(request, "416 Range Not Satisfiable", "{\"error\":\"invalid_range\"}");
        const std::string if_range = request_header(request, "If-Range", 128);
        if (httpd_req_get_hdr_value_len(request, "If-Range") && if_range != tag) range_header.clear();
        sd_export::ByteRange range;
        auto kind = sd_export::parse_range(range_header, size, range);
        if (kind == sd_export::RangeResult::Invalid || kind == sd_export::RangeResult::Unsatisfiable) {
            const auto content_range = "bytes */" + std::to_string(size);
            httpd_resp_set_hdr(request, "Content-Range", content_range.c_str());
            return response(request, "416 Range Not Satisfiable", "{\"error\":\"invalid_range\"}");
        }
        if (range.first > static_cast<uint64_t>(std::numeric_limits<FSIZE_t>::max())) {
            fail(ErrorCode::PathUnsupported);
            return response(request, "500 Internal Server Error", "{\"error\":\"offset_unsupported\"}");
        }
        FileCloser source;
        FRESULT opened = f_open(&source.value, absolute.c_str(), FA_READ);
        source.opened = opened == FR_OK;
        if (!source.opened || f_lseek(&source.value, static_cast<FSIZE_t>(range.first)) != FR_OK ||
            static_cast<uint64_t>(f_tell(&source.value)) != range.first) {
            fail(ErrorCode::SdIo);
            return response(request, "500 Internal Server Error", "{\"error\":\"sd_io\"}");
        }
        mark_transfer();
        httpd_resp_set_type(request, "application/octet-stream");
        httpd_resp_set_hdr(request, "Cache-Control", "no-store");
        httpd_resp_set_hdr(request, "Accept-Ranges", "bytes");
        httpd_resp_set_hdr(request, "ETag", tag.c_str());
        std::string content_range;
        if (kind == sd_export::RangeResult::Partial) {
            httpd_resp_set_status(request, "206 Partial Content");
            content_range = "bytes " + std::to_string(range.first) + '-' + std::to_string(range.last) + '/' + std::to_string(size);
            httpd_resp_set_hdr(request, "Content-Range", content_range.c_str());
        }
        uint64_t remaining = range.length;
        while (remaining) {
            if (interrupted()) return ESP_FAIL;
            size_t wanted = static_cast<size_t>(remaining > IO_BUFFER_SIZE ? IO_BUFFER_SIZE : remaining);
            UINT count = 0;
            if (!read_file(source.value, wanted, count)) return ESP_FAIL;
            if (httpd_resp_send_chunk(request, reinterpret_cast<char*>(io_buffer), static_cast<ssize_t>(count)) != ESP_OK) return ESP_FAIL;
            remaining -= count; activity(); taskYIELD();
        }
        return httpd_resp_send_chunk(request, nullptr, 0);
    }
    esp_err_t complete(httpd_req_t* request) {
        if (!request->content_len || request->content_len > 512)
            return response(request, "400 Bad Request", "{\"error\":\"invalid_receipt\"}");
        std::string body(request->content_len, '\0');
        size_t read = 0;
        while (read < body.size()) {
            if (interrupted()) return ESP_FAIL;
            int bytes = httpd_req_recv(request, body.data() + read, body.size() - read);
            if (bytes <= 0) return ESP_FAIL;
            read += static_cast<size_t>(bytes); activity();
        }
        sd_export::CompletionReceipt receipt;
        if (!sd_export::parse_receipt(body, receipt))
            return response(request, "400 Bad Request", "{\"error\":\"invalid_receipt\"}");
        if (!manifest_ready || receipt.session_id != session_text(snapshot().session_id) ||
            receipt.manifest_sha256 != manifest_hash || receipt.file_count != file_count || receipt.total_bytes != total_bytes)
            return response(request, "409 Conflict", "{\"error\":\"receipt_mismatch\"}");
        // Local file integrity is the client's assertion. The peripheral checks
        // that its completed manifest receipt is echoed exactly; no deletion.
        esp_err_t sent = response(request, "200 OK", "{\"accepted\":true,\"session_id\":" +
            sd_export::json_string(receipt.session_id) + "}");
        if (sent == ESP_OK) { receipt_accepted = true; complete_requested.store(true); }
        return sent;
    }
};

esp_err_t WifiSdExportService::initialize(const WifiSdExportHooks& hooks) {
    if (state_ || !hooks.prepare || !hooks.resume || !hooks.notify || !hooks.storage_drive) return ESP_ERR_INVALID_ARG;
    State* state = new (std::nothrow) State;
    if (!state) return ESP_ERR_NO_MEM;
    state->hooks = hooks;
    state->commands = xQueueCreate(8, sizeof(State::Command));
    if (!state->commands) { delete state; return ESP_ERR_NO_MEM; }
    if (xTaskCreatePinnedToCore(State::task_entry, "sd_export", 6144, state, 6, &state->worker, 1) != pdPASS) {
        vQueueDelete(state->commands); delete state; return ESP_ERR_NO_MEM;
    }
    state_ = state;
    return ESP_OK;
}

bool WifiSdExportService::submit(const protocol::PacketHeader& header, const uint8_t* payload) {
    if (!state_ || header.magic != protocol::MAGIC || header.version != protocol::VERSION ||
        !header.request_id || (header.request_id & 0x80000000U) || header.flags || header.image_id ||
        header.chunk_index || header.total_chunks || header.reserved) return false;
    uint64_t session_id = 0;
    if (header.message_type == MessageType::SdExportStart) {
        if (header.payload_length) return false;
    } else if (header.message_type == MessageType::SdExportCancel || header.message_type == MessageType::SdExportGetStatus) {
        if (header.payload_length != 8 || !payload) return false;
        for (unsigned i = 0; i < 8; ++i) session_id |= static_cast<uint64_t>(payload[i]) << (8 * i);
        if (header.message_type == MessageType::SdExportCancel && !session_id) return false;
    } else return false;
    if (protocol::crc32(payload, header.payload_length) != header.crc32) return false;
    // A prepare hook can occupy the worker for 30 seconds. A bounded BLE-only
    // snapshot reply must remain available so clients can recover a missed
    // PREPARING notification and learn the session ID needed for CANCEL.
    if (header.message_type == MessageType::SdExportGetStatus) {
        auto value = state_->snapshot();
        if (session_id && session_id != value.session_id) value.error = ErrorCode::InvalidSession;
        (void)state_->hooks.notify(state_->hooks.context, header.request_id, value);
        return true;
    }
    const State::Command command{header.message_type, header.request_id, session_id};
    // A client retrying START while prepare is blocked cannot spend the last
    // queue slot needed by CANCEL. BLE control is the sole command producer.
    if (header.message_type == MessageType::SdExportStart && uxQueueSpacesAvailable(state_->commands) <= 1) return false;
    if (xQueueSend(state_->commands, &command, 0) != pdTRUE) return false;
    // Cancellation must interrupt a prepare hook that is currently occupying
    // the worker, rather than wait behind it in the command queue.
    if (header.message_type == MessageType::SdExportCancel) {
        portENTER_CRITICAL(&state_->lock);
        if (session_id == state_->status.session_id) state_->cancel_requested.store(true);
        portEXIT_CRITICAL(&state_->lock);
    }
    return true;
}

size_t WifiSdExportService::copy_info(uint8_t* output, size_t capacity) const {
    if (!state_ || !output) return 0;
    portENTER_CRITICAL(&state_->lock);
    size_t size = state_->info_length;
    if (size > capacity) size = 0;
    if (size) std::memcpy(output, state_->info, size);
    portEXIT_CRITICAL(&state_->lock);
    return size;
}
}  // namespace demo
