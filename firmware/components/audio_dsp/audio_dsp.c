#include "audio_dsp.h"

#include <inttypes.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "esp_sr_afe.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "audio_dsp";

/* exp(-2 * pi * 80 / 16000), matching the host-side analyzer. */
#define DC_BLOCK_ALPHA 0.9690724f

typedef struct {
    audio_dsp_init_config_t init;
    audio_dsp_config_t config;
    bool vad_enabled;
    SemaphoreHandle_t lock;
    int16_t dc_block_buffer[AUDIO_FRAME_SAMPLES];
    int16_t direct_buffer[AUDIO_FRAME_SAMPLES];
    int16_t afe_input_buffer[AUDIO_FRAME_SAMPLES];
    float dc_block_previous_input;
    float dc_block_previous_output;
    uint64_t dc_block_next_sample_index;
    bool dc_block_state_valid;
    int64_t stream_epoch_us;
    bool epoch_valid;
} dsp_state_t;

static dsp_state_t s_dsp;

static bool mode_uses_afe(audio_dsp_mode_t mode)
{
    return mode == AUDIO_DSP_AGC || mode == AUDIO_DSP_NS ||
           mode == AUDIO_DSP_FIXED_GAIN_NS || mode == AUDIO_DSP_AGC_NS;
}

static bool mode_uses_fixed_gain(audio_dsp_mode_t mode)
{
    return mode == AUDIO_DSP_FIXED_GAIN || mode == AUDIO_DSP_FIXED_GAIN_NS;
}

static void reset_dc_block_locked(void)
{
    s_dsp.dc_block_previous_input = 0.0f;
    s_dsp.dc_block_previous_output = 0.0f;
    s_dsp.dc_block_next_sample_index = 0;
    s_dsp.dc_block_state_valid = false;
}

static int16_t dc_block_to_pcm16(float value, uint64_t *clipped)
{
    if (value > INT16_MAX) {
        (*clipped)++;
        return INT16_MAX;
    }
    if (value < INT16_MIN) {
        (*clipped)++;
        return INT16_MIN;
    }
    return (int16_t)(value >= 0.0f ? value + 0.5f : value - 0.5f);
}

static size_t dc_block_apply_locked(const audio_frame_t *input,
                                    int16_t *output,
                                    uint64_t *clipped)
{
    size_t start = 0;
    if (!s_dsp.dc_block_state_valid ||
        input->first_sample_index != s_dsp.dc_block_next_sample_index) {
        s_dsp.dc_block_previous_input = input->samples[0];
        s_dsp.dc_block_previous_output = 0.0f;
        s_dsp.dc_block_state_valid = true;
        output[0] = 0;
        start = 1;
    }

    for (size_t i = start; i < input->sample_count; ++i) {
        const float current_input = input->samples[i];
        const float current_output = DC_BLOCK_ALPHA *
            (s_dsp.dc_block_previous_output + current_input -
             s_dsp.dc_block_previous_input);
        output[i] = dc_block_to_pcm16(current_output, clipped);
        s_dsp.dc_block_previous_input = current_input;
        s_dsp.dc_block_previous_output = current_output;
    }
    s_dsp.dc_block_next_sample_index =
        input->first_sample_index + input->sample_count;
    return input->sample_count;
}

static esp_err_t configure_afe_locked(void)
{
    const bool needed = mode_uses_afe(s_dsp.config.mode) || s_dsp.vad_enabled;
    esp_sr_afe_deinit();
    if (!needed) {
        return ESP_OK;
    }
    const esp_sr_afe_config_t config = {
        .enable_ns = s_dsp.config.mode == AUDIO_DSP_NS ||
                     s_dsp.config.mode == AUDIO_DSP_FIXED_GAIN_NS ||
                     s_dsp.config.mode == AUDIO_DSP_AGC_NS,
        .enable_agc = s_dsp.config.mode == AUDIO_DSP_AGC ||
                      s_dsp.config.mode == AUDIO_DSP_AGC_NS,
        .enable_vad = s_dsp.vad_enabled,
        .agc_target_level_dbfs = s_dsp.config.agc_target_level_dbfs,
        .agc_compression_gain_db = s_dsp.config.agc_compression_gain_db,
    };
    return esp_sr_afe_init(&config);
}

static void afe_output(const int16_t *samples, size_t count,
                       audio_vad_state_t vad_state, size_t vad_cache_bytes,
                       uint64_t first_sample_index, void *ctx)
{
    (void)ctx;
    const int64_t timestamp_us = s_dsp.stream_epoch_us +
        (int64_t)((first_sample_index * 1000000ULL) / AUDIO_SAMPLE_RATE_HZ);
    if (s_dsp.vad_enabled && s_dsp.init.vad_cb) {
        s_dsp.init.vad_cb(vad_state, timestamp_us, first_sample_index,
                          vad_cache_bytes, s_dsp.init.callback_ctx);
    }
    if (mode_uses_afe(s_dsp.config.mode) && s_dsp.init.output_cb) {
        audio_frame_t frame = {
            .samples = (int16_t *)samples,
            .sample_count = count,
            .sample_rate = AUDIO_SAMPLE_RATE_HZ,
            .first_sample_index = first_sample_index,
            .timestamp_us = timestamp_us,
        };
        s_dsp.init.output_cb(&frame, s_dsp.init.callback_ctx);
    }
}

esp_err_t audio_dsp_init(const audio_dsp_init_config_t *config)
{
    ESP_RETURN_ON_FALSE(config && config->output_cb, ESP_ERR_INVALID_ARG, TAG,
                        "output callback is required");
    ESP_RETURN_ON_FALSE(!s_dsp.lock, ESP_ERR_INVALID_STATE, TAG, "already initialized");
    ESP_RETURN_ON_FALSE(config->initial_config.mode < AUDIO_DSP_MODE_COUNT,
                        ESP_ERR_INVALID_ARG, TAG, "invalid mode");
    ESP_RETURN_ON_FALSE(audio_dsp_gain_is_supported(config->initial_config.fixed_gain_db),
                        ESP_ERR_INVALID_ARG, TAG, "unsupported fixed gain");
    s_dsp.init = *config;
    s_dsp.config = config->initial_config;
    s_dsp.vad_enabled = config->vad_enabled;
    reset_dc_block_locked();
    s_dsp.lock = xSemaphoreCreateMutex();
    if (!s_dsp.lock) {
        memset(&s_dsp, 0, sizeof(s_dsp));
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = configure_afe_locked();
    if (err != ESP_OK) {
        vSemaphoreDelete(s_dsp.lock);
        memset(&s_dsp, 0, sizeof(s_dsp));
    }
    return err;
}

void audio_dsp_deinit(void)
{
    if (!s_dsp.lock) {
        return;
    }
    xSemaphoreTake(s_dsp.lock, portMAX_DELAY);
    esp_sr_afe_deinit();
    xSemaphoreGive(s_dsp.lock);
    vSemaphoreDelete(s_dsp.lock);
    memset(&s_dsp, 0, sizeof(s_dsp));
}

esp_err_t audio_dsp_set_config(const audio_dsp_config_t *config)
{
    if (!config || config->mode >= AUDIO_DSP_MODE_COUNT ||
        !audio_dsp_gain_is_supported(config->fixed_gain_db) ||
        config->agc_target_level_dbfs < 0 ||
        config->agc_compression_gain_db < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_dsp.lock, portMAX_DELAY);
    const audio_dsp_config_t previous = s_dsp.config;
    const bool dc_block_changed =
        config->dc_block_enabled != previous.dc_block_enabled;
    s_dsp.config = *config;
    esp_err_t err = configure_afe_locked();
    if (err != ESP_OK) {
        s_dsp.config = previous;
        (void)configure_afe_locked();
    } else {
        if (dc_block_changed) {
            reset_dc_block_locked();
        }
        printf("CONFIG,dsp,mode,%s,dc_block_enabled,%d,dc_block_cutoff_hz,%u,"
               "gain_db,%d,"
               "agc_target_dbfs,-%d,agc_compression_db,%d\n",
               audio_dsp_mode_name(config->mode), config->dc_block_enabled,
               AUDIO_DC_BLOCK_CUTOFF_HZ, config->fixed_gain_db,
               config->agc_target_level_dbfs, config->agc_compression_gain_db);
    }
    xSemaphoreGive(s_dsp.lock);
    return err;
}

audio_dsp_config_t audio_dsp_get_config(void)
{
    xSemaphoreTake(s_dsp.lock, portMAX_DELAY);
    audio_dsp_config_t config = s_dsp.config;
    xSemaphoreGive(s_dsp.lock);
    return config;
}

esp_err_t audio_dsp_set_vad_enabled(bool enabled)
{
    xSemaphoreTake(s_dsp.lock, portMAX_DELAY);
    bool previous = s_dsp.vad_enabled;
    s_dsp.vad_enabled = enabled;
    esp_err_t err = configure_afe_locked();
    if (err != ESP_OK) {
        s_dsp.vad_enabled = previous;
        (void)configure_afe_locked();
    }
    xSemaphoreGive(s_dsp.lock);
    return err;
}

bool audio_dsp_get_vad_enabled(void)
{
    xSemaphoreTake(s_dsp.lock, portMAX_DELAY);
    bool enabled = s_dsp.vad_enabled;
    xSemaphoreGive(s_dsp.lock);
    return enabled;
}

esp_err_t audio_dsp_process(const audio_frame_t *input)
{
    if (!input || !input->samples || input->sample_count == 0 ||
        input->sample_count > AUDIO_FRAME_SAMPLES) {
        return ESP_ERR_INVALID_ARG;
    }
    const int64_t started_us = esp_timer_get_time();
    xSemaphoreTake(s_dsp.lock, portMAX_DELAY);
    if (!s_dsp.epoch_valid) {
        s_dsp.stream_epoch_us = input->timestamp_us -
            (int64_t)((input->first_sample_index * 1000000ULL) / AUDIO_SAMPLE_RATE_HZ);
        s_dsp.epoch_valid = true;
    }

    int16_t *conditioned = input->samples;
    if (s_dsp.config.dc_block_enabled) {
        uint64_t clipped = 0;
        dc_block_apply_locked(input, s_dsp.dc_block_buffer, &clipped);
        conditioned = s_dsp.dc_block_buffer;
        if (s_dsp.init.stats) {
            audio_stats_lock();
            s_dsp.init.stats->dc_block_clipped_samples += clipped;
            audio_stats_unlock();
        }
    } else {
        reset_dc_block_locked();
    }

    const bool fixed_gain = mode_uses_fixed_gain(s_dsp.config.mode);
    int16_t *direct = conditioned;
    if (fixed_gain) {
        uint64_t clipped = 0;
        audio_fixed_gain_apply(conditioned, s_dsp.direct_buffer,
                               input->sample_count, s_dsp.config.fixed_gain_db, &clipped);
        direct = s_dsp.direct_buffer;
        if (s_dsp.init.stats) {
            audio_stats_lock();
            s_dsp.init.stats->fixed_gain_clipped_samples += clipped;
            audio_stats_unlock();
        }
    }

    if (!mode_uses_afe(s_dsp.config.mode)) {
        audio_frame_t output = *input;
        output.samples = direct;
        s_dsp.init.output_cb(&output, s_dsp.init.callback_ctx);
    }

    esp_err_t err = ESP_OK;
    if (esp_sr_afe_is_initialized()) {
        const int16_t *afe_input = conditioned;
        if (s_dsp.config.mode == AUDIO_DSP_FIXED_GAIN_NS) {
            memcpy(s_dsp.afe_input_buffer, direct,
                   input->sample_count * sizeof(int16_t));
            afe_input = s_dsp.afe_input_buffer;
        }
        err = esp_sr_afe_feed(afe_input, input->sample_count,
                              input->first_sample_index, afe_output, NULL);
    }
    xSemaphoreGive(s_dsp.lock);

    const uint32_t elapsed_us = (uint32_t)(esp_timer_get_time() - started_us);
    if (s_dsp.init.stats) {
        audio_stats_lock();
        if (elapsed_us > s_dsp.init.stats->dsp_max_processing_us)
            s_dsp.init.stats->dsp_max_processing_us = elapsed_us;
        audio_stats_unlock();
    }
    return err;
}
