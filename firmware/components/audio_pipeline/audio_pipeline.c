#include "audio_pipeline.h"
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include "audio_capture.h"
#include "audio_dsp.h"
#include "audio_metrics.h"
#include "audio_ring_buffer.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "vad_service.h"

#define PIPELINE_QUEUE_DEPTH 12
#define MAX_PREROLL_MS 2000
typedef struct {
    uint64_t first_sample_index;
    int64_t timestamp_us;
    uint16_t count;
    int16_t samples[AUDIO_FRAME_SAMPLES];
} pipeline_packet_t;
typedef struct {
    audio_pipeline_config_t config;
    QueueHandle_t queue;
    StaticQueue_t queue_control;
    uint8_t *queue_storage;
    TaskHandle_t task;
    bool initialized;
    volatile bool running;
    volatile bool stop_requested;
    bool export_pause_requested;
    bool export_resources_released;
    bool export_was_running;
    audio_runtime_stats_t stats;
    int16_t *preroll_copy;
    size_t preroll_capacity;
    uint64_t event_id;
    bool event_queued;
    bool event_failed;
    sd_event_result_t failed_result;
    uint64_t latest_raw_end;
} pipeline_state_t;
static pipeline_state_t s_pipeline;
static const char *TAG = "event_audio";

static void log_audio_memory(const char *phase)
{
    const uint32_t internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const uint32_t psram = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    const uint32_t dma = MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA;
    ESP_LOGI(TAG, "heap phase=%s internal_free=%u internal_largest=%u psram_free=%u "
             "psram_largest=%u dma_free=%u dma_largest=%u", phase,
             (unsigned)heap_caps_get_free_size(internal),
             (unsigned)heap_caps_get_largest_free_block(internal),
             (unsigned)heap_caps_get_free_size(psram),
             (unsigned)heap_caps_get_largest_free_block(psram),
             (unsigned)heap_caps_get_free_size(dma),
             (unsigned)heap_caps_get_largest_free_block(dma));
}

static bool capture_frame(const audio_frame_t *frame, void *ctx)
{
    (void)ctx;
    if (!s_pipeline.running || frame->sample_count > AUDIO_FRAME_SAMPLES) return false;
    pipeline_packet_t packet = {.first_sample_index = frame->first_sample_index,
        .timestamp_us = frame->timestamp_us, .count = (uint16_t)frame->sample_count};
    memcpy(packet.samples, frame->samples, frame->sample_count * sizeof(int16_t));
    if (xQueueSend(s_pipeline.queue, &packet, 0) != pdTRUE) {
        printf("ERROR,audio_queue_overrun,sample,%" PRIu64 ",count,%u\n",
               frame->first_sample_index, (unsigned)frame->sample_count);
        return false;
    }
    UBaseType_t waiting = uxQueueMessagesWaiting(s_pipeline.queue);
    audio_stats_lock();
    if (waiting > s_pipeline.stats.audio_queue_high_water)
        s_pipeline.stats.audio_queue_high_water = waiting;
    audio_stats_unlock();
    return true;
}
static void processed_frame(const audio_frame_t *frame, void *ctx)
{
    (void)ctx;
    audio_metrics_process_processed(frame);
    audio_stats_lock();
    s_pipeline.stats.processed_samples += frame->sample_count;
    audio_stats_unlock();
    /* Event WAV deliberately uses pre-DSP capture. USB and continuous SD are
     * absent from this integration profile, including while face work runs.
     */
}
static void report_failed_event(int64_t timestamp_us, uint64_t sample)
{
    sd_event_result_t *result = &s_pipeline.failed_result;
    result->vad_end_sample = sample;
    result->vad_end_timestamp_us = timestamp_us;
    result->error = ESP_FAIL;
    result->ready = false;
    char row[384];
    snprintf(row, sizeof(row), "%016" PRIX64 ",event_%016" PRIX64
        ".wav,RAW,16000,1,16,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
        ",%" PRId64 ",%" PRId64 ",FAILED",
        result->event_id, result->event_id, result->audio_first_sample,
        result->vad_start_sample, result->vad_end_sample, result->audio_last_sample,
        result->vad_start_timestamp_us, result->vad_end_timestamp_us);
    (void)sd_recorder_metadata_append(SD_METADATA_AUDIO_EVENTS, row);
    printf("EVENT,audio_terminal,event_id,%016" PRIX64 ",status,FAILED,error,%s\n",
           result->event_id, esp_err_to_name(result->error));
    if (s_pipeline.config.audio_terminal_cb)
        s_pipeline.config.audio_terminal_cb(result, s_pipeline.config.callback_ctx);
}
static void end_event(int64_t timestamp_us, uint64_t sample)
{
    if (!s_pipeline.event_id) return;
    if (s_pipeline.config.event_end_cb)
        s_pipeline.config.event_end_cb(s_pipeline.event_id, timestamp_us, sample,
                                      s_pipeline.config.callback_ctx);
    if (!s_pipeline.event_queued ||
        !sd_recorder_event_end(s_pipeline.event_id, timestamp_us, sample, s_pipeline.event_failed)) {
        report_failed_event(timestamp_us, sample);
    }
    s_pipeline.event_id = 0;
    s_pipeline.event_queued = false;
}
static void vad_event(audio_vad_state_t state, int64_t timestamp_us,
                      uint64_t sample_index, void *ctx)
{
    (void)ctx;
    if (state != AUDIO_VAD_SPEECH) { end_event(timestamp_us, sample_index); return; }
    if (s_pipeline.event_id) end_event(timestamp_us, sample_index);
    uint64_t id = s_pipeline.config.event_open_cb ?
        s_pipeline.config.event_open_cb(timestamp_us, sample_index, s_pipeline.config.callback_ctx) : 0;
    if (!id) {
        printf("ERROR,event_open_rejected,sample,%" PRIu64 "\n", sample_index);
        return;
    }
    s_pipeline.event_id = id;
    s_pipeline.event_failed = false;
    audio_preroll_metadata_t metadata = {0};
    size_t copied = audio_ring_buffer_copy_event_start(sample_index, s_pipeline.preroll_copy,
                                                     s_pipeline.preroll_capacity, &metadata);
    audio_frame_t start = {
        .samples = s_pipeline.preroll_copy, .sample_count = copied,
        .sample_rate = AUDIO_SAMPLE_RATE_HZ, .first_sample_index = metadata.saved_first_sample,
        .timestamp_us = timestamp_us,
    };
    sd_event_metadata_t owner = {
        .event_id = id, .trigger_sample = sample_index,
        .saved_first_sample = metadata.saved_first_sample,
        .vad_start_timestamp_us = timestamp_us,
        .requested_preroll_ms = metadata.requested_preroll_ms,
        .actual_preroll_ms = metadata.actual_preroll_ms,
        .wrap_count = metadata.wrap_count, .overrun_count = metadata.overrun_count,
    };
    s_pipeline.failed_result = (sd_event_result_t) {
        .event_id = id, .audio_first_sample = metadata.saved_first_sample,
        .audio_last_sample = metadata.saved_first_sample, .vad_start_sample = sample_index,
        .vad_start_timestamp_us = timestamp_us, .error = ESP_FAIL,
    };
    snprintf(s_pipeline.failed_result.audio_path, sizeof(s_pipeline.failed_result.audio_path),
             "/sd/event_%016" PRIX64 ".wav", id);
    s_pipeline.event_queued = sd_recorder_event_begin(&start, &owner);
    if (!s_pipeline.event_queued) {
        audio_stats_lock();
        s_pipeline.stats.dropped_samples += copied;
        s_pipeline.stats.queue_overruns++;
        audio_stats_unlock();
        printf("ERROR,event_audio_begin,event_id,%016" PRIX64 ",sd_running,%d\n",
               id, sd_recorder_is_running());
    }
    printf("EVENT,preroll,event_id,%016" PRIX64 ",trigger,%" PRIu64
           ",saved_first,%" PRIu64 ",requested_ms,%" PRIu32 ",actual_ms,%" PRIu32
           ",samples,%u,wraps,%" PRIu32 ",overruns,%" PRIu32 "\n",
           id, sample_index, metadata.saved_first_sample, metadata.requested_preroll_ms,
           metadata.actual_preroll_ms, (unsigned)copied, metadata.wrap_count, metadata.overrun_count);
}
static void pipeline_task(void *arg)
{
    (void)arg;
    pipeline_packet_t packet;
    for (;;) {
        /* Reserve the internal stack during init; begin only after DSP is ready. */
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        while (!s_pipeline.stop_requested || uxQueueMessagesWaiting(s_pipeline.queue)) {
            if (xQueueReceive(s_pipeline.queue, &packet, pdMS_TO_TICKS(100)) != pdTRUE) continue;
            audio_frame_t raw = {.samples = packet.samples, .sample_count = packet.count,
                .sample_rate = AUDIO_SAMPLE_RATE_HZ, .first_sample_index = packet.first_sample_index,
                .timestamp_us = packet.timestamp_us};
            s_pipeline.latest_raw_end = raw.first_sample_index + raw.sample_count;
            if (s_pipeline.event_id && s_pipeline.event_queued &&
                !sd_recorder_event_submit(s_pipeline.event_id, &raw)) s_pipeline.event_failed = true;
            audio_ring_buffer_push(&raw);
            audio_metrics_process_raw(&raw);
            esp_err_t error = audio_dsp_process(&raw);
            if (error != ESP_OK) {
                audio_stats_lock();
                s_pipeline.stats.dropped_samples += raw.sample_count;
                audio_stats_unlock();
                s_pipeline.event_failed = true;
                printf("ERROR,dsp_process,sample,%" PRIu64 ",%s\n",
                       raw.first_sample_index, esp_err_to_name(error));
            }
        }
        s_pipeline.event_failed = true;
        end_event(esp_timer_get_time(), s_pipeline.latest_raw_end);
        /* No queue/DSP/recorder access after publishing quiescence. */
        s_pipeline.running = false;
    }
}
audio_pipeline_config_t audio_pipeline_default_config(void)
{
    return (audio_pipeline_config_t) {
        .dsp = {.mode = AUDIO_DSP_AGC_NS, .dc_block_enabled = true, .fixed_gain_db = 0,
                .agc_target_level_dbfs = 3, .agc_compression_gain_db = 9},
        .preroll_ms = 1000, .metrics_enabled = false, .vad_enabled = true,
    };
}
esp_err_t audio_pipeline_init(const audio_pipeline_config_t *config)
{
    ESP_RETURN_ON_FALSE(config && config->event_open_cb && !s_pipeline.queue,
                        ESP_ERR_INVALID_ARG, TAG, "config/open callback required or already initialized");
    esp_err_t ret = ESP_OK;
    s_pipeline.config = *config;
    printf("CONFIG,audio,sample_rate,16000,channels,1,bits,16,event_source,RAW,"
           "preroll_ms,%" PRIu32 ",dsp,%s,vad,%d,continuous,0,usb,0\n",
           config->preroll_ms, audio_dsp_mode_name(config->dsp.mode), config->vad_enabled);
    const size_t queue_bytes = PIPELINE_QUEUE_DEPTH * sizeof(pipeline_packet_t);
    /* Capture submits from its task, so only queue control needs internal RAM. */
    bool queue_in_psram = true;
    s_pipeline.queue_storage = heap_caps_malloc(queue_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_pipeline.queue_storage) {
        queue_in_psram = false;
        s_pipeline.queue_storage = heap_caps_malloc(queue_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!s_pipeline.queue_storage) {
        ESP_LOGE(TAG, "audio queue storage allocation failed: bytes=%u", (unsigned)queue_bytes);
        log_audio_memory("audio_queue_failed");
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }
    s_pipeline.queue = xQueueCreateStatic(PIPELINE_QUEUE_DEPTH, sizeof(pipeline_packet_t),
                                         s_pipeline.queue_storage, &s_pipeline.queue_control);
    if (!s_pipeline.queue) {
        heap_caps_free(s_pipeline.queue_storage);
        s_pipeline.queue_storage = NULL;
        ESP_LOGE(TAG, "audio queue creation failed: bytes=%u", (unsigned)queue_bytes);
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }
    ESP_LOGI(TAG, "queue ready: depth=%u bytes=%u storage=%s", (unsigned)PIPELINE_QUEUE_DEPTH,
             (unsigned)queue_bytes, queue_in_psram ? "PSRAM" : "internal");
    s_pipeline.preroll_capacity = AUDIO_SAMPLE_RATE_HZ * MAX_PREROLL_MS / 1000U;
    size_t bytes = s_pipeline.preroll_capacity * sizeof(int16_t);
    s_pipeline.preroll_copy = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_pipeline.preroll_copy) s_pipeline.preroll_copy = malloc(bytes);
    ESP_GOTO_ON_FALSE(s_pipeline.preroll_copy, ESP_ERR_NO_MEM, fail, TAG, "pre-roll allocation failed");
    ESP_GOTO_ON_ERROR(audio_ring_buffer_init(MAX_PREROLL_MS, &s_pipeline.stats), fail, TAG, "ring init failed");
    ESP_GOTO_ON_ERROR(audio_ring_buffer_set_preroll(config->preroll_ms), fail, TAG, "invalid pre-roll");
    ESP_GOTO_ON_FALSE(xTaskCreatePinnedToCore(pipeline_task, "audio_processing", 8192, NULL, 15,
                                            &s_pipeline.task, 1) == pdPASS,
                      ESP_ERR_NO_MEM, fail, TAG, "audio_processing stack reservation failed: bytes=8192");
    log_audio_memory("before_capture_init");
    /* DMA buffers and both remaining worker stacks must precede AFE's many
     * internal allocations. I2S and processing remain gated until start.
     */
    ESP_GOTO_ON_ERROR(audio_capture_init(&(audio_capture_config_t) {
        .frame_cb = capture_frame, .stats = &s_pipeline.stats, .read_timeout_ms = 1000,
    }), fail, TAG, "capture init failed");
    ESP_GOTO_ON_ERROR(sd_recorder_init(&(sd_recorder_config_t) {
        .stats = &s_pipeline.stats, .terminal_cb = config->audio_terminal_cb,
        .callback_ctx = config->callback_ctx,
    }), fail, TAG, "recorder init failed");
    esp_err_t storage = sd_recorder_start();
    if (storage != ESP_OK)
        ESP_LOGW(TAG, "SD unavailable (%s); VAD Events retain FAILED audio ownership", esp_err_to_name(storage));
    ESP_GOTO_ON_ERROR(vad_service_init(&(vad_service_config_t) {
        .event_cb = vad_event, .initial_enabled = config->vad_enabled,
    }), fail, TAG, "VAD init failed");
    log_audio_memory("before_dsp_init");
    ESP_GOTO_ON_ERROR(audio_dsp_init(&(audio_dsp_init_config_t) {
        .initial_config = config->dsp, .vad_enabled = config->vad_enabled,
        .output_cb = processed_frame, .vad_cb = vad_service_on_afe_result,
        .stats = &s_pipeline.stats,
    }), fail, TAG, "DSP init failed");
    audio_metrics_set_enabled(config->metrics_enabled);
    vad_service_set_metrics_enabled(config->metrics_enabled);
    s_pipeline.initialized = true;
    log_audio_memory("audio_initialized");
    return ESP_OK;

fail:
    ESP_LOGE(TAG, "audio initialization failed: %s; releasing partial startup", esp_err_to_name(ret));
    log_audio_memory("audio_init_failed");
    esp_err_t cleanup = audio_pipeline_deinit();
    if (cleanup != ESP_OK)
        ESP_LOGE(TAG, "partial startup cleanup failed: %s", esp_err_to_name(cleanup));
    return ret;
}
esp_err_t audio_pipeline_start(void)
{
    if (s_pipeline.export_pause_requested) return ESP_ERR_INVALID_STATE;
    if (s_pipeline.running) return ESP_OK;
    if (!s_pipeline.initialized || !s_pipeline.task) return ESP_ERR_INVALID_STATE;
    if (!sd_recorder_is_running()) (void)sd_recorder_start();
    s_pipeline.stop_requested = false;
    s_pipeline.running = true;
    xTaskNotifyGive(s_pipeline.task);
    esp_err_t error = audio_capture_start();
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "audio capture start failed: %s", esp_err_to_name(error));
        log_audio_memory("audio_capture_start_failed");
        esp_err_t cleanup = audio_pipeline_deinit();
        if (cleanup != ESP_OK)
            ESP_LOGE(TAG, "audio start cleanup failed: %s", esp_err_to_name(cleanup));
    }
    return error;
}
esp_err_t audio_pipeline_stop(void)
{
    ESP_RETURN_ON_ERROR(audio_capture_stop(), TAG, "capture stop failed");
    s_pipeline.stop_requested = true;
    for (int i = 0; s_pipeline.running && i < 200; ++i) vTaskDelay(pdMS_TO_TICKS(10));
    return s_pipeline.running ? ESP_ERR_TIMEOUT : sd_recorder_stop();
}
esp_err_t audio_pipeline_deinit(void)
{
    /* A failed join must preserve resources still reachable by a worker. */
    ESP_RETURN_ON_ERROR(audio_pipeline_stop(), TAG, "audio deinit stop failed");
    if (s_pipeline.task) vTaskDelete(s_pipeline.task);
    audio_dsp_deinit();
    audio_capture_deinit();
    sd_recorder_deinit();
    audio_ring_buffer_deinit();
    free(s_pipeline.preroll_copy);
    if (s_pipeline.queue) vQueueDelete(s_pipeline.queue);
    heap_caps_free(s_pipeline.queue_storage);
    memset(&s_pipeline, 0, sizeof(s_pipeline));
    return ESP_OK;
}
bool audio_pipeline_is_running(void) { return s_pipeline.running; }

static esp_err_t stop_export_workers(void)
{
    ESP_RETURN_ON_ERROR(audio_capture_stop(), TAG, "export capture stop failed");
    s_pipeline.stop_requested = true;
    for (int i = 0; s_pipeline.running && i < 200; ++i) vTaskDelay(pdMS_TO_TICKS(10));
    return s_pipeline.running ? ESP_ERR_TIMEOUT : ESP_OK;
}

esp_err_t audio_pipeline_suspend_for_export(void)
{
    if (!s_pipeline.initialized) return ESP_ERR_INVALID_STATE;
    if (!s_pipeline.export_pause_requested) {
        // Admission is already closed and EventManager acknowledged no owners.
        // Refuse misuse instead of terminating an utterance to make a snapshot.
        if (s_pipeline.event_id) return ESP_ERR_INVALID_STATE;
        s_pipeline.export_was_running = s_pipeline.running;
        s_pipeline.export_pause_requested = true;
    }
    ESP_RETURN_ON_ERROR(stop_export_workers(), TAG, "export worker drain failed");
    if (!s_pipeline.export_resources_released) {
        audio_dsp_deinit();
        audio_capture_deinit();
        s_pipeline.export_resources_released = true;
    }
    log_audio_memory("export_suspended");
    return ESP_OK;
}

esp_err_t audio_pipeline_resume_after_export(void)
{
    if (!s_pipeline.export_pause_requested) return ESP_OK;
    // Also joins a partially completed suspend or a failed prior restart.
    ESP_RETURN_ON_ERROR(stop_export_workers(), TAG, "export resume join failed");
    if (!s_pipeline.export_resources_released) {
        audio_dsp_deinit();
        audio_capture_deinit();
        s_pipeline.export_resources_released = true;
    }
    audio_ring_buffer_reset();
    xQueueReset(s_pipeline.queue);
    ESP_RETURN_ON_ERROR(vad_service_init(&(vad_service_config_t) {
        .event_cb = vad_event, .initial_enabled = s_pipeline.config.vad_enabled,
    }), TAG, "export VAD reset failed");
    vad_service_set_metrics_enabled(s_pipeline.config.metrics_enabled);
    esp_err_t error = audio_capture_init(&(audio_capture_config_t) {
        .frame_cb = capture_frame, .stats = &s_pipeline.stats, .read_timeout_ms = 1000,
    });
    if (error == ESP_OK) error = audio_dsp_init(&(audio_dsp_init_config_t) {
        .initial_config = s_pipeline.config.dsp, .vad_enabled = s_pipeline.config.vad_enabled,
        .output_cb = processed_frame, .vad_cb = vad_service_on_afe_result,
        .stats = &s_pipeline.stats,
    });
    if (error != ESP_OK) {
        audio_dsp_deinit();
        audio_capture_deinit();
        log_audio_memory("export_resume_failed");
        return error;
    }
    s_pipeline.export_resources_released = false;
    if (s_pipeline.export_was_running) {
        s_pipeline.stop_requested = false;
        s_pipeline.running = true;
        xTaskNotifyGive(s_pipeline.task);
        error = audio_capture_start();
        if (error != ESP_OK) {
            // Keep the pause flag so a later CANCEL can retry restoration.
            (void)stop_export_workers();
            return error;
        }
    }
    s_pipeline.export_pause_requested = false;
    log_audio_memory("export_resumed");
    return ESP_OK;
}

esp_err_t audio_pipeline_set_dsp(const audio_dsp_config_t *config) {
    if (s_pipeline.export_pause_requested) return ESP_ERR_INVALID_STATE;
    esp_err_t error = audio_dsp_set_config(config);
    if (error == ESP_OK) s_pipeline.config.dsp = *config;
    return error;
}
esp_err_t audio_pipeline_set_preroll(uint32_t ms) {
    esp_err_t error = audio_ring_buffer_set_preroll(ms);
    if (error == ESP_OK) s_pipeline.config.preroll_ms = ms;
    return error;
}
esp_err_t audio_pipeline_set_metrics(bool enabled)
{
    audio_metrics_set_enabled(enabled);
    vad_service_set_metrics_enabled(enabled);
    s_pipeline.config.metrics_enabled = enabled;
    return ESP_OK;
}
esp_err_t audio_pipeline_set_vad(bool enabled)
{
    /* Runtime disabling must not strand the current writer owner. */
    if (s_pipeline.export_pause_requested || (!enabled && s_pipeline.event_id)) return ESP_ERR_INVALID_STATE;
    esp_err_t error = vad_service_set_enabled(enabled);
    if (error == ESP_OK) s_pipeline.config.vad_enabled = enabled;
    return error;
}
audio_runtime_stats_t audio_pipeline_get_stats(void) { return audio_stats_snapshot(&s_pipeline.stats); }
void audio_pipeline_print_status(void)
{
    audio_runtime_stats_t s = audio_pipeline_get_stats();
    printf("STATUS,audio,running,%d,captured_samples,%" PRIu64 ",processed_samples,%" PRIu64
           ",dropped_samples,%" PRIu64 ",queue_overruns,%" PRIu32 ",queue_high_water,%" PRIu32
           ",ring_overruns,%" PRIu32 ",sd_queue_high_water,%" PRIu32 ",sd_short_writes,%" PRIu32
           ",sd_metadata_drops,%" PRIu32 ",sd_write_max_us,%" PRIu32
           ",capture_max_us,%" PRIu32 ",dsp_max_us,%" PRIu32
           ",internal_free,%u,internal_min,%u,psram_free,%u,psram_min,%u\n",
           s_pipeline.running, s.captured_samples, s.processed_samples, s.dropped_samples,
           s.queue_overruns, s.audio_queue_high_water, s.ring_overruns, s.sd_queue_high_water,
           s.sd_short_writes, s.sd_metadata_drops, s.sd_max_write_us,
           s.capture_max_processing_us, s.dsp_max_processing_us,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
}
