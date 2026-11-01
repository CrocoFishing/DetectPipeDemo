/* Compile the actual imported implementations with host-only dependency shims.
 * Storage injection exercises partial writes and flush/close failures, while
 * all WAV bytes and ring sample values are verified through independent reads.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "audio_types.h"
#include "wav_writer.h"
#include "audio_ring_buffer.h"
#include "../../firmware/components/recorder/private_include/recorder_queue_policy.h"

int fail_psram_allocation = 0;
void audio_stats_lock(void) {}
void audio_stats_unlock(void) {}
static int short_write_once, fail_write_once, fail_flush, fail_close;
static size_t injected_write(const void *data, size_t size, size_t count, FILE *file) {
    if (fail_write_once) { fail_write_once = 0; return 0; }
    if (short_write_once) { short_write_once = 0; return fwrite(data, size, count ? count - 1 : 0, file); }
    return fwrite(data, size, count, file);
}
static int injected_flush(FILE *file) { return fail_flush ? EOF : fflush(file); }
static int injected_close(FILE *file) {
    int result = fclose(file);
    return fail_close ? EOF : result;
}
static size_t host_strlcpy(char *dest, const char *source, size_t size) {
    size_t len = strlen(source);
    if (size) {
        size_t copied = len < size - 1 ? len : size - 1;
        memcpy(dest, source, copied);
        dest[copied] = '\0';
    }
    return len;
}
#define strlcpy host_strlcpy
#define fwrite injected_write
#define fflush injected_flush
#define fclose injected_close
#include "../../firmware/components/recorder/wav_writer.c"
#undef fwrite
#undef fflush
#undef fclose
#include "../../firmware/components/ring_buffer/audio_ring_buffer.c"

static uint32_t read_u32(const uint8_t *bytes) {
    return bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}
static void check_wav(const char *path, const int16_t *expected, size_t count) {
    FILE *file = fopen(path, "rb");
    assert(file);
    uint8_t header[44];
    assert(fread(header, 1, sizeof(header), file) == sizeof(header));
    assert(!memcmp(header, "RIFF", 4) && !memcmp(header + 8, "WAVEfmt ", 8));
    assert(read_u32(header + 4) == 36 + count * 2);
    assert(header[20] == 1 && header[22] == 1 && header[34] == 16);
    assert(read_u32(header + 24) == 16000 && read_u32(header + 28) == 32000);
    assert(read_u32(header + 40) == count * 2);
    for (size_t i = 0; i < count; ++i) {
        int16_t sample;
        assert(fread(&sample, sizeof(sample), 1, file) == 1 && sample == expected[i]);
    }
    assert(fgetc(file) == EOF);
    fclose(file);
}
static void test_wav(void) {
    const char *path = "audio_host_test.wav";
    int16_t samples[] = {-32768, -100, 0, 100, 32767};
    wav_writer_t writer;
    assert(wav_writer_open(&writer, path, 16000, 1, 16) == ESP_OK);
    assert(wav_writer_write(&writer, samples, 5) == ESP_OK);
    assert(wav_writer_close(&writer) == ESP_OK && !writer.file);
    check_wav(path, samples, 5);

    assert(wav_writer_open(&writer, path, 16000, 1, 16) == ESP_OK);
    short_write_once = 1;
    assert(wav_writer_write(&writer, samples, 5) == ESP_FAIL && writer.pcm_bytes == 8);
    assert(wav_writer_close(&writer) == ESP_OK);
    check_wav(path, samples, 4);

    fail_write_once = 1;
    assert(wav_writer_open(&writer, path, 16000, 1, 16) == ESP_FAIL && !writer.file);
    assert(wav_writer_open(&writer, path, 16000, 1, 16) == ESP_OK);
    assert(wav_writer_write(&writer, samples, 5) == ESP_OK);
    fail_flush = 1;
    assert(wav_writer_flush(&writer) == ESP_FAIL);
    assert(wav_writer_close(&writer) == ESP_FAIL && !writer.file);
    fail_flush = 0;

    assert(wav_writer_open(&writer, path, 16000, 1, 16) == ESP_OK);
    fail_close = 1;
    assert(wav_writer_close(&writer) == ESP_FAIL && !writer.file);
    fail_close = 0;
    assert(wav_writer_open(&writer, path, 16000, 1, 16) == ESP_OK);
    writer.pcm_bytes = UINT32_MAX;
    assert(wav_writer_close(&writer) == ESP_ERR_INVALID_SIZE && !writer.file);

    assert(wav_writer_open(&writer, path, 16000, 1, 16) == ESP_OK);
    assert(wav_writer_write(&writer, samples, 5) == ESP_OK);
    fclose(writer.file); /* interrupted recording, unpatched header */
    writer.file = NULL;
    uint64_t repaired = 0;
    assert(wav_writer_repair(path, &repaired) == ESP_OK && repaired == 10);
    check_wav(path, samples, 5);
    assert(wav_writer_repair(path, &repaired) == ESP_OK && repaired == 0);

    assert(wav_writer_open(&writer, path, 16000, 1, 16) == ESP_OK);
    fputc(1, writer.file); /* incomplete PCM16 byte is not repairable */
    fclose(writer.file);
    writer.file = NULL;
    assert(wav_writer_repair(path, &repaired) == ESP_ERR_INVALID_SIZE);
    FILE *bad = fopen(path, "wb");
    assert(bad);
    for (unsigned i = 0; i < 44; ++i) fputc(0, bad);
    fclose(bad);
    assert(wav_writer_repair(path, &repaired) == ESP_ERR_INVALID_RESPONSE);
    assert(wav_writer_open(&writer, "missing_dir/event.wav", 16000, 1, 16) == ESP_FAIL);
    remove(path);
}
static void test_ring(void) {
    audio_runtime_stats_t stats = {0};
    fail_psram_allocation = 1; /* exercise internal-heap fallback */
    assert(audio_ring_buffer_init(100, &stats) == ESP_OK);
    assert(audio_ring_buffer_set_preroll(101) == ESP_ERR_INVALID_ARG);
    assert(audio_ring_buffer_set_preroll(50) == ESP_OK);
    int16_t input[800], copy[1600];
    audio_frame_t frame = {.samples = input, .sample_count = 800, .sample_rate = 16000};
    for (uint64_t first = 1000; first < 4200; first += 800) {
        frame.first_sample_index = first;
        for (size_t i = 0; i < 800; ++i) input[i] = (int16_t)(first + i);
        audio_ring_buffer_push(&frame);
    }
    audio_preroll_metadata_t metadata;
    size_t count = audio_ring_buffer_copy_event_start(3800, copy, 1600, &metadata);
    assert(count == 1200 && metadata.saved_first_sample == 3000 && metadata.actual_preroll_ms == 50);
    assert(metadata.wrap_count == 2 && metadata.overrun_count == 0);
    for (size_t i = 0; i < count; ++i) assert(copy[i] == (int16_t)(3000 + i));
    count = audio_ring_buffer_copy_preroll(3800, copy, 1600, &metadata);
    assert(count == 800 && metadata.saved_first_sample == 3000);
    for (size_t i = 0; i < count; ++i) assert(copy[i] == (int16_t)(3000 + i));

    assert(audio_ring_buffer_set_preroll(0) == ESP_OK);
    assert(audio_ring_buffer_copy_preroll(3800, copy, 1600, &metadata) == 0);
    count = audio_ring_buffer_copy_event_start(3800, copy, 1600, &metadata);
    assert(count == 400 && metadata.actual_preroll_ms == 0);
    for (size_t i = 0; i < count; ++i) assert(copy[i] == (int16_t)(3800 + i));

    frame.first_sample_index = 10000;
    frame.sample_count = 320;
    for (size_t i = 0; i < 320; ++i) input[i] = (int16_t)(10000 + i);
    audio_ring_buffer_push(&frame);
    assert(stats.ring_overruns == 1);
    assert(audio_ring_buffer_set_preroll(80) == ESP_OK);
    count = audio_ring_buffer_copy_event_start(10080, copy, 1600, &metadata);
    assert(count == 320 && metadata.saved_first_sample == 10000 && metadata.actual_preroll_ms == 5);
    assert(metadata.overrun_count == 1);
    for (size_t i = 0; i < count; ++i) assert(copy[i] == (int16_t)(10000 + i));
    count = audio_ring_buffer_copy_event_start(10160, copy, 64, &metadata);
    assert(count == 64 && metadata.saved_first_sample == 10256 && metadata.actual_preroll_ms == 0);
    for (size_t i = 0; i < count; ++i) assert(copy[i] == (int16_t)(10256 + i));
    audio_ring_buffer_reset();
    assert(audio_ring_buffer_get_preroll() == 80);
    assert(audio_ring_buffer_copy_event_start(10320, copy, 1600, &metadata) == 0);
    frame.first_sample_index = 10320;
    audio_ring_buffer_push(&frame);
    count = audio_ring_buffer_copy_event_start(10400, copy, 1600, &metadata);
    assert(count == 320 && metadata.saved_first_sample == 10320);
    assert(stats.ring_overruns == 1); // An intentional pause is not a dropped frame.
    audio_ring_buffer_deinit();
}
int main(void) {
    test_wav();
    test_ring();
    /* Worst-case writer stall: once BEGIN is admitted, repeated data/metadata
     * attempts cannot consume END's reserved slot. Test every queue occupancy,
     * rather than relying on timing or a particular number of PCM frames.
     */
    for (size_t occupied = 0; occupied <= 32; ++occupied) {
        size_t slots = 32 - occupied;
        if (!recorder_queue_admits(PACKET_BEGIN, slots)) continue;
        --slots;
        for (unsigned i = 0; i < 100; ++i) {
            packet_type_t kind = i % 2 ? PACKET_AUDIO : PACKET_METADATA;
            if (recorder_queue_admits(kind, slots)) --slots;
        }
        assert(recorder_queue_admits(PACKET_END, slots));
        --slots;
        assert(slots <= 32);
    }
    puts("PASS: actual WAV header/PCM, short-write, open/flush/close failures, power-loss repair, malformed input; raw pre-roll wrap/zero/fallback/discontinuity/bounded copy; END admission during writer stalls");
    return 0;
}
