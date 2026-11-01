#include "wav_writer.h"

#include <errno.h>
#include <string.h>
#include <sys/stat.h>

static void put_u16_le(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8);
}

static void put_u32_le(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8);
    dst[2] = (uint8_t)(value >> 16);
    dst[3] = (uint8_t)(value >> 24);
}

static uint32_t get_u32_le(const uint8_t *src)
{
    return (uint32_t)src[0] | ((uint32_t)src[1] << 8) |
           ((uint32_t)src[2] << 16) | ((uint32_t)src[3] << 24);
}

static void make_header(uint8_t header[44], uint32_t sample_rate, uint16_t channels,
                        uint16_t bits_per_sample, uint32_t pcm_bytes)
{
    memset(header, 0, 44);
    memcpy(&header[0], "RIFF", 4);
    put_u32_le(&header[4], pcm_bytes + 36U);
    memcpy(&header[8], "WAVEfmt ", 8);
    put_u32_le(&header[16], 16);
    put_u16_le(&header[20], 1);
    put_u16_le(&header[22], channels);
    put_u32_le(&header[24], sample_rate);
    const uint16_t block_align = channels * (bits_per_sample / 8U);
    put_u32_le(&header[28], sample_rate * block_align);
    put_u16_le(&header[32], block_align);
    put_u16_le(&header[34], bits_per_sample);
    memcpy(&header[36], "data", 4);
    put_u32_le(&header[40], pcm_bytes);
}

esp_err_t wav_writer_open(wav_writer_t *writer, const char *path,
                          uint32_t sample_rate, uint16_t channels,
                          uint16_t bits_per_sample)
{
    if (!writer || !path || channels == 0 || bits_per_sample != 16) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(writer, 0, sizeof(*writer));
    writer->file = fopen(path, "wb");
    if (!writer->file) {
        return ESP_FAIL;
    }
    writer->sample_rate = sample_rate;
    writer->channels = channels;
    writer->bits_per_sample = bits_per_sample;
    strlcpy(writer->path, path, sizeof(writer->path));
    uint8_t header[44];
    make_header(header, sample_rate, channels, bits_per_sample, 0);
    if (fwrite(header, 1, sizeof(header), writer->file) != sizeof(header)) {
        fclose(writer->file);
        memset(writer, 0, sizeof(*writer));
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t wav_writer_write(wav_writer_t *writer, const int16_t *samples,
                           size_t sample_count)
{
    if (!writer || !writer->file || (!samples && sample_count)) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t written = fwrite(samples, sizeof(int16_t), sample_count, writer->file);
    writer->pcm_bytes += written * sizeof(int16_t);
    return written == sample_count ? ESP_OK : ESP_FAIL;
}

esp_err_t wav_writer_flush(wav_writer_t *writer)
{
    return writer && writer->file && fflush(writer->file) == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t wav_writer_close(wav_writer_t *writer)
{
    if (!writer || !writer->file) {
        return ESP_ERR_INVALID_STATE;
    }
    if (writer->pcm_bytes > UINT32_MAX - 36U) {
        fclose(writer->file);
        writer->file = NULL;
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t header[44];
    make_header(header, writer->sample_rate, writer->channels,
                writer->bits_per_sample, (uint32_t)writer->pcm_bytes);
    esp_err_t result = ESP_OK;
    if (fflush(writer->file) != 0 || fseek(writer->file, 0, SEEK_SET) != 0 ||
        fwrite(header, 1, sizeof(header), writer->file) != sizeof(header) ||
        fflush(writer->file) != 0) {
        result = ESP_FAIL;
    }
    if (fclose(writer->file) != 0) {
        result = ESP_FAIL;
    }
    writer->file = NULL;
    return result;
}

esp_err_t wav_writer_repair(const char *path, uint64_t *repaired_pcm_bytes)
{
    if (!path) {
        return ESP_ERR_INVALID_ARG;
    }
    struct stat info;
    if (stat(path, &info) != 0 || info.st_size < 44) {
        return ESP_ERR_INVALID_SIZE;
    }
    FILE *file = fopen(path, "r+b");
    if (!file) {
        return ESP_FAIL;
    }
    uint8_t header[44];
    if (fread(header, 1, sizeof(header), file) != sizeof(header) ||
        memcmp(&header[0], "RIFF", 4) != 0 ||
        memcmp(&header[8], "WAVEfmt ", 8) != 0 ||
        memcmp(&header[36], "data", 4) != 0) {
        fclose(file);
        return ESP_ERR_INVALID_RESPONSE;
    }
    const uint64_t payload = (uint64_t)info.st_size - 44U;
    if (payload > UINT32_MAX - 36U || (payload & 1U)) {
        fclose(file);
        return ESP_ERR_INVALID_SIZE;
    }
    const uint32_t expected_riff = (uint32_t)payload + 36U;
    const uint32_t expected_data = (uint32_t)payload;
    if (get_u32_le(&header[4]) == expected_riff &&
        get_u32_le(&header[40]) == expected_data) {
        fclose(file);
        if (repaired_pcm_bytes) {
            *repaired_pcm_bytes = 0;
        }
        return ESP_OK;
    }
    put_u32_le(&header[4], expected_riff);
    put_u32_le(&header[40], expected_data);
    esp_err_t result = ESP_OK;
    if (fseek(file, 0, SEEK_SET) != 0 ||
        fwrite(header, 1, sizeof(header), file) != sizeof(header) ||
        fflush(file) != 0) result = ESP_FAIL;
    if (fclose(file) != 0) result = ESP_FAIL;
    if (result != ESP_OK) return result;
    if (repaired_pcm_bytes) {
        *repaired_pcm_bytes = payload;
    }
    return ESP_OK;
}
