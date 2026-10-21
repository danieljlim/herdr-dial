// Uses the legacy I2C driver, as Waveshare's demos do on this board.
#include "i2c_bus.h"

#include "board.h"
#include "driver/i2c.h"

#define I2C_PORT I2C_NUM_0
#define I2C_TIMEOUT_TICKS 100

void i2c_bus_init(void)
{
    const i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master = {.clk_speed = 300 * 1000},
    };
    ESP_ERROR_CHECK(i2c_param_config(I2C_PORT, &conf));
    ESP_ERROR_CHECK(i2c_driver_install(I2C_PORT, conf.mode, 0, 0, 0));
}

esp_err_t i2c_bus_write(uint8_t addr, uint8_t reg, const uint8_t *buf, uint8_t len)
{
    uint8_t frame[16];
    if (len >= sizeof(frame)) {
        return ESP_ERR_INVALID_SIZE;
    }
    frame[0] = reg;
    for (uint8_t i = 0; i < len; i++) {
        frame[i + 1] = buf[i];
    }
    return i2c_master_write_to_device(I2C_PORT, addr, frame, len + 1, I2C_TIMEOUT_TICKS);
}

esp_err_t i2c_bus_read(uint8_t addr, uint8_t reg, uint8_t *buf, uint8_t len)
{
    return i2c_master_write_read_device(I2C_PORT, addr, &reg, 1, buf, len, I2C_TIMEOUT_TICKS);
}
