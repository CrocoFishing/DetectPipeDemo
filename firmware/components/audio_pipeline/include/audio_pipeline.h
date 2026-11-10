#pragma once
#include <stdbool.h>
#include "audio_types.h"
#include "sd_recorder.h"
#include "esp_err.h"

/* Measured from the VAD trigger sample; saved pre-roll is additional. */
#define AUDIO_EVENT_MAX_DURATION_MS 300000U

#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t (*audio_event_open_cb_t)(int64_t timestamp_us, uint64_t sample_index, void *ctx);
typedef void (*audio_event_end_cb_t)(uint64_t event_id, int64_t timestamp_us, uint64_t sample_index, void *ctx);
typedef struct {
    audio_dsp_config_t dsp;
    uint32_t preroll_ms;
    bool metrics_enabled;
    bool vad_enabled;
    audio_event_open_cb_t event_open_cb;
    audio_event_end_cb_t event_end_cb;
    sd_event_terminal_cb_t audio_terminal_cb;
    void *callback_ctx;
} audio_pipeline_config_t;
audio_pipeline_config_t audio_pipeline_default_config(void);
esp_err_t audio_pipeline_init(const audio_pipeline_config_t *config);
esp_err_t audio_pipeline_start(void);
esp_err_t audio_pipeline_stop(void);
esp_err_t audio_pipeline_deinit(void);
bool audio_pipeline_is_running(void);
// Export coordinator must gate Event creation and wait for all existing owners
// before suspending. Storage stays mounted/running; resume is safe to retry.
esp_err_t audio_pipeline_suspend_for_export(void);
esp_err_t audio_pipeline_resume_after_export(void);
esp_err_t audio_pipeline_set_dsp(const audio_dsp_config_t *config);
esp_err_t audio_pipeline_set_preroll(uint32_t milliseconds);
esp_err_t audio_pipeline_set_metrics(bool enabled);
esp_err_t audio_pipeline_set_vad(bool enabled);
audio_runtime_stats_t audio_pipeline_get_stats(void);
void audio_pipeline_print_status(void);
#ifdef __cplusplus
}
#endif
