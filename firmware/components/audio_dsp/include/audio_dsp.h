#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "audio_types.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_DC_BLOCK_CUTOFF_HZ 80U

typedef void (*audio_dsp_output_cb_t)(const audio_frame_t *frame, void *ctx);
typedef void (*audio_dsp_vad_cb_t)(audio_vad_state_t state, int64_t timestamp_us,
                                   uint64_t sample_index, size_t afe_cache_bytes,
                                   void *ctx);

typedef struct {
    audio_dsp_config_t initial_config;
    bool vad_enabled;
    audio_dsp_output_cb_t output_cb;
    audio_dsp_vad_cb_t vad_cb;
    void *callback_ctx;
    audio_runtime_stats_t *stats;
} audio_dsp_init_config_t;

bool audio_dsp_gain_is_supported(int gain_db);
size_t audio_fixed_gain_apply(const int16_t *input, int16_t *output, size_t count,
                              int gain_db, uint64_t *clipped_samples);
esp_err_t audio_dsp_init(const audio_dsp_init_config_t *config);
void audio_dsp_deinit(void);
esp_err_t audio_dsp_set_config(const audio_dsp_config_t *config);
audio_dsp_config_t audio_dsp_get_config(void);
esp_err_t audio_dsp_set_vad_enabled(bool enabled);
bool audio_dsp_get_vad_enabled(void);
esp_err_t audio_dsp_process(const audio_frame_t *input);

#ifdef __cplusplus
}
#endif
