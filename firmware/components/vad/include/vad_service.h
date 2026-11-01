#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "audio_types.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*vad_event_cb_t)(audio_vad_state_t state, int64_t timestamp_us,
                               uint64_t sample_index, void *ctx);

typedef struct {
    vad_event_cb_t event_cb;
    void *event_cb_ctx;
    bool initial_enabled;
} vad_service_config_t;

esp_err_t vad_service_init(const vad_service_config_t *config);
esp_err_t vad_service_set_enabled(bool enabled);
bool vad_service_is_enabled(void);
void vad_service_set_metrics_enabled(bool enabled);
audio_vad_state_t vad_service_state(void);
void vad_service_on_afe_result(audio_vad_state_t state, int64_t timestamp_us,
                               uint64_t sample_index, size_t afe_cache_bytes,
                               void *unused);

#ifdef __cplusplus
}
#endif
