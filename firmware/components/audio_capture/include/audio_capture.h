#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "audio_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef bool (*audio_capture_frame_cb_t)(const audio_frame_t *frame, void *ctx);

typedef struct {
    audio_capture_frame_cb_t frame_cb;
    void *frame_cb_ctx;
    audio_runtime_stats_t *stats;
    uint32_t read_timeout_ms;
} audio_capture_config_t;

esp_err_t audio_capture_init(const audio_capture_config_t *config);
esp_err_t audio_capture_start(void);
esp_err_t audio_capture_stop(void);
bool audio_capture_is_running(void);
void audio_capture_deinit(void);

#ifdef __cplusplus
}
#endif
