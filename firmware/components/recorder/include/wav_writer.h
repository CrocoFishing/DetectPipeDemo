#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    FILE *file;
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits_per_sample;
    uint64_t pcm_bytes;
    char path[192];
} wav_writer_t;

esp_err_t wav_writer_open(wav_writer_t *writer, const char *path,
                          uint32_t sample_rate, uint16_t channels,
                          uint16_t bits_per_sample);
esp_err_t wav_writer_write(wav_writer_t *writer, const int16_t *samples,
                           size_t sample_count);
esp_err_t wav_writer_flush(wav_writer_t *writer);
esp_err_t wav_writer_close(wav_writer_t *writer);
esp_err_t wav_writer_repair(const char *path, uint64_t *repaired_pcm_bytes);

#ifdef __cplusplus
}
#endif
