#include "audio_dsp.h"

#include <limits.h>

typedef struct {
    int db;
    int32_t multiplier_q15;
} gain_entry_t;

static const gain_entry_t s_gains[] = {
    {0, 32768},
    {6, 65382},
    {12, 130463},
    {18, 260309},
    {24, 519321},
};

static const gain_entry_t *find_gain(int db)
{
    for (size_t i = 0; i < sizeof(s_gains) / sizeof(s_gains[0]); ++i) {
        if (s_gains[i].db == db) {
            return &s_gains[i];
        }
    }
    return NULL;
}

bool audio_dsp_gain_is_supported(int gain_db)
{
    return find_gain(gain_db) != NULL;
}

size_t audio_fixed_gain_apply(const int16_t *input, int16_t *output, size_t count,
                              int gain_db, uint64_t *clipped_samples)
{
    const gain_entry_t *gain = find_gain(gain_db);
    if (!input || !output || !gain) {
        return 0;
    }
    uint64_t clipped = 0;
    for (size_t i = 0; i < count; ++i) {
        int64_t scaled = ((int64_t)input[i] * gain->multiplier_q15 + 16384) >> 15;
        if (scaled > INT16_MAX) {
            scaled = INT16_MAX;
            clipped++;
        } else if (scaled < INT16_MIN) {
            scaled = INT16_MIN;
            clipped++;
        }
        output[i] = (int16_t)scaled;
    }
    if (clipped_samples) {
        *clipped_samples += clipped;
    }
    return count;
}
