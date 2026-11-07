#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "audio_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t trigger_sample;
    uint64_t saved_first_sample;
    uint32_t requested_preroll_ms;
    uint32_t actual_preroll_ms;
    uint32_t wrap_count;
    uint32_t overrun_count;
} audio_preroll_metadata_t;

esp_err_t audio_ring_buffer_init(uint32_t maximum_ms, audio_runtime_stats_t *stats);
void audio_ring_buffer_deinit(void);
// Clear history after an intentional capture pause without reallocating buffers.
void audio_ring_buffer_reset(void);
esp_err_t audio_ring_buffer_set_preroll(uint32_t milliseconds);
uint32_t audio_ring_buffer_get_preroll(void);
void audio_ring_buffer_push(const audio_frame_t *frame);
size_t audio_ring_buffer_copy_preroll(uint64_t trigger_sample, int16_t *destination,
                                      size_t destination_samples,
                                      audio_preroll_metadata_t *metadata);
size_t audio_ring_buffer_copy_event_start(uint64_t trigger_sample, int16_t *destination,
                                          size_t destination_samples,
                                          audio_preroll_metadata_t *metadata);

#ifdef __cplusplus
}
#endif
