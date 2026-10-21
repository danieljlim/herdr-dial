#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

void i2c_bus_init(void);
esp_err_t i2c_bus_write(uint8_t addr, uint8_t reg, const uint8_t *buf, uint8_t len);
esp_err_t i2c_bus_read(uint8_t addr, uint8_t reg, uint8_t *buf, uint8_t len);

#ifdef __cplusplus
}
#endif
