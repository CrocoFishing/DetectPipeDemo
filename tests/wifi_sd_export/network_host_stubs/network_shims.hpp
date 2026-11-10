#pragma once
// Hardware boundaries only. network_lifecycle_test.cpp includes the real service.
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
using esp_err_t = int;
using ssize_t = std::ptrdiff_t;
constexpr int ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_NO_MEM = 0x101,
    ESP_ERR_INVALID_ARG = 0x102, ESP_ERR_INVALID_STATE = 0x103,
    ESP_ERR_NOT_FOUND = 0x105, ESP_ERR_TIMEOUT = 0x107;
inline const char* esp_err_to_name(int) { return "injected error"; }
constexpr unsigned MALLOC_CAP_SPIRAM = 1, MALLOC_CAP_8BIT = 2,
    MALLOC_CAP_INTERNAL = 4, MALLOC_CAP_DMA = 8;
#define CONFIG_SPIRAM 1
#define CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM 1
inline unsigned frees = 0, stops = 0, wifi_stops = 0, netif_destroys = 0;
inline int stop_result = ESP_OK;
inline void* heap_caps_malloc(size_t size, unsigned) { return std::malloc(size); }
inline void heap_caps_free(void* value) { if (value) ++frees; std::free(value); }
inline size_t heap_caps_get_free_size(unsigned) { return 2048; }
inline size_t heap_caps_get_largest_free_block(unsigned) { return 512; }
template <typename... Args> void host_log(Args...) {}
#define ESP_LOGI(...) host_log(__VA_ARGS__)
#define ESP_LOGW(...) host_log(__VA_ARGS__)
#define ESP_LOGE(...) host_log(__VA_ARGS__)
using portMUX_TYPE = int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
#define taskYIELD() ((void)0)
#define pdMS_TO_TICKS(ms) (ms)
constexpr int pdTRUE = 1, pdPASS = 1;
using QueueHandle_t = void*;
using TaskHandle_t = void*;
inline void* xQueueCreate(unsigned, size_t) { return nullptr; }
inline void vQueueDelete(void*) {}
inline int xQueueSend(void*, const void*, unsigned) { return pdTRUE; }
inline int xQueueReceive(void*, void*, unsigned) { return 0; }
inline unsigned uxQueueSpacesAvailable(void*) { return 8; }
template <typename... Args> int xTaskCreatePinnedToCore(Args...) { return 0; }
inline int64_t esp_timer_get_time() { return 100; }
inline void esp_fill_random(void* p, size_t size) { std::memset(p, 1, size); }
struct esp_netif_t {};
struct esp_netif_config_t {};
#define ESP_NETIF_DEFAULT_WIFI_AP() esp_netif_config_t{}
inline int esp_netif_init() { return ESP_OK; }
inline int esp_event_loop_create_default() { return ESP_OK; }
inline esp_netif_t* esp_netif_new(const esp_netif_config_t*) { static esp_netif_t netif; return &netif; }
inline int esp_netif_attach_wifi_ap(esp_netif_t*) { return ESP_OK; }
inline void esp_netif_destroy_default_wifi(esp_netif_t*) { ++netif_destroys; }
inline int esp_wifi_set_default_wifi_ap_handlers() { return ESP_OK; }
struct wifi_init_config_t {};
#define WIFI_INIT_CONFIG_DEFAULT() wifi_init_config_t{}
struct wifi_config_t {
    struct { char ssid[32], password[64]; uint8_t ssid_len; int channel, max_connection, authmode;
        struct { bool capable, required; } pmf_cfg; } ap;
};
constexpr int WIFI_STORAGE_RAM = 1, WIFI_MODE_AP = 1, WIFI_AUTH_WPA2_PSK = 1, WIFI_IF_AP = 1;
inline int esp_wifi_init(wifi_init_config_t*) { return ESP_OK; }
inline int esp_wifi_set_storage(int) { return ESP_OK; }
inline int esp_wifi_set_mode(int) { return ESP_OK; }
inline int esp_wifi_set_config(int, wifi_config_t*) { return ESP_OK; }
inline int esp_wifi_start() { return ESP_OK; }
inline int esp_wifi_stop() { ++wifi_stops; return ESP_OK; }
inline int esp_wifi_deinit() { return ESP_OK; }
constexpr int SHUT_RDWR = 2;
inline int shutdown(int, int) { return 0; }
using httpd_handle_t = void*;
using httpd_method_t = int;
constexpr int HTTP_GET = 1, HTTP_POST = 2, HTTP_DELETE = 3;
struct httpd_req_t { void* user_ctx; int method; const char* uri; size_t content_len; };
struct httpd_config_t {
    size_t stack_size = 0, max_req_hdr_len = 0, max_uri_len = 0;
    unsigned task_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    int max_open_sockets = 0, max_uri_handlers = 0, recv_wait_timeout = 0, send_wait_timeout = 0;
    bool lru_purge_enable = false;
};
#define HTTPD_DEFAULT_CONFIG() httpd_config_t{}
inline httpd_config_t started_config;
inline int httpd_start(void** handle, const httpd_config_t* config) {
    started_config = *config; *handle = reinterpret_cast<void*>(1); return ESP_OK;
}
inline int httpd_stop(void*) { ++stops; return stop_result; }
struct httpd_uri_t { const char* uri; int method; int (*handler)(httpd_req_t*); void* user_ctx; };
inline int httpd_register_uri_handler(void*, const httpd_uri_t*) { return ESP_OK; }
inline int httpd_req_to_sockfd(httpd_req_t*) { return 1; }
inline size_t httpd_req_get_hdr_value_len(httpd_req_t*, const char*) { return 0; }
inline int httpd_req_get_hdr_value_str(httpd_req_t*, const char*, char*, size_t) { return ESP_FAIL; }
inline size_t httpd_req_get_url_query_len(httpd_req_t*) { return 0; }
inline int httpd_req_get_url_query_str(httpd_req_t*, char*, size_t) { return ESP_FAIL; }
inline int httpd_req_recv(httpd_req_t*, char*, size_t) { return -1; }
inline int httpd_resp_set_status(httpd_req_t*, const char*) { return ESP_OK; }
inline int httpd_resp_set_type(httpd_req_t*, const char*) { return ESP_OK; }
inline int httpd_resp_set_hdr(httpd_req_t*, const char*, const char*) { return ESP_OK; }
inline int httpd_resp_send(httpd_req_t*, const char*, ssize_t) { return ESP_OK; }
inline int httpd_resp_send_chunk(httpd_req_t*, const char*, ssize_t) { return ESP_OK; }
using FSIZE_t = uint32_t;
using UINT = unsigned;
using FRESULT = int;
constexpr int FR_OK = 0, FR_DISK_ERR = 1, FR_NO_FILE = 4, FR_NO_PATH = 5, FR_INVALID_NAME = 6;
constexpr unsigned FA_READ = 1, AM_DIR = 16;
#define FF_LFN_UNICODE 2
#define FF_USE_LFN 1
struct FIL { FSIZE_t size = 0, position = 0; };
struct FF_DIR { uint32_t blk_ofs = UINT32_MAX; };
struct FILINFO { char fname[256]{}, altname[16]{}; FSIZE_t fsize = 0; unsigned fattrib = 0; };
inline int read_result = FR_OK;
inline bool short_read = false;
inline int f_open(FIL*, const char*, unsigned) { return FR_OK; }
inline int f_close(FIL*) { return FR_OK; }
inline int f_opendir(FF_DIR*, const char*) { return FR_OK; }
inline int f_closedir(FF_DIR*) { return FR_OK; }
inline int f_readdir(FF_DIR*, FILINFO* info) { info->fname[0] = '\0'; return FR_OK; }
inline int f_stat(const char*, FILINFO*) { return FR_NO_FILE; }
inline int f_unlink(const char*) { return FR_OK; }
inline int f_lseek(FIL* f, FSIZE_t pos) { f->position = pos; return FR_OK; }
inline FSIZE_t f_size(FIL* f) { return f->size; }
inline FSIZE_t f_tell(FIL* f) { return f->position; }
inline int f_read(FIL*, void*, UINT wanted, UINT* count) {
    *count = read_result != FR_OK ? 0 : short_read ? wanted - 1 : wanted;
    return read_result;
}
using psa_hash_operation_t = int;
#define PSA_HASH_OPERATION_INIT 0
constexpr int PSA_SUCCESS = 0, PSA_ALG_SHA_256 = 1;
inline int psa_crypto_init() { return PSA_SUCCESS; }
inline int psa_hash_setup(int*, int) { return PSA_SUCCESS; }
inline int psa_hash_abort(int*) { return PSA_SUCCESS; }
inline int psa_hash_update(int*, const uint8_t*, size_t) { return PSA_SUCCESS; }
inline int psa_hash_finish(int*, uint8_t* output, size_t size, size_t* length) {
    std::memset(output, 0, size); *length = size; return PSA_SUCCESS;
}
