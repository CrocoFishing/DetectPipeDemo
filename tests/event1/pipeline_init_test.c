/* Compile production pipeline and VAD transitions; model hardware dependencies
 * to check resource ownership, recording boundaries and forced completion. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pipeline_host_shims.h"
#include "audio_pipeline.h"
#include "audio_capture.h"
#include "audio_dsp.h"
#include "audio_ring_buffer.h"
#include "audio_metrics.h"
#include "vad_service.h"

static const char *calls[128];
static unsigned call_count, allocation_calls, free_calls, task_create_calls, task_delete_calls;
static unsigned capture_init_calls, dsp_init_calls;
static bool capture_live, capture_running, dsp_live, recorder_live, recorder_running, ring_live;
static bool fail_capture_init, fail_processing_task, fail_dsp_init, fail_capture_start;
static bool fail_capture_stop, fail_sd_mount;
static host_task_t processing_task;
static QueueHandle_t live_queue;
static void *allocations[16];
static unsigned live_allocations;
static unsigned event_opens, event_ends, recorder_ends, failed_terminals;
static bool accept_begin, accept_submit, accept_end, last_producer_failed;
static uint64_t saved_samples, last_end_sample;
static int64_t last_end_timestamp;
static size_t preroll_lag_samples;
static void record_call(const char *name) {
    assert(call_count < sizeof(calls) / sizeof(calls[0]));
    calls[call_count++] = name;
}
static void host_free(void *memory);
#define free host_free
#include "../../firmware/components/audio_pipeline/audio_pipeline.c"
#undef free
#include "../../firmware/components/vad/vad_service.c"

static unsigned first_call(const char *name) {
    for (unsigned i = 0; i < call_count; ++i) if (!strcmp(calls[i], name)) return i;
    assert(!"expected dependency call missing");
    return 0;
}
static void assert_before(const char *first, const char *second) {
    assert(first_call(first) < first_call(second));
}
void *heap_caps_malloc(size_t bytes, unsigned caps) {
    assert(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) ||
           caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    assert(allocation_calls < sizeof(allocations) / sizeof(allocations[0]));
    void *memory = malloc(bytes);
    assert(memory);
    allocations[allocation_calls++] = memory;
    ++live_allocations;
    record_call("allocate");
    return memory;
}
static void host_free(void *memory) {
    if (!memory) return;
    for (unsigned i = 0; i < allocation_calls; ++i) {
        if (allocations[i] != memory) continue;
        assert(!capture_live && !dsp_live && !recorder_live && !ring_live);
        assert(!processing_task.allocated);
        if (memory == s_pipeline.queue_storage) assert(live_queue == NULL);
        allocations[i] = NULL;
        --live_allocations;
        ++free_calls;
        record_call("free");
        free(memory);
        return;
    }
    assert(!"unknown or double free");
}
void heap_caps_free(void *memory) { host_free(memory); }
QueueHandle_t xQueueCreateStatic(UBaseType_t depth, UBaseType_t item_bytes,
                                uint8_t *storage, StaticQueue_t *control) {
    assert(!live_queue && storage && control);
    assert(depth == PIPELINE_QUEUE_DEPTH && item_bytes == sizeof(pipeline_packet_t));
    *control = (StaticQueue_t) {.storage = storage, .depth = depth, .item_bytes = item_bytes};
    live_queue = control;
    record_call("queue_create");
    return control;
}
void vQueueDelete(QueueHandle_t queue) {
    assert(queue == live_queue);
    assert(!capture_live && !dsp_live && !recorder_live && !ring_live);
    live_queue = NULL;
    record_call("queue_delete");
}
BaseType_t xTaskCreatePinnedToCore(void (*entry)(void *), const char *name,
        uint32_t stack, void *argument, UBaseType_t priority, TaskHandle_t *task, int core) {
    (void)entry; (void)argument;
    assert(!strcmp(name, "audio_processing") && stack == 8192 && priority == 15 && core == 1);
    assert(!dsp_live && !processing_task.allocated);
    ++task_create_calls;
    record_call("processing_task_create");
    if (fail_processing_task) return pdFALSE;
    processing_task = (host_task_t) {.allocated = true};
    *task = &processing_task;
    return pdPASS;
}
void vTaskDelete(TaskHandle_t task) {
    assert(task == &processing_task && processing_task.allocated);
    assert(!capture_running);
    processing_task.allocated = false;
    ++task_delete_calls;
    record_call("processing_task_delete");
}
BaseType_t xTaskNotifyGive(TaskHandle_t task) {
    assert(task == &processing_task && processing_task.allocated);
    ++task->notifications;
    record_call("processing_notify");
    return pdPASS;
}
void vTaskDelay(TickType_t wait) {
    (void)wait;
    /* The worker has no packets in these lifecycle cases. Its stop handshake
     * is modeled here; its task allocation must survive normal stop. */
    if (s_pipeline.stop_requested) s_pipeline.running = false;
}

void audio_stats_lock(void) {}
void audio_stats_unlock(void) {}
audio_runtime_stats_t audio_stats_snapshot(const audio_runtime_stats_t *stats) { return *stats; }
const char *audio_dsp_mode_name(audio_dsp_mode_t mode) { (void)mode; return "RAW"; }
esp_err_t audio_capture_init(const audio_capture_config_t *config) {
    assert(config && config->frame_cb && config->stats && !capture_live && !dsp_live);
    capture_live = true; /* Also models partial resource ownership on error. */
    ++capture_init_calls;
    record_call("capture_init");
    return fail_capture_init ? ESP_ERR_NO_MEM : ESP_OK;
}
esp_err_t audio_capture_start(void) {
    assert(capture_live && dsp_live && processing_task.allocated);
    record_call("capture_start");
    if (fail_capture_start) return ESP_FAIL;
    capture_running = true;
    return ESP_OK;
}
esp_err_t audio_capture_stop(void) {
    record_call("capture_stop");
    if (fail_capture_stop) return ESP_ERR_TIMEOUT;
    capture_running = false;
    return ESP_OK;
}
bool audio_capture_is_running(void) { return capture_running; }
void audio_capture_deinit(void) {
    assert(!capture_running);
    capture_live = false;
    record_call("capture_deinit");
}
esp_err_t audio_dsp_init(const audio_dsp_init_config_t *config) {
    assert(config && capture_live && processing_task.allocated && recorder_live && ring_live);
    assert(!dsp_live);
    dsp_live = true; /* Failed AFE creation can own resources needing teardown. */
    ++dsp_init_calls;
    record_call("dsp_init");
    return fail_dsp_init ? ESP_ERR_NO_MEM : ESP_OK;
}
void audio_dsp_deinit(void) {
    assert(!capture_running && (!processing_task.allocated ||
           (s_pipeline.export_pause_requested && !s_pipeline.running)));
    dsp_live = false;
    record_call("dsp_deinit");
}
esp_err_t audio_dsp_set_config(const audio_dsp_config_t *config) { (void)config; return ESP_OK; }
esp_err_t audio_dsp_set_vad_enabled(bool enabled) { (void)enabled; return ESP_OK; }
esp_err_t audio_dsp_process(const audio_frame_t *input) { (void)input; return ESP_OK; }
esp_err_t audio_ring_buffer_init(uint32_t maximum_ms, audio_runtime_stats_t *stats) {
    assert(maximum_ms == MAX_PREROLL_MS && stats && !ring_live);
    ring_live = true;
    record_call("ring_init");
    return ESP_OK;
}
void audio_ring_buffer_deinit(void) {
    assert(!capture_live && !processing_task.allocated && !dsp_live && !recorder_live);
    ring_live = false;
    record_call("ring_deinit");
}
void audio_ring_buffer_reset(void) {
    assert(ring_live && !capture_running && !s_pipeline.running);
    record_call("ring_reset");
}
esp_err_t audio_ring_buffer_set_preroll(uint32_t ms) { (void)ms; return ESP_OK; }
void audio_ring_buffer_push(const audio_frame_t *frame) { (void)frame; }
size_t audio_ring_buffer_copy_event_start(uint64_t sample, int16_t *dest, size_t count,
                                        audio_preroll_metadata_t *metadata) {
    (void)dest;
    const uint64_t first = sample > AUDIO_SAMPLE_RATE_HZ ? sample - AUDIO_SAMPLE_RATE_HZ : 0;
    size_t copied = (size_t)(sample - first) + preroll_lag_samples;
    assert(copied <= count);
    *metadata = (audio_preroll_metadata_t) {.saved_first_sample = first};
    return copied;
}
esp_err_t sd_recorder_init(const sd_recorder_config_t *config) {
    assert(config && config->stats && !recorder_live);
    recorder_live = true;
    record_call("recorder_init");
    return ESP_OK;
}
esp_err_t sd_recorder_start(void) {
    assert(recorder_live);
    record_call("recorder_start");
    if (fail_sd_mount) return ESP_ERR_TIMEOUT;
    recorder_running = true;
    return ESP_OK;
}
esp_err_t sd_recorder_stop(void) { recorder_running = false; record_call("recorder_stop"); return ESP_OK; }
void sd_recorder_deinit(void) {
    assert(!capture_live && !processing_task.allocated && !dsp_live);
    recorder_live = recorder_running = false;
    record_call("recorder_deinit");
}
bool sd_recorder_is_running(void) { return recorder_running; }
bool sd_recorder_event_begin(const audio_frame_t *frame, const sd_event_metadata_t *metadata) {
    assert(frame && metadata && metadata->event_id == s_pipeline.event_id);
    if (accept_begin) saved_samples += frame->sample_count;
    return accept_begin;
}
bool sd_recorder_event_submit(uint64_t event_id, const audio_frame_t *frame) {
    assert(event_id == s_pipeline.event_id && frame && frame->sample_count > 0);
    assert(frame->first_sample_index + frame->sample_count <= s_pipeline.event_end_sample);
    if (accept_submit) saved_samples += frame->sample_count;
    return accept_submit;
}
bool sd_recorder_event_end(uint64_t event_id, int64_t timestamp, uint64_t sample, bool failed) {
    assert(event_id == s_pipeline.event_id);
    ++recorder_ends;
    last_end_timestamp = timestamp;
    last_end_sample = sample;
    last_producer_failed = failed;
    return accept_end;
}
bool sd_recorder_metadata_append(sd_metadata_kind_t kind, const char *row) { (void)kind; (void)row; return false; }
void audio_metrics_set_enabled(bool enabled) { (void)enabled; }
void audio_metrics_process_raw(const audio_frame_t *frame) { (void)frame; }
void audio_metrics_process_processed(const audio_frame_t *frame) { (void)frame; }

static uint64_t open_event(int64_t timestamp, uint64_t sample, void *ctx) {
    (void)timestamp; (void)sample; (void)ctx; return ++event_opens;
}
static void event_end(uint64_t id, int64_t timestamp, uint64_t sample, void *ctx) {
    (void)timestamp; (void)sample; (void)ctx;
    assert(id == s_pipeline.event_id);
    ++event_ends;
}
static void audio_failed(const sd_event_result_t *result, void *ctx) {
    (void)ctx;
    assert(result && !result->ready && result->event_id == s_pipeline.event_id);
    ++failed_terminals;
}
static audio_pipeline_config_t config(void) {
    audio_pipeline_config_t result = audio_pipeline_default_config();
    result.event_open_cb = open_event;
    result.event_end_cb = event_end;
    result.audio_terminal_cb = audio_failed;
    return result;
}
static void assert_resources(bool retained) {
    assert(capture_live == retained && dsp_live == retained && recorder_live == retained && ring_live == retained);
    assert(processing_task.allocated == retained);
    assert((live_queue != NULL) == retained);
    assert(live_allocations == (retained ? 2U : 0U));
}
static void reset_case(void) {
    assert_resources(false);
    fail_capture_init = fail_processing_task = fail_dsp_init = fail_capture_start = false;
    fail_capture_stop = fail_sd_mount = false;
    call_count = allocation_calls = free_calls = task_create_calls = task_delete_calls = 0;
    capture_init_calls = dsp_init_calls = 0;
    event_opens = event_ends = recorder_ends = failed_terminals = 0;
    saved_samples = last_end_sample = 0;
    last_end_timestamp = 0;
    accept_begin = accept_submit = accept_end = true;
    last_producer_failed = false;
    preroll_lag_samples = 0;
    memset(allocations, 0, sizeof(allocations));
    memset(&processing_task, 0, sizeof(processing_task));
}
static void assert_cleanup_order(void) {
    assert_before("processing_task_delete", "dsp_deinit");
    assert_before("dsp_deinit", "capture_deinit");
    assert_before("capture_deinit", "recorder_deinit");
    assert_before("recorder_deinit", "ring_deinit");
    assert_before("ring_deinit", "free");
}
static void test_order_and_retained_restart(void) {
    reset_case();
    audio_pipeline_config_t cfg = config();
    assert(audio_pipeline_init(&cfg) == ESP_OK);
    assert_before("capture_init", "dsp_init");
    assert_before("processing_task_create", "dsp_init");
    assert_before("recorder_start", "dsp_init");
    assert(processing_task.notifications == 0 && !audio_pipeline_is_running());
    assert_resources(true);
    assert(audio_pipeline_init(&cfg) == ESP_ERR_INVALID_ARG);
    assert(allocation_calls == 2 && task_create_calls == 1);
    for (unsigned run = 0; run < 2; ++run) {
        assert(audio_pipeline_start() == ESP_OK && audio_pipeline_is_running());
        assert(audio_pipeline_start() == ESP_OK);
        assert(allocation_calls == 2 && task_create_calls == 1);
        assert(audio_pipeline_stop() == ESP_OK && !audio_pipeline_is_running());
        assert_resources(true);
        assert(capture_init_calls == 1 && dsp_init_calls == 1 && task_delete_calls == 0);
    }
    assert(audio_pipeline_deinit() == ESP_OK);
    assert_resources(false);
    assert_cleanup_order();
    assert(free_calls == 2 && task_delete_calls == 1);
    assert(audio_pipeline_deinit() == ESP_OK);
    assert(free_calls == 2 && task_delete_calls == 1);
}
static void test_capture_init_failure_before_dsp(void) {
    reset_case();
    audio_pipeline_config_t cfg = config();
    fail_capture_init = true;
    assert(audio_pipeline_init(&cfg) == ESP_ERR_NO_MEM);
    assert(capture_init_calls == 1 && dsp_init_calls == 0);
    assert_resources(false);
    fail_capture_init = false;
    assert(audio_pipeline_init(&cfg) == ESP_OK);
    assert(audio_pipeline_deinit() == ESP_OK);
    assert_resources(false);
}
static void test_processing_task_failure_before_dsp(void) {
    reset_case();
    audio_pipeline_config_t cfg = config();
    fail_processing_task = true;
    assert(audio_pipeline_init(&cfg) == ESP_ERR_NO_MEM);
    assert(task_create_calls == 1 && dsp_init_calls == 0);
    assert_resources(false);
    fail_processing_task = false;
    assert(audio_pipeline_init(&cfg) == ESP_OK);
    assert(audio_pipeline_deinit() == ESP_OK);
    assert_resources(false);
}
static void test_dsp_failure_cleanup_and_immediate_retry(void) {
    reset_case();
    audio_pipeline_config_t cfg = config();
    fail_dsp_init = true;
    assert(audio_pipeline_init(&cfg) == ESP_ERR_NO_MEM);
    assert_resources(false);
    assert_cleanup_order();
    assert(free_calls == 2 && task_delete_calls == 1);
    fail_dsp_init = false;
    assert(audio_pipeline_init(&cfg) == ESP_OK);
    assert(audio_pipeline_deinit() == ESP_OK);
    assert_resources(false);
    assert(free_calls == 4 && task_delete_calls == 2);
}
static void test_start_failure_then_deinit(void) {
    reset_case();
    audio_pipeline_config_t cfg = config();
    assert(audio_pipeline_init(&cfg) == ESP_OK);
    fail_capture_start = true;
    assert(audio_pipeline_start() == ESP_FAIL && !audio_pipeline_is_running());
    assert(allocation_calls == 2 && task_create_calls == 1);
    assert_resources(false); /* start failure itself must tear down AFE/resources. */
    assert(free_calls == 2 && task_delete_calls == 1);
    assert(audio_pipeline_deinit() == ESP_OK);
    assert_resources(false);
    assert_cleanup_order();
}
static void test_deinit_stop_failure_preserves_resources(void) {
    reset_case();
    audio_pipeline_config_t cfg = config();
    assert(audio_pipeline_init(&cfg) == ESP_OK);
    assert(audio_pipeline_start() == ESP_OK);
    fail_capture_stop = true;
    assert(audio_pipeline_deinit() == ESP_ERR_TIMEOUT);
    assert_resources(true);
    assert(free_calls == 0 && task_delete_calls == 0 && capture_running);
    fail_capture_stop = false;
    assert(audio_pipeline_deinit() == ESP_OK);
    assert_resources(false);
}
static void test_sd_timeout_nonfatal_and_invalid_init(void) {
    reset_case();
    audio_pipeline_config_t cfg = config();
    assert(audio_pipeline_init(NULL) == ESP_ERR_INVALID_ARG);
    audio_pipeline_config_t invalid = cfg;
    invalid.event_open_cb = NULL;
    assert(audio_pipeline_init(&invalid) == ESP_ERR_INVALID_ARG);
    assert(allocation_calls == 0 && task_create_calls == 0);
    fail_sd_mount = true;
    assert(audio_pipeline_init(&cfg) == ESP_OK);
    assert(!recorder_running && dsp_live);
    assert(audio_pipeline_start() == ESP_OK);
    assert(audio_pipeline_deinit() == ESP_OK);
    assert_resources(false);
}
static void test_export_suspend_retains_storage_and_retries_resume(void) {
    reset_case();
    audio_pipeline_config_t cfg = config();
    assert(audio_pipeline_init(&cfg) == ESP_OK);
    assert(audio_pipeline_start() == ESP_OK);
    s_pipeline.stats.captured_samples = 12345;
    // An active utterance cannot be truncated by an accidental suspend call.
    s_pipeline.event_id = 71;
    assert(audio_pipeline_suspend_for_export() == ESP_ERR_INVALID_STATE);
    assert(capture_running && recorder_running);
    s_pipeline.event_id = 0;
    assert(audio_pipeline_suspend_for_export() == ESP_OK);
    assert(!capture_live && !dsp_live && !audio_pipeline_is_running());
    assert(recorder_live && recorder_running && ring_live && processing_task.allocated);
    assert(live_allocations == 2 && free_calls == 0 && task_delete_calls == 0);
    assert(audio_pipeline_suspend_for_export() == ESP_OK);
    assert(audio_pipeline_start() == ESP_ERR_INVALID_STATE);
    fail_dsp_init = true;
    assert(audio_pipeline_resume_after_export() == ESP_ERR_NO_MEM);
    assert(!capture_live && !dsp_live && recorder_running);
    assert(s_pipeline.export_pause_requested);
    fail_dsp_init = false;
    assert(audio_pipeline_resume_after_export() == ESP_OK);
    assert_resources(true);
    assert(capture_running && recorder_running && !s_pipeline.export_pause_requested);
    assert(s_pipeline.stats.captured_samples == 12345);
    assert(audio_pipeline_resume_after_export() == ESP_OK);
    assert(audio_pipeline_deinit() == ESP_OK);
    assert_resources(false);
}
static void test_export_partial_suspend_can_restore(void) {
    reset_case();
    audio_pipeline_config_t cfg = config();
    assert(audio_pipeline_init(&cfg) == ESP_OK && audio_pipeline_start() == ESP_OK);
    fail_capture_stop = true;
    assert(audio_pipeline_suspend_for_export() == ESP_ERR_TIMEOUT);
    assert(capture_running && dsp_live && recorder_running && s_pipeline.export_pause_requested);
    assert(audio_pipeline_resume_after_export() == ESP_ERR_TIMEOUT);
    fail_capture_stop = false;
    assert(audio_pipeline_resume_after_export() == ESP_OK);
    assert(capture_running && dsp_live && recorder_running);
    assert(audio_pipeline_deinit() == ESP_OK);
    assert_resources(false);
}
static void test_five_minute_cap_and_vad_rearm(void) {
    reset_case();
    audio_pipeline_config_t cfg = config();
    assert(audio_pipeline_init(&cfg) == ESP_OK);
    const uint64_t start = 5000000000ULL; /* Exercise 64-bit capture indices. */
    const int64_t timestamp = 7000000000LL;
    const uint64_t end = start + (uint64_t)AUDIO_SAMPLE_RATE_HZ * 300;
    assert(AUDIO_EVENT_MAX_DURATION_MS == 300000);
    preroll_lag_samples = 160; /* Force the last 20 ms packet to need trimming. */
    vad_service_on_afe_result(AUDIO_VAD_SPEECH, timestamp, start, 0, NULL);
    assert(event_opens == 1 && s_pipeline.event_id == 1);
    assert(s_pipeline.event_end_sample == end);
    int16_t samples[AUDIO_FRAME_SAMPLES] = {0};
    audio_frame_t raw = {.samples = samples, .sample_count = AUDIO_FRAME_SAMPLES,
        .sample_rate = AUDIO_SAMPLE_RATE_HZ, .first_sample_index = start + preroll_lag_samples};
    while (raw.first_sample_index < end) {
        assert(event_ends == 0 && recorder_ends == 0);
        record_event_frame(&raw);
        /* Real VAD service must not manufacture another start on SPEECH. */
        vad_service_on_afe_result(AUDIO_VAD_SPEECH, timestamp, raw.first_sample_index, 0, NULL);
        raw.first_sample_index += AUDIO_FRAME_SAMPLES;
    }
    assert(event_ends == 1 && recorder_ends == 1 && !s_pipeline.event_id);
    assert(last_end_sample == end && last_end_timestamp == timestamp + 300000000LL);
    assert(saved_samples == (uint64_t)AUDIO_SAMPLE_RATE_HZ * 301); /* 1 s pre-roll. */
    assert(!last_producer_failed && failed_terminals == 0 && s_pipeline.wait_for_silence);
    record_event_frame(&raw);
    vad_event(AUDIO_VAD_SPEECH, timestamp, end, NULL); /* Guard even a redundant callback. */
    assert(event_opens == 1 && recorder_ends == 1);
    vad_service_on_afe_result(AUDIO_VAD_SILENCE, timestamp + 301000000LL, end, 0, NULL);
    assert(!s_pipeline.wait_for_silence && event_ends == 1);
    vad_service_on_afe_result(AUDIO_VAD_SPEECH, timestamp + 302000000LL, end + 16000, 0, NULL);
    assert(event_opens == 2 && s_pipeline.event_id == 2);
    vad_service_on_afe_result(AUDIO_VAD_SILENCE, timestamp + 303000000LL, end + 32000, 0, NULL);
    assert(event_ends == 2 && recorder_ends == 2);
    assert(audio_pipeline_deinit() == ESP_OK);
    assert_resources(false);
}
static void test_normal_end_before_limit(void) {
    reset_case();
    audio_pipeline_config_t cfg = config();
    assert(audio_pipeline_init(&cfg) == ESP_OK);
    vad_service_on_afe_result(AUDIO_VAD_SPEECH, 1000000, 16000, 0, NULL);
    vad_service_on_afe_result(AUDIO_VAD_SILENCE, 2000000, 32000, 0, NULL);
    assert(event_ends == 1 && recorder_ends == 1 && last_end_sample == 32000);
    assert(last_end_timestamp == 2000000 && !s_pipeline.wait_for_silence);
    assert(!last_producer_failed && failed_terminals == 0);
    assert(audio_pipeline_deinit() == ESP_OK);
    assert_resources(false);
}
static void test_limit_failure_paths(void) {
    for (unsigned failure = 0; failure < 4; ++failure) {
        reset_case();
        audio_pipeline_config_t cfg = config();
        assert(audio_pipeline_init(&cfg) == ESP_OK);
        accept_begin = failure != 0;
        accept_submit = failure != 1;
        accept_end = failure != 2;
        vad_service_on_afe_result(AUDIO_VAD_SPEECH, 0, 0, 0, NULL);
        int16_t samples[AUDIO_FRAME_SAMPLES] = {0};
        audio_frame_t raw = {.samples = samples, .sample_count = AUDIO_FRAME_SAMPLES,
            .sample_rate = AUDIO_SAMPLE_RATE_HZ};
        if (failure == 3) {
            record_event_frame(&raw);
            /* A gap crossing the cap is a failure even with no final PCM submitted. */
            raw.first_sample_index = s_pipeline.event_end_sample + 1;
            record_event_frame(&raw);
        } else {
            /* Keep input continuous to isolate each injected recorder failure. */
            while (s_pipeline.event_id) {
                record_event_frame(&raw);
                raw.first_sample_index += AUDIO_FRAME_SAMPLES;
            }
        }
        assert(event_ends == 1 && !s_pipeline.event_id && s_pipeline.wait_for_silence);
        assert(failed_terminals == (failure == 0 || failure == 2 ? 1U : 0U));
        assert(recorder_ends == (failure == 0 ? 0U : 1U));
        if (failure == 1 || failure == 3) assert(last_producer_failed);
        if (failure == 2) assert(!last_producer_failed);
        assert(audio_pipeline_deinit() == ESP_OK);
        assert_resources(false);
    }
}
int main(void) {
    test_order_and_retained_restart();
    test_capture_init_failure_before_dsp();
    test_processing_task_failure_before_dsp();
    test_dsp_failure_cleanup_and_immediate_retry();
    test_start_failure_then_deinit();
    test_deinit_stop_failure_preserves_resources();
    test_sd_timeout_nonfatal_and_invalid_init();
    test_export_suspend_retains_storage_and_retries_resume();
    test_export_partial_suspend_can_restore();
    test_five_minute_cap_and_vad_rearm();
    test_normal_end_before_limit();
    test_limit_failure_paths();
    puts("pipeline startup/lifecycle/duration tests passed (15 cases)");
    return 0;
}
