#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "audio_dsp.h"

typedef struct {
    bool enable_ns;
    bool enable_agc;
    bool enable_vad;
    int agc_target_level_dbfs;
    int agc_compression_gain_db;
} esp_sr_afe_config_t;

typedef void (*esp_sr_output_cb_t)(const int16_t *samples, size_t count,
                                   audio_vad_state_t vad_state,
                                   size_t vad_cache_bytes,
                                   uint64_t first_sample_index, void *ctx);

esp_err_t esp_sr_afe_init(const esp_sr_afe_config_t *config);
void esp_sr_afe_deinit(void);
bool esp_sr_afe_is_initialized(void);
size_t esp_sr_afe_feed_chunk_samples(void);
size_t esp_sr_afe_fetch_chunk_samples(void);
esp_err_t esp_sr_afe_feed(const int16_t *samples, size_t count,
                          uint64_t first_sample_index,
                          esp_sr_output_cb_t callback, void *ctx);
