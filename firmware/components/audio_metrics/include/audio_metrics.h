#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "audio_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    double mean;
    double rms;
    double rms_dbfs;
    int32_t absolute_peak;
    double peak_dbfs;
    uint64_t clipped_samples;
    double clipping_ratio;
} audio_metric_values_t;

void audio_metrics_set_enabled(bool enabled);
bool audio_metrics_is_enabled(void);
audio_metric_values_t audio_metrics_calculate(const int16_t *samples, size_t count);
void audio_metrics_process_raw(const audio_frame_t *raw);
void audio_metrics_process_processed(const audio_frame_t *processed);
void audio_metrics_process(const audio_frame_t *raw, const audio_frame_t *processed);

#ifdef __cplusplus
}
#endif
