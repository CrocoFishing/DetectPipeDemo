#include "segment_manager.h"

#include <dirent.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wav_writer.h"

static bool parse_numbered_name(const char *name, const char *prefix, uint32_t *number)
{
    unsigned parsed = 0;
    int used = 0;
    char pattern[32];
    snprintf(pattern, sizeof(pattern), "%s_%%6u.wav%%n", prefix);
    if (sscanf(name, pattern, &parsed, &used) != 1 ||
        name[used] != '\0') {
        return false;
    }
    *number = parsed;
    return true;
}

esp_err_t segment_manager_init(segment_manager_t *manager, const char *mount_point)
{
    if (!manager || !mount_point) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(manager, 0, sizeof(*manager));
    strlcpy(manager->mount_point, mount_point, sizeof(manager->mount_point));
    DIR *directory = opendir(mount_point);
    if (!directory) {
        return ESP_FAIL;
    }
    uint32_t highest = 0;
    uint32_t highest_event = 0;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        uint32_t number = 0;
        if (parse_numbered_name(entry->d_name, "audio", &number) && number > highest) {
            highest = number;
        }
        if (parse_numbered_name(entry->d_name, "event", &number) &&
            number > highest_event) {
            highest_event = number;
        }
    }
    closedir(directory);
    manager->next_number = highest + 1;
    manager->next_event_number = highest_event + 1;
    return ESP_OK;
}

esp_err_t segment_manager_next_event_path(segment_manager_t *manager, char *path,
                                          size_t path_size, uint32_t *event_number)
{
    if (!manager || !path || path_size == 0 || !event_number) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint32_t number = manager->next_event_number++;
    int length = snprintf(path, path_size, "%s/event_%06" PRIu32 ".wav",
                          manager->mount_point, number);
    if (length < 0 || (size_t)length >= path_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    *event_number = number;
    return ESP_OK;
}

esp_err_t segment_manager_next_path(segment_manager_t *manager, char *path, size_t path_size,
                                    uint32_t *segment_number)
{
    if (!manager || !path || path_size == 0 || !segment_number) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint32_t number = manager->next_number++;
    int length = snprintf(path, path_size, "%s/audio_%06" PRIu32 ".wav",
                          manager->mount_point, number);
    if (length < 0 || (size_t)length >= path_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    *segment_number = number;
    return ESP_OK;
}

esp_err_t segment_manager_log_closed(const segment_manager_t *manager, uint32_t segment_number,
                                     const char *path, uint64_t first_sample_index,
                                     uint64_t sample_count)
{
    char csv_path[64];
    snprintf(csv_path, sizeof(csv_path), "%s/segments.csv", manager->mount_point);
    FILE *file = fopen(csv_path, "a+");
    if (!file) {
        return ESP_FAIL;
    }
    if (fseek(file, 0, SEEK_END) == 0 && ftell(file) == 0) {
        fputs("segment_number,file,first_sample_index,sample_count\n", file);
    }
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    const int written = fprintf(file, "%" PRIu32 ",%s,%" PRIu64 ",%" PRIu64 "\n",
                                segment_number, name, first_sample_index, sample_count);
    const int flushed = fflush(file);
    const int closed = fclose(file);
    return written > 0 && flushed == 0 && closed == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t segment_manager_recover_newest(const segment_manager_t *manager)
{
    if (!manager) {
        return ESP_ERR_INVALID_ARG;
    }
    bool attempted = false;
    esp_err_t overall = ESP_OK;
    const char *prefixes[] = {"audio", "event"};
    const uint32_t next_numbers[] = {manager->next_number, manager->next_event_number};
    for (size_t i = 0; i < 2; ++i) {
        if (next_numbers[i] <= 1) {
            continue;
        }
        attempted = true;
        char path[192];
        snprintf(path, sizeof(path), "%s/%s_%06" PRIu32 ".wav",
                 manager->mount_point, prefixes[i], next_numbers[i] - 1);
        uint64_t repaired = 0;
        esp_err_t err = wav_writer_repair(path, &repaired);
        if (err != ESP_OK) {
            printf("RECOVERY,failed,%s,%s\n", path, esp_err_to_name(err));
            overall = err;
        } else if (repaired) {
            printf("RECOVERY,repaired,%s,%" PRIu64 "\n", path, repaired);
        } else {
            printf("RECOVERY,no_action,%s\n", path);
        }
    }
    if (!attempted) {
        printf("RECOVERY,no_action\n");
    }
    return overall;
}
