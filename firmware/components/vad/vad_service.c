#include "vad_service.h"

#include <inttypes.h>
#include <stdio.h>
#include "audio_dsp.h"

static vad_service_config_t s_config;
static bool s_enabled;
static bool s_metrics_enabled;
static audio_vad_state_t s_state = AUDIO_VAD_SILENCE;

esp_err_t vad_service_init(const vad_service_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    s_config = *config;
    s_enabled = config->initial_enabled;
    s_state = AUDIO_VAD_SILENCE;
    return ESP_OK;
}

esp_err_t vad_service_set_enabled(bool enabled)
{
    esp_err_t err = audio_dsp_set_vad_enabled(enabled);
    if (err == ESP_OK) {
        s_enabled = enabled;
        s_state = AUDIO_VAD_SILENCE;
        printf("CONFIG,vad,enabled,%d\n", enabled);
    }
    return err;
}

bool vad_service_is_enabled(void)
{
    return s_enabled;
}

void vad_service_set_metrics_enabled(bool enabled)
{
    s_metrics_enabled = enabled;
}

audio_vad_state_t vad_service_state(void)
{
    return s_state;
}

void vad_service_on_afe_result(audio_vad_state_t state, int64_t timestamp_us,
                               uint64_t sample_index, size_t afe_cache_bytes,
                               void *unused)
{
    (void)unused;
    if (!s_enabled) {
        return;
    }
    if (s_metrics_enabled) {
        printf("METRIC,vad,%" PRId64 ",%" PRIu64 ",state,%d,afe_cache_bytes,%u\n",
               timestamp_us, sample_index, state, (unsigned)afe_cache_bytes);
    }
    if (state != s_state) {
        printf("EVENT,%s,%" PRId64 ",%" PRIu64 "\n",
               state == AUDIO_VAD_SPEECH ? "vad_start" : "vad_end",
               timestamp_us, sample_index);
        s_state = state;
        if (s_config.event_cb) {
            s_config.event_cb(state, timestamp_us, sample_index,
                              s_config.event_cb_ctx);
        }
    }
}
