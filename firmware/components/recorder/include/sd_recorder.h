#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "audio_types.h"
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t event_id;
    uint64_t audio_first_sample;
    uint64_t audio_last_sample; /* exclusive; actual saved capture boundary */
    uint64_t vad_start_sample;
    uint64_t vad_end_sample;
    int64_t vad_start_timestamp_us;
    int64_t vad_end_timestamp_us;
    bool ready;
    esp_err_t error;
    char audio_path[96];
} sd_event_result_t;
typedef void (*sd_event_terminal_cb_t)(const sd_event_result_t *result, void *ctx);
typedef struct {
    audio_runtime_stats_t *stats;
    sd_event_terminal_cb_t terminal_cb;
    void *callback_ctx;
} sd_recorder_config_t;
typedef struct {
    uint64_t event_id;
    uint64_t trigger_sample;
    uint64_t saved_first_sample;
    int64_t vad_start_timestamp_us;
    uint32_t requested_preroll_ms;
    uint32_t actual_preroll_ms;
    uint32_t wrap_count;
    uint32_t overrun_count;
} sd_event_metadata_t;
typedef enum {
    SD_METADATA_AUDIO_EVENTS,
    SD_METADATA_FACE_ASSOCIATIONS,
    SD_METADATA_EVENT_LIFECYCLE,
} sd_metadata_kind_t;
esp_err_t sd_recorder_init(const sd_recorder_config_t *config);
esp_err_t sd_recorder_start(void);
esp_err_t sd_recorder_stop(void);
bool sd_recorder_is_running(void);
bool sd_recorder_is_mounted(void);
// Native FatFS reads avoid signed 32-bit VFS offsets for 2 to 4 GiB FAT32 files.
// Only call after the export barrier and retain snapshot ownership until close.
esp_err_t sd_recorder_fatfs_drive(char *destination, size_t capacity);
// Caller first gates all producers and waits for their quiescence. A successful
// acknowledgement means every earlier packet is closed and no WAV owner remains.
esp_err_t sd_recorder_flush_barrier(uint32_t timeout_ms);
bool sd_recorder_event_begin(const audio_frame_t *buffered_audio, const sd_event_metadata_t *metadata);
bool sd_recorder_event_submit(uint64_t event_id, const audio_frame_t *frame);
bool sd_recorder_event_end(uint64_t event_id, int64_t timestamp_us, uint64_t sample_index, bool producer_failed);
/* Copies one CSV row, max 511 bytes, no wait. Writer adds headers/newlines. */
bool sd_recorder_metadata_append(sd_metadata_kind_t kind, const char *csv_row);
void sd_recorder_deinit(void);
#ifdef __cplusplus
}
#endif
