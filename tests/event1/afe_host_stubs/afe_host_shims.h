#pragma once
/* Narrow ESP-SR/FreeRTOS declarations for startup lifecycle tests. No task is
 * executed: taking the completion semaphore simulates the fetch task exiting. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_INVALID_SIZE 0x104
static inline const char *esp_err_to_name(esp_err_t error) {
    (void)error;
    return "host error";
}
#define ESP_RETURN_ON_FALSE(condition, error, tag, ...) do { \
    (void)(tag); if (!(condition)) return (error); \
} while (0)
#define ESP_RETURN_ON_ERROR(expression, tag, ...) do { \
    esp_err_t host_error = (expression); (void)(tag); \
    if (host_error != ESP_OK) return host_error; \
} while (0)
static inline void host_log(const char *tag, const char *format, ...) {
    (void)tag; (void)format;
}
#define ESP_LOGE(tag, ...) host_log(tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) host_log(tag, __VA_ARGS__)
#define ESP_LOGI(tag, ...) host_log(tag, __VA_ARGS__)

#define MALLOC_CAP_SPIRAM 1U
#define MALLOC_CAP_8BIT 2U
#define MALLOC_CAP_INTERNAL 4U
void *heap_caps_malloc(size_t bytes, unsigned caps);
void *heap_caps_calloc(size_t count, size_t bytes, unsigned caps);
void heap_caps_free(void *memory);
static inline size_t heap_caps_get_free_size(unsigned caps) { (void)caps; return 0; }
static inline size_t heap_caps_get_largest_free_block(unsigned caps) { (void)caps; return 0; }

typedef unsigned UBaseType_t;
typedef int BaseType_t;
typedef uint32_t TickType_t;
#define pdTRUE 1
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
typedef struct {
    uint8_t *storage;
    UBaseType_t depth;
    size_t item_bytes;
} StaticQueue_t;
typedef StaticQueue_t *QueueHandle_t;
typedef struct { int token; } StaticSemaphore_t;
typedef void *SemaphoreHandle_t;
typedef void *TaskHandle_t;
/* size_t keeps the byte-count width aligned with the 64-bit host. On ESP32
 * both UBaseType_t and size_t are 32-bit. */
QueueHandle_t xQueueCreate(UBaseType_t depth, size_t item_bytes);
QueueHandle_t xQueueCreateStatic(UBaseType_t depth, size_t item_bytes,
                                uint8_t *storage, StaticQueue_t *control);
void vQueueDelete(QueueHandle_t queue);
BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t wait);
BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t wait);
SemaphoreHandle_t xSemaphoreCreateBinary(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t wait);
BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore);
void vSemaphoreDelete(SemaphoreHandle_t semaphore);
BaseType_t xTaskCreatePinnedToCore(void (*entry)(void *), const char *name,
        uint32_t stack, void *argument, UBaseType_t priority, TaskHandle_t *task, int core);
void vTaskDelete(TaskHandle_t task);
void vTaskDelay(TickType_t wait);
uint32_t ulTaskNotifyTake(BaseType_t clear_on_exit, TickType_t wait);
BaseType_t xTaskNotifyGive(TaskHandle_t task);

typedef struct { int token; } srmodel_list_t;
typedef struct { int token; } esp_afe_sr_data_t;
#define MODEL_NAME_MAX_LENGTH 64
#define ESP_VADN_PREFIX "vadnet"
#define AFE_TYPE_SR 1
#define AFE_MODE_HIGH_PERF 1
#define AFE_NS_MODE_WEBRTC 1
#define AFE_AGC_MODE_WEBRTC 1
#define VAD_MODE_1 1
#define VAD_SPEECH 1
#define AFE_VAD_ENERGY_THRESHOLD_DEFAULT (-60.0f)
typedef struct {
    bool aec_init, se_init, wakenet_init, ns_init, agc_init, vad_init;
    int afe_ns_mode, agc_mode, agc_target_level_dbfs, agc_compression_gain_db;
    char *vad_model_name;
    int vad_mode, vad_min_speech_ms, vad_min_noise_ms, vad_delay_ms;
    float vad_energy_threshold;
} afe_config_t;
typedef struct {
    int ret_value, data_size, vad_state, vad_cache_size;
    int16_t *data;
} afe_fetch_result_t;
typedef struct {
    esp_afe_sr_data_t *(*create_from_config)(afe_config_t *config);
    int (*get_feed_chunksize)(esp_afe_sr_data_t *data);
    int (*get_fetch_chunksize)(esp_afe_sr_data_t *data);
    afe_fetch_result_t *(*fetch_with_delay)(esp_afe_sr_data_t *data, TickType_t wait);
    int (*feed)(esp_afe_sr_data_t *data, const int16_t *samples);
    void (*destroy)(esp_afe_sr_data_t *data);
} esp_afe_sr_iface_t;
srmodel_list_t *esp_srmodel_init(const char *partition);
char *esp_srmodel_filter(srmodel_list_t *models, const char *prefix, const char *suffix);
void esp_srmodel_deinit(srmodel_list_t *models);
afe_config_t *afe_config_init(const char *input, srmodel_list_t *models, int type, int mode);
void afe_config_free(afe_config_t *config);
void afe_config_print(const afe_config_t *config);
const esp_afe_sr_iface_t *esp_afe_handle_from_config(const afe_config_t *config);
