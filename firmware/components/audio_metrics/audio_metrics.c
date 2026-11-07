#include "audio_metrics.h"

#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>

typedef struct {
    long double sum;
    long double square_sum;
    int32_t peak;
    uint64_t clipped;
    uint64_t count;
    uint64_t first_sample_index;
    int64_t timestamp_us;
} metric_accumulator_t;

static bool s_enabled;
static metric_accumulator_t s_raw;
static metric_accumulator_t s_processed;

static double to_dbfs(double value)
{
    return value > 0.0 ? 20.0 * log10(value / 32768.0) : -INFINITY;
}

audio_metric_values_t audio_metrics_calculate(const int16_t *samples, size_t count)
{
    audio_metric_values_t result = {.rms_dbfs = -INFINITY, .peak_dbfs = -INFINITY};
    if (!samples || count == 0) {
        return result;
    }
    long double sum = 0.0;
    long double square_sum = 0.0;
    int32_t peak = 0;
    uint64_t clipped = 0;
    for (size_t i = 0; i < count; ++i) {
        const int32_t value = samples[i];
        const int32_t absolute = value == INT16_MIN ? 32768 : abs(value);
        sum += value;
        square_sum += (long double)value * value;
        if (absolute > peak) {
            peak = absolute;
        }
        if (value == INT16_MIN || value == INT16_MAX) {
            clipped++;
        }
    }
    result.mean = (double)(sum / count);
    result.rms = sqrt((double)(square_sum / count));
    result.rms_dbfs = to_dbfs(result.rms);
    result.absolute_peak = peak;
    result.peak_dbfs = to_dbfs(peak);
    result.clipped_samples = clipped;
    result.clipping_ratio = (double)clipped / count;
    return result;
}

static void accumulate(metric_accumulator_t *acc, const audio_frame_t *frame)
{
    if (acc->count == 0) {
        acc->first_sample_index = frame->first_sample_index;
        acc->timestamp_us = frame->timestamp_us;
    }
    for (size_t i = 0; i < frame->sample_count; ++i) {
        const int32_t value = frame->samples[i];
        const int32_t absolute = value == INT16_MIN ? 32768 : abs(value);
        acc->sum += value;
        acc->square_sum += (long double)value * value;
        if (absolute > acc->peak) {
            acc->peak = absolute;
        }
        if (value == INT16_MIN || value == INT16_MAX) {
            acc->clipped++;
        }
    }
    acc->count += frame->sample_count;
}

static void log_accumulator(const char *stream, metric_accumulator_t *acc)
{
    const double mean = (double)(acc->sum / acc->count);
    const double rms = sqrt((double)(acc->square_sum / acc->count));
    printf("METRIC,audio1,%" PRId64 ",%" PRIu64 ",stream,%s,mean,%.3f,rms,%.3f,"
           "rms_dbfs,%.3f,peak,%ld,peak_dbfs,%.3f,clipped,%" PRIu64 ",clipping_ratio,%.9f\n",
           acc->timestamp_us, acc->first_sample_index, stream, mean, rms,
           to_dbfs(rms), (long)acc->peak, to_dbfs(acc->peak), acc->clipped,
           (double)acc->clipped / acc->count);
    *acc = (metric_accumulator_t) {0};
}

void audio_metrics_set_enabled(bool enabled)
{
    s_enabled = enabled;
    if (!enabled) {
        s_raw = (metric_accumulator_t) {0};
        s_processed = (metric_accumulator_t) {0};
    }
}

bool audio_metrics_is_enabled(void)
{
    return s_enabled;
}

void audio_metrics_process(const audio_frame_t *raw, const audio_frame_t *processed)
{
    if (!s_enabled || !raw || !processed) {
        return;
    }
    audio_metrics_process_raw(raw);
    audio_metrics_process_processed(processed);
}

void audio_metrics_process_raw(const audio_frame_t *raw)
{
    if (!s_enabled || !raw) {
        return;
    }
    accumulate(&s_raw, raw);
    if (s_raw.count >= AUDIO_SAMPLE_RATE_HZ) {
        log_accumulator("raw", &s_raw);
    }
}

void audio_metrics_process_processed(const audio_frame_t *processed)
{
    if (!s_enabled || !processed) {
        return;
    }
    accumulate(&s_processed, processed);
    if (s_processed.count >= AUDIO_SAMPLE_RATE_HZ) {
        log_accumulator("processed", &s_processed);
    }
}
