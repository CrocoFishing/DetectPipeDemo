#pragma once
#include "recorder_host_shims.h"
static inline unsigned char ff_diskio_get_pdrv_card(const sdmmc_card_t *card) { (void)card; return 0; }
