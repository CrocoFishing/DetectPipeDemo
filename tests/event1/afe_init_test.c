/* Exercise the actual AFE bridge startup/cleanup with controlled payload heap,
 * queue, semaphore and task failures. ESP-SR itself is an inert lifecycle mock. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "afe_host_shims.h"

enum fault {
    NO_FAULT, MODEL_LOAD, MODEL_FILTER, CONFIG_CREATE, IFACE_LOOKUP, AFE_CREATE,
    QUEUE_CREATE, SEMAPHORE_CREATE, TASK_CREATE
};
typedef struct {
    void *memory;
    size_t bytes;
    unsigned caps;
    bool live, zeroed;
} allocation_t;
static allocation_t allocations[64];
static unsigned allocation_calls, allocations_succeeded, free_calls, resource_calls;
static unsigned fail_resource;
static bool fail_psram, fail_internal;
static enum fault fault;
static QueueHandle_t live_queue;
static bool model_live, config_live, afe_live, semaphore_live, task_live;
static bool task_released;
static unsigned model_inits, model_deinits, config_inits, config_frees;
static unsigned afe_creates, afe_destroys, queue_creates, queue_deletes;
static unsigned semaphore_creates, semaphore_deletes, task_creates, task_joins;
static unsigned tasks_created, task_cancellations, task_notifications;
static srmodel_list_t model;
static afe_config_t library_config;
static esp_afe_sr_data_t afe_data;
static int semaphore_token, task_token;
static int feed_chunk = 160, fetch_chunk = 512;
static void host_free(void *memory);

/* Catch libc frees too, so ownership/double-free checks cover all bridge exits. */
#define free host_free
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable:4200) /* MSVC warns on the production C99 flexible array. */
#endif
#include "../../firmware/components/audio_dsp/esp_sr_afe.c"
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#undef free

static allocation_t *live_allocation(void *memory) {
    for (unsigned i = allocation_calls; i > 0; --i) {
        if (allocations[i - 1].live && allocations[i - 1].memory == memory)
            return &allocations[i - 1];
    }
    assert(!"unowned or already-freed bridge allocation");
    return NULL;
}
static void *allocate(size_t bytes, unsigned caps, bool zeroed) {
    assert(allocation_calls < sizeof(allocations) / sizeof(allocations[0]));
    assert(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) ||
           caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (caps & MALLOC_CAP_SPIRAM) ++resource_calls;
    allocation_t *record = &allocations[allocation_calls++];
    record->bytes = bytes;
    record->caps = caps;
    record->zeroed = zeroed;
    if (resource_calls == fail_resource ||
        ((caps & MALLOC_CAP_SPIRAM) ? fail_psram : fail_internal)) return NULL;
    record->memory = zeroed ? calloc(1, bytes) : malloc(bytes);
    assert(record->memory);
    record->live = true;
    ++allocations_succeeded;
    return record->memory;
}
void *heap_caps_malloc(size_t bytes, unsigned caps) {
    return allocate(bytes, caps, false);
}
void *heap_caps_calloc(size_t count, size_t bytes, unsigned caps) {
    assert(count && bytes <= SIZE_MAX / count);
    return allocate(count * bytes, caps, true);
}
static void host_free(void *memory) {
    if (!memory) return;
    assert(!task_live); /* Join the fetch worker before releasing its resources. */
    allocation_t *record = live_allocation(memory);
    if (memory == s_afe.output_queue_storage) assert(!live_queue);
    record->live = false;
    ++free_calls;
    free(memory);
}
void heap_caps_free(void *memory) { host_free(memory); }
QueueHandle_t xQueueCreate(UBaseType_t depth, size_t item_bytes) {
    (void)depth; (void)item_bytes;
    assert(!"AFE output payload must not use a dynamic internal queue");
    return NULL;
}
QueueHandle_t xQueueCreateStatic(UBaseType_t depth, size_t item_bytes,
                                uint8_t *storage, StaticQueue_t *control) {
    ++queue_creates;
    assert(depth == AFE_OUTPUT_QUEUE_DEPTH);
    assert(item_bytes == sizeof(afe_output_packet_t) + (size_t)fetch_chunk * sizeof(int16_t));
    assert(control == &s_afe.output_queue_control);
    assert(storage == s_afe.output_queue_storage && !live_queue);
    allocation_t *backing = live_allocation(storage);
    assert(backing->bytes == depth * item_bytes && !backing->zeroed);
    if (fault == QUEUE_CREATE) return NULL;
    control->storage = storage;
    control->depth = depth;
    control->item_bytes = item_bytes;
    live_queue = control;
    return control;
}
void vQueueDelete(QueueHandle_t queue) {
    assert(queue == live_queue && !task_live);
    assert(live_allocation(queue->storage)->live);
    ++queue_deletes;
    live_queue = NULL;
}
BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t wait) {
    (void)queue; (void)item; (void)wait;
    assert(!"startup test must not execute the fetch worker");
    return 0;
}
BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t wait) {
    (void)queue; (void)item; (void)wait;
    return 0;
}
SemaphoreHandle_t xSemaphoreCreateBinary(void) {
    ++semaphore_creates;
    assert(!semaphore_live);
    if (fault == SEMAPHORE_CREATE) return NULL;
    semaphore_live = true;
    return &semaphore_token;
}
BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t wait) {
    assert(semaphore == &semaphore_token && semaphore_live);
    assert(wait == portMAX_DELAY && task_live && task_released && s_afe.fetch_started);
    assert(atomic_load(&s_afe.stop_fetch));
    assert(afe_live && live_queue); /* Library/queue still available at worker exit. */
    task_live = false;
    task_released = false;
    ++task_joins;
    return pdTRUE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore) {
    (void)semaphore;
    assert(!"startup test must not execute the fetch worker");
    return 0;
}
void vSemaphoreDelete(SemaphoreHandle_t semaphore) {
    assert(semaphore == &semaphore_token && semaphore_live && !task_live);
    semaphore_live = false;
    ++semaphore_deletes;
}
BaseType_t xTaskCreatePinnedToCore(void (*entry)(void *), const char *name,
        uint32_t stack, void *argument, UBaseType_t priority, TaskHandle_t *task, int core) {
    ++task_creates;
    assert(entry == afe_fetch_task && strcmp(name, "afe_fetch") == 0);
    assert(stack == 4096 && !argument && priority == 16 && core == 1);
    assert(!afe_live && !live_queue && !model_live && !config_live);
    assert(semaphore_live && !task_live && !task_released && !s_afe.fetch_started);
    assert(!s_afe.data && !s_afe.feed_buffer && !s_afe.output_queue_storage);
    assert(!atomic_load(&s_afe.stop_fetch));
    if (fault == TASK_CREATE) return 0;
    task_live = true;
    ++tasks_created;
    *task = &task_token;
    return pdPASS;
}
void vTaskDelete(TaskHandle_t task) {
    assert(task == &task_token && task_live && !task_released && !s_afe.fetch_started);
    assert(atomic_load(&s_afe.stop_fetch));
    task_live = false;
    ++task_cancellations;
}
void vTaskDelay(TickType_t wait) { (void)wait; assert(!"fetch worker is inert"); }
uint32_t ulTaskNotifyTake(BaseType_t clear_on_exit, TickType_t wait) {
    (void)clear_on_exit; (void)wait;
    assert(!"startup test must not execute the fetch worker");
    return 0;
}
BaseType_t xTaskNotifyGive(TaskHandle_t task) {
    assert(task == &task_token && task_live && !task_released && s_afe.fetch_started);
    assert(afe_live && live_queue && semaphore_live && !config_live);
    assert(s_afe.feed_buffer && s_afe.fetch_packet && s_afe.output_packet);
    task_released = true;
    ++task_notifications;
    return pdPASS;
}

srmodel_list_t *esp_srmodel_init(const char *partition) {
    assert(strcmp(partition, "model") == 0 && !model_live);
    assert(task_live && !task_released && !s_afe.fetch_started && semaphore_live);
    if (fault == MODEL_LOAD) return NULL;
    ++model_inits;
    model_live = true;
    return &model;
}
char *esp_srmodel_filter(srmodel_list_t *models, const char *prefix, const char *suffix) {
    assert(models == &model && model_live && !suffix);
    assert(strcmp(prefix, ESP_VADN_PREFIX) == 0);
    return fault == MODEL_FILTER ? NULL : "vadnet1_medium";
}
void esp_srmodel_deinit(srmodel_list_t *models) {
    assert(models == &model && model_live && !afe_live && !task_live);
    model_live = false;
    ++model_deinits;
}
afe_config_t *afe_config_init(const char *input, srmodel_list_t *models, int type, int mode) {
    assert(strcmp(input, "M") == 0 && type == AFE_TYPE_SR && mode == AFE_MODE_HIGH_PERF);
    assert(!config_live && (models ? model_live && models == &model : !model_live));
    assert(task_live && !task_released && !s_afe.fetch_started);
    if (fault == CONFIG_CREATE) return NULL;
    memset(&library_config, 0, sizeof(library_config));
    config_live = true;
    ++config_inits;
    return &library_config;
}
void afe_config_free(afe_config_t *config) {
    assert(config == &library_config && config_live);
    config_live = false;
    ++config_frees;
}
void afe_config_print(const afe_config_t *config) {
    assert(config == &library_config && config_live);
}
static esp_afe_sr_data_t *create_afe(afe_config_t *config) {
    assert(config == &library_config && config_live && !afe_live);
    if (fault == AFE_CREATE) return NULL;
    assert(!config->vad_init || model_live);
    ++afe_creates;
    afe_live = true;
    return &afe_data;
}
static int get_feed_chunk(esp_afe_sr_data_t *data) {
    assert(data == &afe_data && afe_live);
    return feed_chunk;
}
static int get_fetch_chunk(esp_afe_sr_data_t *data) {
    assert(data == &afe_data && afe_live);
    return fetch_chunk;
}
static afe_fetch_result_t *fetch_afe(esp_afe_sr_data_t *data, TickType_t wait) {
    (void)data; (void)wait;
    assert(!"startup test must not execute the fetch worker");
    return NULL;
}
static int feed_afe(esp_afe_sr_data_t *data, const int16_t *samples) {
    (void)data; (void)samples;
    assert(!"startup test does not feed PCM");
    return 0;
}
static void destroy_afe(esp_afe_sr_data_t *data) {
    assert(data == &afe_data && afe_live && !task_live);
    afe_live = false;
    ++afe_destroys;
}
const esp_afe_sr_iface_t *esp_afe_handle_from_config(const afe_config_t *config) {
    static const esp_afe_sr_iface_t iface = {
        create_afe, get_feed_chunk, get_fetch_chunk, fetch_afe, feed_afe, destroy_afe
    };
    assert(config == &library_config && config_live);
    return fault == IFACE_LOOKUP ? NULL : &iface;
}

static const esp_sr_afe_config_t config = {false, false, true, 3, 9};
static void assert_clean(void) {
    assert(!esp_sr_afe_is_initialized());
    assert(esp_sr_afe_feed_chunk_samples() == 0 && esp_sr_afe_fetch_chunk_samples() == 0);
    assert(!live_queue && !semaphore_live && !task_live && !afe_live && !model_live && !config_live);
    assert(!task_released && !s_afe.fetch_started);
    assert(free_calls == allocations_succeeded && queue_deletes <= queue_creates);
    assert(model_inits == model_deinits && config_inits == config_frees && afe_creates == afe_destroys);
    assert(tasks_created == task_joins + task_cancellations && task_notifications == task_joins);
    for (unsigned i = 0; i < allocation_calls; ++i) assert(!allocations[i].live);
}
static void reset_case(void) {
    esp_sr_afe_deinit();
    assert_clean();
    memset(allocations, 0, sizeof(allocations));
    allocation_calls = allocations_succeeded = free_calls = resource_calls = 0;
    model_inits = model_deinits = config_inits = config_frees = 0;
    afe_creates = afe_destroys = queue_creates = queue_deletes = 0;
    semaphore_creates = semaphore_deletes = task_creates = task_joins = 0;
    tasks_created = task_cancellations = task_notifications = 0;
    fault = NO_FAULT;
    fail_resource = 0;
    fail_psram = fail_internal = false;
    feed_chunk = 160;
    fetch_chunk = 512;
}
static void assert_running(void) {
    assert(esp_sr_afe_is_initialized() && afe_live && task_live && live_queue && semaphore_live);
    assert(task_released && s_afe.fetch_started);
    assert(esp_sr_afe_feed_chunk_samples() == (size_t)feed_chunk);
    assert(esp_sr_afe_fetch_chunk_samples() == (size_t)fetch_chunk);
    assert(s_afe.feed_buffer && s_afe.fetch_packet && s_afe.output_packet && s_afe.output_queue_storage);
    assert(!config_live);
    assert(live_allocation(s_afe.feed_buffer)->bytes == (size_t)feed_chunk * sizeof(int16_t));
    assert(live_allocation(s_afe.feed_buffer)->zeroed);
    assert(live_allocation(s_afe.fetch_packet)->zeroed && live_allocation(s_afe.output_packet)->zeroed);
}
static void retry_after_failure(void) {
    assert_clean();
    fault = NO_FAULT;
    fail_resource = 0;
    fail_psram = fail_internal = false;
    assert(esp_sr_afe_init(&config) == ESP_OK);
    assert_running();
    unsigned joins = task_joins;
    esp_sr_afe_deinit();
    assert(task_joins == joins + 1);
    assert_clean();
    esp_sr_afe_deinit();
    assert(task_joins == joins + 1);
    assert_clean();
}
static void test_psram_when_internal_payload_unavailable(void) {
    reset_case();
    fail_internal = true;
    assert(esp_sr_afe_init(&config) == ESP_OK);
    assert_running();
    assert(allocation_calls == 4 && resource_calls == 4);
    for (unsigned i = 0; i < allocation_calls; ++i)
        assert(allocations[i].caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    assert(queue_creates == 1 && semaphore_creates == 1 && task_creates == 1);
    esp_sr_afe_deinit();
    assert(free_calls == 4 && queue_deletes == 1 && semaphore_deletes == 1 && task_joins == 1);
    assert_clean();
}
static void test_internal_fallback(void) {
    reset_case();
    fail_psram = true;
    assert(esp_sr_afe_init(&config) == ESP_OK);
    assert_running();
    assert(allocation_calls == 8 && resource_calls == 4);
    for (unsigned i = 0; i < allocation_calls; i += 2) {
        assert(allocations[i].caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        assert(!allocations[i].memory);
        assert(allocations[i + 1].caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        assert(allocations[i + 1].bytes == allocations[i].bytes);
    }
    esp_sr_afe_deinit();
    assert_clean();
}
static void test_bridge_allocation_failures(void) {
    for (unsigned resource = 1; resource <= 4; ++resource) {
        reset_case();
        fail_resource = resource;
        assert(esp_sr_afe_init(&config) == ESP_ERR_NO_MEM);
        assert(task_creates == 1 && tasks_created == 1 && task_cancellations == 1);
        assert(task_joins == 0 && task_notifications == 0);
        assert(resource_calls == resource && allocation_calls == resource + 1);
        assert(allocations[allocation_calls - 2].caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        assert(allocations[allocation_calls - 1].caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        retry_after_failure();
    }
}
static void test_create_failures(void) {
    const enum fault faults[] = {QUEUE_CREATE, SEMAPHORE_CREATE, TASK_CREATE};
    for (unsigned i = 0; i < sizeof(faults) / sizeof(faults[0]); ++i) {
        reset_case();
        fault = faults[i];
        assert(esp_sr_afe_init(&config) == ESP_ERR_NO_MEM);
        assert(free_calls == (fault == QUEUE_CREATE ? 4U : 0U));
        assert(queue_deletes == 0 && task_joins == 0 && task_notifications == 0);
        assert(semaphore_deletes == (fault == SEMAPHORE_CREATE ? 0U : 1U));
        assert(task_cancellations == (fault == QUEUE_CREATE ? 1U : 0U));
        assert(tasks_created == task_cancellations);
        assert(model_inits == (fault == QUEUE_CREATE ? 1U : 0U));
        retry_after_failure();
    }
}
static void test_library_failures(void) {
    const enum fault faults[] = {MODEL_LOAD, MODEL_FILTER, CONFIG_CREATE, IFACE_LOOKUP, AFE_CREATE};
    const esp_err_t errors[] = {ESP_ERR_NOT_FOUND, ESP_ERR_NOT_FOUND, ESP_ERR_NO_MEM, ESP_FAIL, ESP_ERR_NO_MEM};
    for (unsigned i = 0; i < sizeof(faults) / sizeof(faults[0]); ++i) {
        reset_case();
        fault = faults[i];
        assert(esp_sr_afe_init(&config) == errors[i]);
        assert(allocation_calls == 0 && task_creates == 1 && task_cancellations == 1);
        assert(task_notifications == 0 && task_joins == 0);
        retry_after_failure();
    }
}
static void test_invalid_duplicate_and_reinit(void) {
    reset_case();
    assert(esp_sr_afe_init(NULL) == ESP_ERR_INVALID_ARG);
    assert_clean();
    assert(esp_sr_afe_init(&config) == ESP_OK);
    QueueHandle_t first_control = live_queue;
    void *first_storage = s_afe.output_queue_storage;
    assert(esp_sr_afe_init(&config) == ESP_ERR_INVALID_STATE);
    assert(esp_sr_afe_init(NULL) == ESP_ERR_INVALID_ARG);
    assert(allocation_calls == 4 && task_creates == 1 && model_inits == 1);
    assert(live_queue == first_control && s_afe.output_queue_storage == first_storage);
    assert_running();
    esp_sr_afe_deinit();
    assert_clean();
    assert(esp_sr_afe_init(&config) == ESP_OK);
    assert(live_queue == first_control); /* Static queue control survives reset. */
    esp_sr_afe_deinit();
    assert_clean();
}
int main(void) {
    test_psram_when_internal_payload_unavailable();
    test_internal_fallback();
    test_bridge_allocation_failures();
    test_create_failures();
    test_library_failures();
    test_invalid_duplicate_and_reinit();
    puts("AFE init/deinit tests passed (15 failure/heap/lifecycle cases)");
    return 0;
}
