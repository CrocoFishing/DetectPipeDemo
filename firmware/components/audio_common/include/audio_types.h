#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_SAMPLE_RATE_HZ       16000U
#define AUDIO_CHANNEL_COUNT        1U
#define AUDIO_BYTES_PER_SAMPLE     2U
#define AUDIO_BYTES_PER_SECOND     32000U
#define AUDIO_FRAME_MS             20U
#define AUDIO_FRAME_SAMPLES        ((AUDIO_SAMPLE_RATE_HZ * AUDIO_FRAME_MS) / 1000U)
#define AUDIO_FRAME_BYTES          (AUDIO_FRAME_SAMPLES * AUDIO_BYTES_PER_SAMPLE)

typedef struct {
    int16_t *samples;
    size_t sample_count;
    uint32_t sample_rate;
    uint64_t first_sample_index;
    int64_t timestamp_us;
} audio_frame_t;

typedef enum {
    AUDIO_DSP_RAW = 0,
    AUDIO_DSP_FIXED_GAIN,
    AUDIO_DSP_AGC,
    AUDIO_DSP_NS,
    AUDIO_DSP_FIXED_GAIN_NS,
    AUDIO_DSP_AGC_NS,
    AUDIO_DSP_MODE_COUNT,
} audio_dsp_mode_t;

typedef struct {
    audio_dsp_mode_t mode;
    bool dc_block_enabled;
    int fixed_gain_db;
    int agc_target_level_dbfs;
    int agc_compression_gain_db;
} audio_dsp_config_t;

typedef enum {
    AUDIO_VAD_SILENCE = 0,
    AUDIO_VAD_SPEECH = 1,
} audio_vad_state_t;

typedef struct {
    uint64_t captured_samples;
    uint64_t processed_samples;
    uint64_t dropped_samples;
    uint32_t queue_overruns;
    uint32_t audio_queue_high_water;
    uint32_t sd_short_writes;
    uint32_t usb_underruns;
    uint32_t ring_overruns;
    uint64_t dc_block_clipped_samples;
    uint64_t fixed_gain_clipped_samples;
    uint32_t capture_max_processing_us;
    uint32_t dsp_max_processing_us;
    uint32_t sd_max_write_us;
    uint32_t sd_queue_high_water;
    uint32_t sd_metadata_drops;
} audio_runtime_stats_t;

const char *audio_dsp_mode_name(audio_dsp_mode_t mode);
/* Short shared critical section protects cross-core counters and snapshots. */
void audio_stats_lock(void);
void audio_stats_unlock(void);
audio_runtime_stats_t audio_stats_snapshot(const audio_runtime_stats_t *stats);

#ifdef __cplusplus
}
#endif
