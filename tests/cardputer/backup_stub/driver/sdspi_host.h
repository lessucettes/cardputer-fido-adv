#pragma once
#include "driver/spi_common.h"

typedef struct {
    spi_host_device_t host_id;
    gpio_num_t gpio_cs;
    gpio_num_t gpio_cd;
    gpio_num_t gpio_wp;
} sdspi_device_config_t;

#define SDSPI_DEVICE_CONFIG_DEFAULT() \
    { SPI2_HOST, 13, GPIO_NUM_NC, GPIO_NUM_NC }
