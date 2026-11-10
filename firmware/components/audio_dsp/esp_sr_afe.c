#include "esp_sr_afe.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_afe_config.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_vadn_models.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "model_path.h"

#define AFE_OUTPUT_QUEUE_DEPTH 8
#define AFE_FETCH_WAIT_MS 100

typedef struct {
    size_t count;
    size_t vad_cache_bytes;
    audio_vad_state_t vad_state;
    int16_t samples[];
} afe_output_packet_t;

static const char *TAG = "esp_sr_afe";

typedef struct {
    const esp_afe_sr_iface_t *iface;
    esp_afe_sr_data_t *data;
    srmodel_list_t *models;
    int16_t *feed_buffer;
    size_t feed_samples;
    size_t fetch_samples;
    size_t feed_used;
    uint64_t feed_first_sample;
    uint64_t next_output_sample;
    bool output_index_initialized;
    QueueHandle_t output_queue;
    StaticQueue_t output_queue_control;
    uint8_t *output_queue_storage;
    SemaphoreHandle_t fetch_stopped;
    TaskHandle_t fetch_task;
    bool fetch_started;
    atomic_bool stop_fetch;
    atomic_int last_fetch_error;
    afe_output_packet_t *fetch_packet;
    afe_output_packet_t *output_packet;
    uint64_t fed_samples;
    uint64_t output_samples;
    uint64_t last_progress_feed;
} afe_state_t;

static afe_state_t s_afe;

static void log_memory_failure(const char *resource, size_t bytes)
{
    ESP_LOGE(TAG, "%s failed: bytes=%u feed_samples=%u fetch_samples=%u "
             "internal_free=%u internal_largest=%u psram_free=%u psram_largest=%u",
             resource, (unsigned)bytes,
             (unsigned)s_afe.feed_samples, (unsigned)s_afe.fetch_samples,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

static void *pcm_calloc(size_t bytes, bool *in_psram)
{
    void *buffer = heap_caps_calloc(1, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    *in_psram = buffer != NULL;
    if (!buffer) {
        buffer = heap_caps_calloc(1, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return buffer;
}

static void afe_fetch_task(void *arg)
{
    (void)arg;
    /* Reserve this internal stack before ESP-SR fragments the heap. Library
     * pointers and bridge buffers are published before this gate is released.
     */
    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    while (!atomic_load(&s_afe.stop_fetch)) {
        /* Feed runs independently, so waiting for a complete output frame
         * cannot prevent the input needed to produce that frame.
         * The bounded wait also lets deinit join this task before destroy.
         */
        afe_fetch_result_t *result = s_afe.iface->fetch_with_delay(
            s_afe.data, pdMS_TO_TICKS(AFE_FETCH_WAIT_MS));
        if (atomic_load(&s_afe.stop_fetch)) {
            break;
        }
        if (!result || result->ret_value != ESP_OK) {
            atomic_store(&s_afe.last_fetch_error, result ? result->ret_value : ESP_FAIL);
            /* Some failures return before the requested timeout. Always let
             * the lower-priority producer and control tasks run in that case.
             */
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        if (!result->data || result->data_size <= 0 ||
            result->data_size % sizeof(int16_t) != 0 ||
            (size_t)result->data_size > s_afe.fetch_samples * sizeof(int16_t)) {
            ESP_LOGE(TAG, "invalid AFE output size: %d", result->data_size);
            atomic_store(&s_afe.last_fetch_error, ESP_ERR_INVALID_SIZE);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        atomic_store(&s_afe.last_fetch_error, ESP_OK);
        afe_output_packet_t *packet = s_afe.fetch_packet;
        packet->count = (size_t)result->data_size / sizeof(int16_t);
        packet->vad_state = result->vad_state == VAD_SPEECH ?
            AUDIO_VAD_SPEECH : AUDIO_VAD_SILENCE;
        packet->vad_cache_bytes = result->vad_cache_size > 0 ?
            (size_t)result->vad_cache_size : 0;
        memcpy(packet->samples, result->data, (size_t)result->data_size);
        /* Own the PCM before the next fetch invalidates the library pointer.
         * Callbacks remain on the original DSP task under its existing lock.
         */
        while (!atomic_load(&s_afe.stop_fetch) &&
               xQueueSend(s_afe.output_queue, packet, pdMS_TO_TICKS(20)) != pdTRUE) {
        }
    }
    xSemaphoreGive(s_afe.fetch_stopped);
    vTaskDelete(NULL);
}

esp_err_t esp_sr_afe_init(const esp_sr_afe_config_t *config)
{
    ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG, "config is required");
    ESP_RETURN_ON_FALSE(!s_afe.data && !s_afe.fetch_task, ESP_ERR_INVALID_STATE,
                        TAG, "already initialized");

    atomic_init(&s_afe.stop_fetch, false);
    atomic_init(&s_afe.last_fetch_error, ESP_OK);
    s_afe.fetch_stopped = xSemaphoreCreateBinary();
    if (!s_afe.fetch_stopped) {
        log_memory_failure("AFE fetch stop semaphore allocation", sizeof(StaticSemaphore_t));
        goto no_mem;
    }
    if (xTaskCreatePinnedToCore(afe_fetch_task, "afe_fetch", 4096, NULL, 16,
                                &s_afe.fetch_task, 1) != pdPASS) {
        s_afe.fetch_task = NULL;
        log_memory_failure("AFE fetch task creation (internal stack)", 4096);
        goto no_mem;
    }

    char *vad_model_name = NULL;
    char vad_model_log[MODEL_NAME_MAX_LENGTH] = "none";
    if (config->enable_vad) {
        s_afe.models = esp_srmodel_init("model");
        if (!s_afe.models) {
            ESP_LOGE(TAG, "VADNet model partition could not be loaded");
            esp_sr_afe_deinit();
            return ESP_ERR_NOT_FOUND;
        }
        vad_model_name = esp_srmodel_filter(s_afe.models, ESP_VADN_PREFIX, NULL);
        if (!vad_model_name) {
            ESP_LOGE(TAG, "VADNet model is missing from the model partition");
            esp_sr_afe_deinit();
            return ESP_ERR_NOT_FOUND;
        }
        snprintf(vad_model_log, sizeof(vad_model_log), "%s", vad_model_name);
    }

    afe_config_t *afe_config = afe_config_init(
        "M", s_afe.models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    if (!afe_config) {
        log_memory_failure("AFE configuration allocation", sizeof(afe_config_t));
        esp_sr_afe_deinit();
        return ESP_ERR_NO_MEM;
    }
    afe_config->aec_init = false;
    afe_config->se_init = false;
    afe_config->wakenet_init = false;
    afe_config->ns_init = config->enable_ns;
    afe_config->afe_ns_mode = AFE_NS_MODE_WEBRTC;
    afe_config->agc_init = config->enable_agc;
    afe_config->agc_mode = AFE_AGC_MODE_WEBRTC;
    afe_config->agc_target_level_dbfs = config->agc_target_level_dbfs;
    afe_config->agc_compression_gain_db = config->agc_compression_gain_db;
    afe_config->vad_init = config->enable_vad;
    afe_config->vad_model_name = vad_model_name;
    afe_config->vad_mode = VAD_MODE_0;
    afe_config->vad_min_speech_ms = 128;
    afe_config->vad_min_noise_ms = 1000;
    afe_config->vad_delay_ms = 128;
    afe_config->vad_energy_threshold = AFE_VAD_ENERGY_THRESHOLD_DEFAULT;

    s_afe.iface = esp_afe_handle_from_config(afe_config);
    if (!s_afe.iface) {
        afe_config_free(afe_config);
        esp_sr_afe_deinit();
        return ESP_FAIL;
    }
    s_afe.data = s_afe.iface->create_from_config(afe_config);
    afe_config_print(afe_config);
    afe_config_free(afe_config);
    if (!s_afe.data) {
        log_memory_failure("AFE library creation (allocation size unknown)", 0);
        esp_sr_afe_deinit();
        return ESP_ERR_NO_MEM;
    }
    s_afe.feed_samples = (size_t)s_afe.iface->get_feed_chunksize(s_afe.data);
    s_afe.fetch_samples = (size_t)s_afe.iface->get_fetch_chunksize(s_afe.data);
    if (s_afe.feed_samples == 0 || s_afe.fetch_samples == 0) {
        esp_sr_afe_deinit();
        return ESP_ERR_INVALID_SIZE;
    }
    const size_t feed_bytes = s_afe.feed_samples * sizeof(int16_t);
    const size_t packet_size = sizeof(afe_output_packet_t) +
        s_afe.fetch_samples * sizeof(int16_t);
    const size_t queue_bytes = AFE_OUTPUT_QUEUE_DEPTH * packet_size;
    bool feed_psram, fetch_psram, output_psram;
    s_afe.feed_buffer = pcm_calloc(feed_bytes, &feed_psram);
    if (!s_afe.feed_buffer) {
        log_memory_failure("AFE feed buffer allocation", feed_bytes);
        goto no_mem;
    }
    s_afe.fetch_packet = pcm_calloc(packet_size, &fetch_psram);
    if (!s_afe.fetch_packet) {
        log_memory_failure("AFE fetch packet allocation", packet_size);
        goto no_mem;
    }
    s_afe.output_packet = pcm_calloc(packet_size, &output_psram);
    if (!s_afe.output_packet) {
        log_memory_failure("AFE output packet allocation", packet_size);
        goto no_mem;
    }
    /* These PCM packets are used only by tasks. Keep the queue control block
     * in internal BSS, while external payload storage preserves internal RAM
     * for the library, task stacks and DMA.
     */
    bool queue_psram = true;
    s_afe.output_queue_storage = heap_caps_malloc(queue_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_afe.output_queue_storage) {
        queue_psram = false;
        s_afe.output_queue_storage = heap_caps_malloc(queue_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!s_afe.output_queue_storage) {
        log_memory_failure("AFE output queue storage allocation", queue_bytes);
        goto no_mem;
    }
    s_afe.output_queue = xQueueCreateStatic(AFE_OUTPUT_QUEUE_DEPTH, packet_size,
        s_afe.output_queue_storage, &s_afe.output_queue_control);
    if (!s_afe.output_queue) {
        log_memory_failure("AFE output queue creation", queue_bytes);
        goto no_mem;
    }
    s_afe.fetch_started = true;
    xTaskNotifyGive(s_afe.fetch_task);
    ESP_LOGI(TAG, "bridge ready: feed_bytes=%u packet_bytes=%u queue_depth=%u queue_bytes=%u "
             "feed_storage=%s fetch_storage=%s output_storage=%s queue_storage=%s",
             (unsigned)feed_bytes, (unsigned)packet_size, AFE_OUTPUT_QUEUE_DEPTH,
             (unsigned)queue_bytes, feed_psram ? "PSRAM" : "internal",
             fetch_psram ? "PSRAM" : "internal", output_psram ? "PSRAM" : "internal",
             queue_psram ? "PSRAM" : "internal");
    printf("CONFIG,afe,feed_samples,%u,fetch_samples,%u,ns,%d,agc,%d,vad,%d,"
           "agc_target_dbfs,-%d,agc_compression_db,%d,vad_backend,%s,"
           "vad_model,%s,vad_energy_threshold_dbfs,%.1f\n",
           (unsigned)s_afe.feed_samples, (unsigned)s_afe.fetch_samples,
           config->enable_ns, config->enable_agc, config->enable_vad,
           config->agc_target_level_dbfs, config->agc_compression_gain_db,
           config->enable_vad ? "VADNet" : "disabled",
           vad_model_log,
           (double)AFE_VAD_ENERGY_THRESHOLD_DEFAULT);
    return ESP_OK;

no_mem:
    esp_sr_afe_deinit();
    return ESP_ERR_NO_MEM;
}

void esp_sr_afe_deinit(void)
{
    if (s_afe.fetch_task) {
        atomic_store(&s_afe.stop_fetch, true);
        if (s_afe.fetch_started) {
            xSemaphoreTake(s_afe.fetch_stopped, portMAX_DELAY);
        } else {
            /* The unreleased worker only waits for its startup notification,
             * and therefore cannot be using partially initialized resources.
             */
            vTaskDelete(s_afe.fetch_task);
        }
    }
    if (s_afe.iface && s_afe.data) {
        s_afe.iface->destroy(s_afe.data);
    }
    if (s_afe.output_queue) {
        vQueueDelete(s_afe.output_queue);
    }
    heap_caps_free(s_afe.output_queue_storage);
    heap_caps_free(s_afe.feed_buffer);
    heap_caps_free(s_afe.fetch_packet);
    heap_caps_free(s_afe.output_packet);
    if (s_afe.fetch_stopped) {
        vSemaphoreDelete(s_afe.fetch_stopped);
    }
    if (s_afe.models) {
        esp_srmodel_deinit(s_afe.models);
    }
    memset(&s_afe, 0, sizeof(s_afe));
}

bool esp_sr_afe_is_initialized(void)
{
    return s_afe.data != NULL;
}

size_t esp_sr_afe_feed_chunk_samples(void)
{
    return s_afe.feed_samples;
}

size_t esp_sr_afe_fetch_chunk_samples(void)
{
    return s_afe.fetch_samples;
}

static esp_err_t fetch_available(esp_sr_output_cb_t callback, void *ctx)
{
    afe_output_packet_t *packet = s_afe.output_packet;
    while (xQueueReceive(s_afe.output_queue, packet, 0) == pdTRUE) {
        callback(packet->samples, packet->count,
                 packet->vad_state, packet->vad_cache_bytes,
                 s_afe.next_output_sample, ctx);
        s_afe.next_output_sample += packet->count;
        s_afe.output_samples += packet->count;
        s_afe.last_progress_feed = s_afe.fed_samples;
    }
    return ESP_OK;
}

esp_err_t esp_sr_afe_feed(const int16_t *samples, size_t count,
                          uint64_t first_sample_index,
                          esp_sr_output_cb_t callback, void *ctx)
{
    ESP_RETURN_ON_FALSE(s_afe.data && samples && callback, ESP_ERR_INVALID_STATE,
                        TAG, "AFE is not ready");
    ESP_RETURN_ON_ERROR(fetch_available(callback, ctx), TAG, "AFE output failed");
    size_t offset = 0;
    while (offset < count) {
        if (s_afe.feed_used == 0) {
            s_afe.feed_first_sample = first_sample_index + offset;
        } else if (first_sample_index + offset != s_afe.feed_first_sample + s_afe.feed_used) {
            s_afe.feed_used = 0;
            s_afe.feed_first_sample = first_sample_index + offset;
            ESP_LOGW(TAG, "discontinuous input; partial AFE feed discarded");
        }
        const size_t remaining = s_afe.feed_samples - s_afe.feed_used;
        const size_t copy_count = (count - offset) < remaining ? (count - offset) : remaining;
        memcpy(&s_afe.feed_buffer[s_afe.feed_used], &samples[offset],
               copy_count * sizeof(int16_t));
        s_afe.feed_used += copy_count;
        offset += copy_count;
        if (s_afe.feed_used == s_afe.feed_samples) {
            if (!s_afe.output_index_initialized) {
                s_afe.next_output_sample = s_afe.feed_first_sample;
                s_afe.output_index_initialized = true;
            }
            int fed = s_afe.iface->feed(s_afe.data, s_afe.feed_buffer);
            s_afe.feed_used = 0;
            if (fed <= 0) {
                ESP_LOGE(TAG, "AFE feed failed: %d", fed);
                return ESP_FAIL;
            }
            s_afe.fed_samples += s_afe.feed_samples;
            ESP_RETURN_ON_ERROR(fetch_available(callback, ctx), TAG, "AFE fetch failed");
        }
    }
    if (s_afe.fed_samples - s_afe.last_progress_feed >= AUDIO_SAMPLE_RATE_HZ) {
        ESP_LOGW(TAG, "AFE output stalled: fed=%" PRIu64 ", output=%" PRIu64
                 ", last_fetch_error=%d", s_afe.fed_samples, s_afe.output_samples,
                 atomic_load(&s_afe.last_fetch_error));
        s_afe.last_progress_feed = s_afe.fed_samples;
    }
    return ESP_OK;
}
