#include "audio_ring_buffer.h"

#include <stdlib.h>
#include <string.h>
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

typedef struct {
    int16_t *samples;
    size_t capacity;
    size_t count;
    size_t write_pos;
    uint64_t first_sample_index;
    uint64_t next_sample_index;
    uint32_t requested_ms;
    uint32_t wrap_count;
    uint32_t overrun_count;
    SemaphoreHandle_t lock;
    audio_runtime_stats_t *stats;
} ring_state_t;

static ring_state_t s_ring;

esp_err_t audio_ring_buffer_init(uint32_t maximum_ms, audio_runtime_stats_t *stats)
{
    if (maximum_ms == 0 || s_ring.samples) {
        return ESP_ERR_INVALID_ARG;
    }
    s_ring.capacity = (size_t)(((uint64_t)AUDIO_SAMPLE_RATE_HZ * maximum_ms) / 1000U);
    s_ring.samples = heap_caps_calloc(s_ring.capacity, sizeof(int16_t),
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_ring.samples) {
        s_ring.samples = calloc(s_ring.capacity, sizeof(int16_t));
    }
    if (!s_ring.samples) {
        memset(&s_ring, 0, sizeof(s_ring));
        return ESP_ERR_NO_MEM;
    }
    s_ring.lock = xSemaphoreCreateMutex();
    if (!s_ring.lock) {
        free(s_ring.samples);
        memset(&s_ring, 0, sizeof(s_ring));
        return ESP_ERR_NO_MEM;
    }
    s_ring.requested_ms = maximum_ms;
    s_ring.stats = stats;
    return ESP_OK;
}

void audio_ring_buffer_deinit(void)
{
    if (s_ring.lock) {
        vSemaphoreDelete(s_ring.lock);
    }
    free(s_ring.samples);
    memset(&s_ring, 0, sizeof(s_ring));
}

void audio_ring_buffer_reset(void)
{
    if (!s_ring.lock) return;
    xSemaphoreTake(s_ring.lock, portMAX_DELAY);
    s_ring.count = 0;
    s_ring.write_pos = 0;
    s_ring.first_sample_index = 0;
    s_ring.next_sample_index = 0;
    xSemaphoreGive(s_ring.lock);
}

esp_err_t audio_ring_buffer_set_preroll(uint32_t milliseconds)
{
    if (!s_ring.samples || milliseconds > (s_ring.capacity * 1000U) / AUDIO_SAMPLE_RATE_HZ) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_ring.lock, portMAX_DELAY);
    s_ring.requested_ms = milliseconds;
    xSemaphoreGive(s_ring.lock);
    return ESP_OK;
}

uint32_t audio_ring_buffer_get_preroll(void)
{
    if (!s_ring.lock) {
        return 0;
    }
    xSemaphoreTake(s_ring.lock, portMAX_DELAY);
    uint32_t value = s_ring.requested_ms;
    xSemaphoreGive(s_ring.lock);
    return value;
}

void audio_ring_buffer_push(const audio_frame_t *frame)
{
    if (!s_ring.samples || !frame || !frame->samples || frame->sample_count == 0) {
        return;
    }
    xSemaphoreTake(s_ring.lock, portMAX_DELAY);
    if (s_ring.count && frame->first_sample_index != s_ring.next_sample_index) {
        s_ring.count = 0;
        s_ring.write_pos = 0;
        s_ring.overrun_count++;
        if (s_ring.stats) {
            audio_stats_lock();
            s_ring.stats->ring_overruns++;
            audio_stats_unlock();
        }
    }
    for (size_t i = 0; i < frame->sample_count; ++i) {
        if (s_ring.count == s_ring.capacity) {
            s_ring.first_sample_index++;
        } else {
            if (s_ring.count == 0) {
                s_ring.first_sample_index = frame->first_sample_index + i;
            }
            s_ring.count++;
        }
        s_ring.samples[s_ring.write_pos++] = frame->samples[i];
        if (s_ring.write_pos == s_ring.capacity) {
            s_ring.write_pos = 0;
            s_ring.wrap_count++;
        }
    }
    s_ring.next_sample_index = frame->first_sample_index + frame->sample_count;
    xSemaphoreGive(s_ring.lock);
}

size_t audio_ring_buffer_copy_preroll(uint64_t trigger_sample, int16_t *destination,
                                      size_t destination_samples,
                                      audio_preroll_metadata_t *metadata)
{
    if (!s_ring.samples || !destination || !metadata) {
        return 0;
    }
    xSemaphoreTake(s_ring.lock, portMAX_DELAY);
    const uint64_t requested = ((uint64_t)s_ring.requested_ms * AUDIO_SAMPLE_RATE_HZ) / 1000U;
    uint64_t wanted_first = trigger_sample > requested ? trigger_sample - requested : 0;
    uint64_t available_first = s_ring.first_sample_index;
    uint64_t available_last = s_ring.next_sample_index;
    if (trigger_sample < available_last) {
        available_last = trigger_sample;
    }
    if (wanted_first < available_first) {
        wanted_first = available_first;
    }
    size_t count = available_last > wanted_first ? (size_t)(available_last - wanted_first) : 0;
    if (count > destination_samples) {
        wanted_first += count - destination_samples;
        count = destination_samples;
    }
    for (size_t i = 0; i < count; ++i) {
        const uint64_t absolute = wanted_first + i;
        const size_t oldest_pos = (s_ring.write_pos + s_ring.capacity - s_ring.count) % s_ring.capacity;
        const size_t offset = (size_t)(absolute - s_ring.first_sample_index);
        destination[i] = s_ring.samples[(oldest_pos + offset) % s_ring.capacity];
    }
    *metadata = (audio_preroll_metadata_t) {
        .trigger_sample = trigger_sample,
        .saved_first_sample = wanted_first,
        .requested_preroll_ms = s_ring.requested_ms,
        .actual_preroll_ms = (uint32_t)((count * 1000U) / AUDIO_SAMPLE_RATE_HZ),
        .wrap_count = s_ring.wrap_count,
        .overrun_count = s_ring.overrun_count,
    };
    xSemaphoreGive(s_ring.lock);
    return count;
}

size_t audio_ring_buffer_copy_event_start(uint64_t trigger_sample, int16_t *destination,
                                          size_t destination_samples,
                                          audio_preroll_metadata_t *metadata)
{
    if (!s_ring.samples || !destination || !metadata) {
        return 0;
    }
    xSemaphoreTake(s_ring.lock, portMAX_DELAY);
    const uint64_t requested = ((uint64_t)s_ring.requested_ms * AUDIO_SAMPLE_RATE_HZ) / 1000U;
    uint64_t wanted_first = trigger_sample > requested ? trigger_sample - requested : 0;
    if (wanted_first < s_ring.first_sample_index) {
        wanted_first = s_ring.first_sample_index;
    }
    const uint64_t available_last = s_ring.next_sample_index;
    size_t count = available_last > wanted_first ? (size_t)(available_last - wanted_first) : 0;
    if (count > destination_samples) {
        wanted_first += count - destination_samples;
        count = destination_samples;
    }
    const size_t oldest_pos = (s_ring.write_pos + s_ring.capacity - s_ring.count) % s_ring.capacity;
    for (size_t i = 0; i < count; ++i) {
        const size_t offset = (size_t)(wanted_first + i - s_ring.first_sample_index);
        destination[i] = s_ring.samples[(oldest_pos + offset) % s_ring.capacity];
    }
    const uint64_t retained_before_trigger = trigger_sample > wanted_first ?
        trigger_sample - wanted_first : 0;
    *metadata = (audio_preroll_metadata_t) {
        .trigger_sample = trigger_sample,
        .saved_first_sample = wanted_first,
        .requested_preroll_ms = s_ring.requested_ms,
        .actual_preroll_ms = (uint32_t)((retained_before_trigger * 1000U) /
                                       AUDIO_SAMPLE_RATE_HZ),
        .wrap_count = s_ring.wrap_count,
        .overrun_count = s_ring.overrun_count,
    };
    xSemaphoreGive(s_ring.lock);
    return count;
}
