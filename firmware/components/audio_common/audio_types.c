#include "audio_types.h"
#include "freertos/FreeRTOS.h"

static portMUX_TYPE s_stats_lock = portMUX_INITIALIZER_UNLOCKED;
void audio_stats_lock(void) { portENTER_CRITICAL(&s_stats_lock); }
void audio_stats_unlock(void) { portEXIT_CRITICAL(&s_stats_lock); }
audio_runtime_stats_t audio_stats_snapshot(const audio_runtime_stats_t *stats)
{
    audio_stats_lock();
    audio_runtime_stats_t snapshot = *stats;
    audio_stats_unlock();
    return snapshot;
}

const char *audio_dsp_mode_name(audio_dsp_mode_t mode)
{
    static const char *const names[AUDIO_DSP_MODE_COUNT] = {
        "RAW", "FIXED_GAIN", "AGC", "NS", "FIXED_GAIN_NS", "AGC_NS"
    };
    return (mode >= 0 && mode < AUDIO_DSP_MODE_COUNT) ? names[mode] : "UNKNOWN";
}
