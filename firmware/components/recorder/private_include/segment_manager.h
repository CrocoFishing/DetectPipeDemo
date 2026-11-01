#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    char mount_point[32];
    uint32_t next_number;
    uint32_t next_event_number;
} segment_manager_t;

esp_err_t segment_manager_init(segment_manager_t *manager, const char *mount_point);
esp_err_t segment_manager_next_path(segment_manager_t *manager, char *path, size_t path_size,
                                    uint32_t *segment_number);
esp_err_t segment_manager_next_event_path(segment_manager_t *manager, char *path,
                                          size_t path_size, uint32_t *event_number);
esp_err_t segment_manager_log_closed(const segment_manager_t *manager, uint32_t segment_number,
                                     const char *path, uint64_t first_sample_index,
                                     uint64_t sample_count);
esp_err_t segment_manager_recover_newest(const segment_manager_t *manager);
