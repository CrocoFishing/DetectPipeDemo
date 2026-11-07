/* Exercise real recorder admission/copies with a paused writer and a bounded
 * FIFO backed by the actual production queue allocation. No SD I/O or timing
 * shim claims hardware scheduling or sustained card throughput. */
#define RECORDER_QUEUE_TEST 1
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "recorder_host_shims.h"
#include "audio_types.h"
#include "wav_writer.h"

typedef struct { void *pointer; size_t size; } allocation_t;
static allocation_t allocations[8];
static unsigned live_allocations;
static QueueHandle_t live_queue;
static UBaseType_t queued, read_index, write_index;
static unsigned fifo_wraps;
static void host_free(void *memory);
static size_t host_strlcpy(char *dest, const char *source, size_t bytes) {
    size_t length = strlen(source);
    if (bytes) {
        size_t copy = length < bytes - 1 ? length : bytes - 1;
        memcpy(dest, source, copy); dest[copy] = '\0';
    }
    return length;
}
void audio_stats_lock(void) {}
void audio_stats_unlock(void) {}

#define free host_free
#define strlcpy host_strlcpy
#include "../../firmware/components/recorder/sd_recorder.c"
#undef strlcpy
#undef free

void *heap_caps_malloc(size_t bytes, unsigned caps) {
    assert(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    for (size_t i = 0; i < sizeof(allocations) / sizeof(allocations[0]); ++i) {
        if (allocations[i].pointer) continue;
        allocations[i].pointer = malloc(bytes);
        allocations[i].size = bytes;
        assert(allocations[i].pointer);
        ++live_allocations;
        return allocations[i].pointer;
    }
    assert(!"unexpected live allocation count");
    return NULL;
}
static void host_free(void *memory) {
    if (!memory) return;
    for (size_t i = 0; i < sizeof(allocations) / sizeof(allocations[0]); ++i) {
        if (allocations[i].pointer != memory) continue;
        if (memory == s_recorder.queue_storage) assert(!live_queue);
        allocations[i].pointer = NULL; allocations[i].size = 0;
        assert(live_allocations); --live_allocations; free(memory); return;
    }
    assert(!"free of unowned or already freed memory");
}
void heap_caps_free(void *memory) { host_free(memory); }
QueueHandle_t xQueueCreate(UBaseType_t depth, UBaseType_t item_bytes) {
    (void)depth; (void)item_bytes;
    assert(!"queue backing must use the recorder's PSRAM allocation"); return NULL;
}
QueueHandle_t xQueueCreateStatic(UBaseType_t depth, UBaseType_t item_bytes,
                                uint8_t *storage, StaticQueue_t *control) {
    assert(!live_queue && depth == RECORDER_QUEUE_DEPTH && item_bytes == sizeof(recorder_packet_t));
    bool found = false;
    for (size_t i = 0; i < sizeof(allocations) / sizeof(allocations[0]); ++i) {
        if (allocations[i].pointer == storage) {
            assert(allocations[i].size == depth * item_bytes); found = true;
        }
    }
    assert(found && control);
    control->storage = storage; control->depth = depth; control->item_bytes = item_bytes;
    live_queue = control; queued = read_index = write_index = 0;
    return control;
}
void vQueueDelete(QueueHandle_t queue) {
    assert(queue == live_queue && !queued); live_queue = NULL;
}
UBaseType_t uxQueueSpacesAvailable(QueueHandle_t queue) {
    assert(queue == live_queue && queued <= queue->depth); return queue->depth - queued;
}
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t queue) {
    assert(queue == live_queue); return queued;
}
BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t wait) {
    (void)wait;
    assert(queue == live_queue && item);
    if (queued == queue->depth) return 0;
    memcpy(queue->storage + (size_t)write_index * queue->item_bytes, item, queue->item_bytes);
    write_index = (write_index + 1) % queue->depth;
    if (!write_index) ++fifo_wraps;
    ++queued; return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t wait) {
    (void)wait;
    assert(queue == live_queue && item);
    if (!queued) return 0;
    memcpy(item, queue->storage + (size_t)read_index * queue->item_bytes, queue->item_bytes);
    read_index = (read_index + 1) % queue->depth;
    --queued; return pdTRUE;
}

/* Link-only dependencies: these tests pause the writer instead of calling it. */
esp_err_t wav_writer_open(wav_writer_t *writer, const char *path,
        uint32_t rate, uint16_t channels, uint16_t bits) {
    (void)writer; (void)path; (void)rate; (void)channels; (void)bits; return ESP_FAIL;
}
esp_err_t wav_writer_write(wav_writer_t *writer, const int16_t *samples, size_t count) {
    (void)writer; (void)samples; (void)count; return ESP_FAIL;
}
esp_err_t wav_writer_flush(wav_writer_t *writer) { (void)writer; return ESP_FAIL; }
esp_err_t wav_writer_close(wav_writer_t *writer) { (void)writer; return ESP_FAIL; }
esp_err_t wav_writer_repair(const char *path, uint64_t *bytes) {
    (void)path; (void)bytes; return ESP_FAIL;
}

static recorder_packet_t receive_packet(void) {
    recorder_packet_t packet;
    assert(xQueueReceive(live_queue, &packet, 0) == pdTRUE); return packet;
}
static void fill_pcm(int16_t *samples, size_t count, unsigned frame_index) {
    for (size_t i = 0; i < count; ++i) samples[i] = (int16_t)(frame_index * 320 + (unsigned)i);
}
static void test_writer_stall(audio_runtime_stats_t *stats, unsigned frame_count) {
    const uint64_t id = UINT64_C(0x0000000B00000001) + frame_count;
    const int64_t duration_us = (int64_t)frame_count * 20000;
    int16_t samples[320]; fill_pcm(samples, 320, 0);
    audio_frame_t frame = {.samples = samples, .sample_count = 320, .sample_rate = 16000,
        .first_sample_index = 700, .timestamp_us = 0};
    sd_event_metadata_t metadata = {.event_id = id, .trigger_sample = 1020};
    assert(sd_recorder_event_begin(&frame, &metadata));
    frame.first_sample_index = 1020;
    // One incoming320-sample frame every20ms; the writer consumes none.
    for (unsigned i = 0; i < frame_count; ++i) {
        fill_pcm(samples, 320, i + 1);
        assert(sd_recorder_event_submit(id, &frame));
        memset(samples, 0xff, sizeof(samples)); // Caller reuses the capture buffer.
        frame.first_sample_index += 320;
    }
    assert(sd_recorder_event_end(id, duration_us, frame.first_sample_index, false));
    assert(queued == frame_count + 2 && stats->queue_overruns == 0 && stats->dropped_samples == 0);
    assert(stats->sd_metadata_drops == 0 && stats->sd_queue_high_water == frame_count + 2);
    recorder_packet_t packet = receive_packet();
    assert(packet.type == PACKET_BEGIN && packet.event_id == id && packet.count == 320);
    assert(packet.first_sample_index == 700 && packet.metadata.trigger_sample == 1020);
    assert(packet.owned_samples && packet.owned_samples != samples);
    for (unsigned i = 0; i < 320; ++i) assert(packet.owned_samples[i] == (int16_t)i);
    host_free(packet.owned_samples);
    for (unsigned i = 0; i < frame_count; ++i) {
        packet = receive_packet();
        assert(packet.type == PACKET_AUDIO && packet.event_id == id && packet.count == 320);
        assert(packet.first_sample_index == 1020 + (uint64_t)i * 320);
        for (unsigned j = 0; j < 320; ++j)
            assert(packet.data.samples[j] == (int16_t)((i + 1) * 320 + j));
    }
    packet = receive_packet();
    assert(packet.type == PACKET_END && packet.event_id == id && !packet.producer_failed);
    assert(packet.first_sample_index == frame.first_sample_index && packet.timestamp_us == duration_us);
    assert(!queued && live_allocations == 1);
}
static void test_saturation_preserves_end(audio_runtime_stats_t *stats) {
    const uint64_t id = UINT64_C(0x0000000B00000002);
    const unsigned capacity = live_queue->depth;
    int16_t samples[320] = {0};
    audio_frame_t empty = {.sample_count = 0, .first_sample_index = 20000};
    sd_event_metadata_t metadata = {.event_id = id, .trigger_sample = 20000};
    assert(sd_recorder_event_begin(&empty, &metadata));
    audio_frame_t frame = {.samples = samples, .sample_count = 320, .sample_rate = 16000,
        .first_sample_index = 20000};
    unsigned frames = 0;
    while (uxQueueSpacesAvailable(live_queue) > 2) {
        assert(sd_recorder_event_submit(id, &frame)); ++frames; frame.first_sample_index += 320;
    }
    assert(frames == capacity - 3 && queued == capacity - 2);
    assert(!sd_recorder_event_submit(id, &frame));
    assert(!sd_recorder_metadata_append(SD_METADATA_EVENT_LIFECYCLE, "0000000B00000002,OPEN,0,OK"));
    assert(queued == capacity - 2 && stats->queue_overruns == 1 && stats->dropped_samples == 320);
    assert(stats->sd_metadata_drops == 1 && stats->sd_queue_high_water == capacity - 2);
    assert(sd_recorder_event_end(id, 1240000, frame.first_sample_index, true));
    assert(queued == capacity - 1 && uxQueueSpacesAvailable(live_queue) == 1);
    // A new utterance cannot spend the final reserved control slot. Its
    // rejected pre-roll allocation must be freed immediately.
    metadata.event_id = id + 1;
    assert(!sd_recorder_event_begin(&frame, &metadata));
    assert(queued == capacity - 1 && live_allocations == 1);
    recorder_packet_t packet = receive_packet();
    assert(packet.type == PACKET_BEGIN && packet.event_id == id && !packet.owned_samples);
    for (unsigned i = 0; i < frames; ++i) {
        packet = receive_packet();
        assert(packet.type == PACKET_AUDIO && packet.event_id == id);
        assert(packet.first_sample_index == 20000 + (uint64_t)i * 320);
    }
    packet = receive_packet();
    assert(packet.type == PACKET_END && packet.event_id == id && packet.producer_failed);
    assert(packet.first_sample_index == frame.first_sample_index);
    assert(!queued && fifo_wraps > 0);
}
int main(void) {
    audio_runtime_stats_t stats = {0};
    sd_recorder_config_t config = {.stats = &stats};
    assert(sd_recorder_init(&config) == ESP_OK);
    s_recorder.running = true; // Model an initialized writer paused before dequeue.
    test_writer_stall(&stats, 49);  //980ms writer stall.
    test_writer_stall(&stats, 249); //4980ms writer stall.
    test_saturation_preserves_end(&stats);
    s_recorder.running = false;
    sd_recorder_deinit();
    assert(!live_queue && !live_allocations);
    puts("PASS: actual recorder buffers980ms and4980ms stalled writer without loss; bounded FIFO copies/order, saturation counters, END admission, rejected BEGIN cleanup");
    return 0;
}
