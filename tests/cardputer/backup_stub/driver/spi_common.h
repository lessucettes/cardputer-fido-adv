#pragma once
#include <stdint.h>

#include "esp_err.h"

typedef int gpio_num_t;
typedef int spi_host_device_t;

#define GPIO_NUM_12 12
#define GPIO_NUM_14 14
#define GPIO_NUM_39 39
#define GPIO_NUM_40 40
#define GPIO_NUM_NC (-1)

#define SPI2_HOST 1
#define SPI3_HOST 2
#define SPI_DMA_CH_AUTO 3

typedef struct {
    int mosi_io_num;
    int miso_io_num;
    int sclk_io_num;
    int quadwp_io_num;
    int quadhd_io_num;
    int max_transfer_sz;
} spi_bus_config_t;

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t spi_bus_initialize(spi_host_device_t host_id,
                             const spi_bus_config_t *bus_config,
                             int dma_chan);
esp_err_t spi_bus_free(spi_host_device_t host_id);

#ifdef __cplusplus
}
#endif
