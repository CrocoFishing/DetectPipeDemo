#pragma once
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
extern int fail_psram_allocation;
static inline void *heap_caps_calloc(size_t count, size_t bytes, unsigned caps) {
    (void)caps;
    return fail_psram_allocation ? NULL : calloc(count, bytes);
}
