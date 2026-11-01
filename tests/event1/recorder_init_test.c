/* Exercise the real recorder init/deinit with controlled heap availability.
 * This executable has its own dependency shims, separate from artifact tests. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "recorder_host_shims.h"
#include "audio_types.h"
#include "wav_writer.h"

static bool fail_psram, fail_internal, fail_queue_create;
static unsigned allocation_calls, free_calls, queue_create_calls, queue_delete_calls;
static unsigned allocation_caps[8];
static size_t allocation_bytes[8];
static void *live_storage;
static QueueHandle_t live_queue;
static void host_free(void *memory);
static size_t host_strlcpy(char *dest, const char *source, size_t bytes) {
    size_t len = strlen(source);
    if (bytes) {
        size_t copied = len < bytes - 1 ? len : bytes - 1;
        memcpy(dest, source, copied);
        dest[copied] = '\0';
    }
    return len;
}
void audio_stats_lock(void) {}
void audio_stats_unlock(void) {}

/* Wrap production frees so leaks/double frees fail even when libc tolerates a
 * NULL pointer. The allocator implementation below uses unwrapped libc. */
#define free host_free
#define strlcpy host_strlcpy
#include "../../firmware/components/recorder/sd_recorder.c"
#undef strlcpy
#undef free

void *heap_caps_malloc(size_t bytes, unsigned caps) {
    assert(allocation_calls < sizeof(allocation_caps) / sizeof(allocation_caps[0]));
    allocation_caps[allocation_calls] = caps;
    allocation_bytes[allocation_calls++] = bytes;
    assert(bytes == RECORDER_QUEUE_DEPTH * sizeof(recorder_packet_t));
    assert(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) ||
           caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if ((caps & MALLOC_CAP_SPIRAM) ? fail_psram : fail_internal) return NULL;
    assert(live_storage == NULL);
    live_storage = malloc(bytes);
    assert(live_storage);
    return live_storage;
}
static void host_free(void *memory) {
    if (!memory) return;
    assert(memory == live_storage);
    assert(live_queue == NULL); /* Queue must be deleted before its backing. */
    ++free_calls;
    live_storage = NULL;
    free(memory);
}
void heap_caps_free(void *memory) { host_free(memory); }
QueueHandle_t xQueueCreate(UBaseType_t depth, UBaseType_t item_bytes) {
    (void)depth; (void)item_bytes;
    assert(!"recorder must use a static queue with separately allocated backing");
    return NULL;
}
QueueHandle_t xQueueCreateStatic(UBaseType_t depth, UBaseType_t item_bytes,
                                uint8_t *storage, StaticQueue_t *control) {
    ++queue_create_calls;
    assert(depth == RECORDER_QUEUE_DEPTH);
    assert(item_bytes == sizeof(recorder_packet_t));
    assert(storage == live_storage && control);
    uintptr_t start = (uintptr_t)storage;
    uintptr_t end = start + depth * item_bytes;
    assert((uintptr_t)control < start || (uintptr_t)control >= end);
    assert(live_queue == NULL);
    if (fail_queue_create) return NULL;
    control->storage = storage;
    control->depth = depth;
    control->item_bytes = item_bytes;
    live_queue = control;
    return control;
}
void vQueueDelete(QueueHandle_t queue) {
    assert(queue == live_queue && live_storage == queue->storage);
    ++queue_delete_calls;
    live_queue = NULL;
}

/* Link-only worker dependencies; start/event I/O is outside this test. */
esp_err_t wav_writer_open(wav_writer_t *writer, const char *path,
        uint32_t rate, uint16_t channels, uint16_t bits) {
    (void)writer; (void)path; (void)rate; (void)channels; (void)bits;
    return ESP_FAIL;
}
esp_err_t wav_writer_write(wav_writer_t *writer, const int16_t *samples, size_t count) {
    (void)writer; (void)samples; (void)count;
    return ESP_FAIL;
}
esp_err_t wav_writer_flush(wav_writer_t *writer) { (void)writer; return ESP_FAIL; }
esp_err_t wav_writer_close(wav_writer_t *writer) { (void)writer; return ESP_FAIL; }
esp_err_t wav_writer_repair(const char *path, uint64_t *bytes) {
    (void)path; (void)bytes;
    return ESP_FAIL;
}

static void reset_case(void) {
    assert(!live_storage && !live_queue);
    sd_recorder_deinit();
    fail_psram = fail_internal = fail_queue_create = false;
    allocation_calls = free_calls = queue_create_calls = queue_delete_calls = 0;
    memset(allocation_caps, 0, sizeof(allocation_caps));
    memset(allocation_bytes, 0, sizeof(allocation_bytes));
}
static void assert_psram_first(void) {
    assert(allocation_caps[0] == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}
static void assert_fallback(void) {
    assert_psram_first();
    assert(allocation_caps[1] == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    assert(allocation_bytes[0] == allocation_bytes[1]);
}
static void assert_clean(unsigned expected_frees, unsigned expected_deletes) {
    assert(!live_storage && !live_queue && !sd_recorder_is_running());
    assert(free_calls == expected_frees);
    assert(queue_delete_calls == expected_deletes);
}
static void test_psram_when_internal_unavailable(void) {
    reset_case();
    const sd_recorder_config_t config = {0};
    fail_internal = true;
    assert(sd_recorder_init(&config) == ESP_OK);
    assert(allocation_calls == 1 && queue_create_calls == 1);
    assert_psram_first();
    sd_recorder_deinit();
    assert_clean(1, 1);
}
static void test_internal_fallback(void) {
    reset_case();
    const sd_recorder_config_t config = {0};
    fail_psram = true;
    assert(sd_recorder_init(&config) == ESP_OK);
    assert(allocation_calls == 2 && queue_create_calls == 1);
    assert_fallback();
    sd_recorder_deinit();
    assert_clean(1, 1);
}
static void test_allocation_failure_retry(void) {
    reset_case();
    const sd_recorder_config_t config = {0};
    fail_psram = fail_internal = true;
    assert(sd_recorder_init(&config) == ESP_ERR_NO_MEM);
    assert(allocation_calls == 2 && queue_create_calls == 0);
    assert_fallback();
    assert_clean(0, 0);
    fail_psram = false;
    assert(sd_recorder_init(&config) == ESP_OK);
    assert(allocation_calls == 3 && queue_create_calls == 1);
    sd_recorder_deinit();
    assert_clean(1, 1);
}
static void test_static_queue_failure_retry(void) {
    reset_case();
    const sd_recorder_config_t config = {0};
    fail_queue_create = true;
    assert(sd_recorder_init(&config) == ESP_ERR_NO_MEM);
    assert(allocation_calls == 1 && queue_create_calls == 1);
    assert_clean(1, 0);
    fail_queue_create = false;
    assert(sd_recorder_init(&config) == ESP_OK);
    assert(allocation_calls == 2 && queue_create_calls == 2);
    sd_recorder_deinit();
    assert_clean(2, 1);
}
static void test_invalid_duplicate_and_reinit(void) {
    reset_case();
    const sd_recorder_config_t config = {0};
    assert(sd_recorder_init(NULL) == ESP_ERR_INVALID_ARG);
    assert(allocation_calls == 0 && queue_create_calls == 0);
    assert(sd_recorder_init(&config) == ESP_OK);
    QueueHandle_t first_control = live_queue;
    void *first_storage = live_storage;
    assert(sd_recorder_init(&config) == ESP_ERR_INVALID_ARG);
    assert(sd_recorder_init(NULL) == ESP_ERR_INVALID_ARG);
    assert(allocation_calls == 1 && queue_create_calls == 1);
    assert(live_queue == first_control && live_storage == first_storage);
    sd_recorder_deinit();
    assert_clean(1, 1);
    sd_recorder_deinit();
    assert_clean(1, 1);
    assert(sd_recorder_init(&config) == ESP_OK);
    assert(live_queue == first_control); /* Stable control object, no heap allocation. */
    assert(allocation_calls == 2 && queue_create_calls == 2);
    sd_recorder_deinit();
    assert_clean(2, 2);
}
int main(void) {
    test_psram_when_internal_unavailable();
    test_internal_fallback();
    test_allocation_failure_retry();
    test_static_queue_failure_retry();
    test_invalid_duplicate_and_reinit();
    puts("recorder init/deinit tests passed (5 cases)");
    return 0;
}
