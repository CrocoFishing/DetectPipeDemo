#pragma once
/* Host-only declarations needed to compile sd_recorder.c. Hardware and worker
 * entry points remain inert; the lifecycle test exercises allocation/queues. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_TIMEOUT 0x107
static inline const char *esp_err_to_name(esp_err_t error) {
    (void)error;
    return "host error";
}
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
void heap_caps_free(void *memory);
static inline size_t heap_caps_get_free_size(unsigned caps) { (void)caps; return 0; }
static inline size_t heap_caps_get_largest_free_block(unsigned caps) { (void)caps; return 0; }

typedef unsigned UBaseType_t;
typedef int BaseType_t;
typedef uint32_t TickType_t;
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
#define pdTRUE 1
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
typedef struct {
    uint8_t *storage;
    UBaseType_t depth;
    UBaseType_t item_bytes;
} StaticQueue_t;
typedef StaticQueue_t *QueueHandle_t;
typedef void *TaskHandle_t;
QueueHandle_t xQueueCreate(UBaseType_t depth, UBaseType_t item_bytes);
QueueHandle_t xQueueCreateStatic(UBaseType_t depth, UBaseType_t item_bytes,
                                uint8_t *storage, StaticQueue_t *control);
void vQueueDelete(QueueHandle_t queue);
#ifdef RECORDER_QUEUE_TEST
UBaseType_t uxQueueSpacesAvailable(QueueHandle_t queue);
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t queue);
BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t wait);
BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t wait);
#else
static inline UBaseType_t uxQueueSpacesAvailable(QueueHandle_t queue) {
    return queue->depth;
}
static inline UBaseType_t uxQueueMessagesWaiting(QueueHandle_t queue) {
    (void)queue;
    return 0;
}
static inline BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t wait) {
    (void)queue; (void)item; (void)wait;
    return pdTRUE;
}
static inline BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t wait) {
    (void)queue; (void)item; (void)wait;
    return 0;
}
#endif
static inline BaseType_t xTaskCreatePinnedToCore(void (*entry)(void *), const char *name,
        uint32_t stack, void *argument, UBaseType_t priority, TaskHandle_t *task, int core) {
    (void)entry; (void)name; (void)stack; (void)argument; (void)priority; (void)task; (void)core;
    return 0;
}
static inline void vTaskDelete(TaskHandle_t task) { (void)task; }
static inline void vTaskDelay(TickType_t wait) { (void)wait; }

typedef struct { int sd_mosi, sd_miso, sd_sclk, sd_cs; } board_audio_pins_t;
static inline const board_audio_pins_t *board_audio_pins(void) {
    static const board_audio_pins_t pins = {0};
    return &pins;
}
typedef struct { int slot; } sdmmc_host_t;
typedef struct { int unused; } sdmmc_card_t;
typedef struct { int gpio_cs, host_id; } sdspi_device_config_t;
typedef struct {
    int mosi_io_num, miso_io_num, sclk_io_num, quadwp_io_num, quadhd_io_num, max_transfer_sz;
} spi_bus_config_t;
typedef struct {
    bool format_if_mount_failed;
    int max_files;
    size_t allocation_unit_size;
} esp_vfs_fat_sdmmc_mount_config_t;
#define SDSPI_HOST_DEFAULT() {0}
#define SDSPI_DEVICE_CONFIG_DEFAULT() {0}
#define SDSPI_DEFAULT_DMA 0
static inline esp_err_t spi_bus_initialize(int host, const spi_bus_config_t *config, int dma) {
    (void)host; (void)config; (void)dma;
    return ESP_FAIL;
}
static inline esp_err_t spi_bus_free(int host) { (void)host; return ESP_OK; }
static inline esp_err_t esp_vfs_fat_sdspi_mount(const char *path, const sdmmc_host_t *host,
        const sdspi_device_config_t *slot, const esp_vfs_fat_sdmmc_mount_config_t *config,
        sdmmc_card_t **card) {
    (void)path; (void)host; (void)slot; (void)config; (void)card;
    return ESP_FAIL;
}
static inline esp_err_t esp_vfs_fat_sdcard_unmount(const char *path, sdmmc_card_t *card) {
    (void)path; (void)card;
    return ESP_OK;
}
static inline int64_t esp_timer_get_time(void) { return 0; }

typedef struct { int unused; } DIR;
struct dirent { char d_name[256]; };
static inline DIR *opendir(const char *path) { (void)path; return NULL; }
static inline struct dirent *readdir(DIR *directory) { (void)directory; return NULL; }
static inline int closedir(DIR *directory) { (void)directory; return 0; }
