#pragma once
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
static inline const char *esp_err_to_name(esp_err_t error) { (void)error; return "host error"; }
#define ESP_RETURN_ON_ERROR(expression, tag, ...) do { \
    esp_err_t host_error = (expression); (void)(tag); \
    if (host_error != ESP_OK) return host_error; \
} while (0)
#define ESP_RETURN_ON_FALSE(condition, error, tag, ...) do { \
    (void)(tag); if (!(condition)) return (error); \
} while (0)
#define ESP_GOTO_ON_ERROR(expression, label, tag, ...) do { \
    esp_err_t host_error = (expression); (void)(tag); \
    if (host_error != ESP_OK) { ret = host_error; goto label; } \
} while (0)
#define ESP_GOTO_ON_FALSE(condition, error, label, tag, ...) do { \
    (void)(tag); if (!(condition)) { ret = (error); goto label; } \
} while (0)
static inline void host_log(const char *tag, const char *format, ...) { (void)tag; (void)format; }
#define ESP_LOGE(tag, ...) host_log(tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) host_log(tag, __VA_ARGS__)
#define ESP_LOGI(tag, ...) host_log(tag, __VA_ARGS__)

#define MALLOC_CAP_SPIRAM 1U
#define MALLOC_CAP_8BIT 2U
#define MALLOC_CAP_INTERNAL 4U
#define MALLOC_CAP_DMA 8U
void *heap_caps_malloc(size_t bytes, unsigned caps);
void heap_caps_free(void *memory);
static inline size_t heap_caps_get_free_size(unsigned caps) { (void)caps; return 0; }
static inline size_t heap_caps_get_largest_free_block(unsigned caps) { (void)caps; return 0; }
static inline size_t heap_caps_get_minimum_free_size(unsigned caps) { (void)caps; return 0; }
static inline int64_t esp_timer_get_time(void) { return 0; }

typedef unsigned UBaseType_t;
typedef int BaseType_t;
typedef uint32_t TickType_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
typedef struct { uint8_t *storage; UBaseType_t depth, item_bytes; } StaticQueue_t;
typedef StaticQueue_t *QueueHandle_t;
typedef struct { bool allocated; unsigned notifications; } host_task_t;
typedef host_task_t *TaskHandle_t;
QueueHandle_t xQueueCreateStatic(UBaseType_t depth, UBaseType_t item_bytes,
                                uint8_t *storage, StaticQueue_t *control);
void vQueueDelete(QueueHandle_t queue);
BaseType_t xTaskCreatePinnedToCore(void (*entry)(void *), const char *name,
        uint32_t stack, void *argument, UBaseType_t priority, TaskHandle_t *task, int core);
void vTaskDelete(TaskHandle_t task);
void vTaskDelay(TickType_t wait);
BaseType_t xTaskNotifyGive(TaskHandle_t task);
static inline uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t wait) {
    (void)clear; (void)wait; return 0;
}
static inline UBaseType_t uxQueueMessagesWaiting(QueueHandle_t queue) { (void)queue; return 0; }
static inline BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t wait) {
    (void)queue; (void)item; (void)wait; return pdTRUE;
}
static inline BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t wait) {
    (void)queue; (void)item; (void)wait; return pdFALSE;
}
static inline BaseType_t xQueueReset(QueueHandle_t queue) { (void)queue; return pdPASS; }
