#pragma once
#include "driver/spi_common.h"

typedef struct {
    int slot;
    int max_freq_khz;
} sdmmc_host_t;

typedef struct {
    int reserved;
} sdmmc_card_t;

#define SDMMC_FREQ_DEFAULT 20000

#define SDSPI_HOST_DEFAULT() \
    { SPI2_HOST, SDMMC_FREQ_DEFAULT }
