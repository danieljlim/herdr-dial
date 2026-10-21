// DRV2605 haptic driver, internal-trigger mode. The board's actuator is an LRA (see the schematic's
// LRA_P/LRA_N), so this uses LRA mode and the LRA effect library. The EN pin is tied high, so the
// chip's standby bit is the only way to idle it.
#include "haptics.h"

#include "esp_timer.h"
#include "i2c_bus.h"

#define DRV2605_ADDR 0x5A

#define REG_STATUS     0x00
#define REG_MODE       0x01
#define REG_RTP_INPUT  0x02
#define REG_LIBRARY    0x03
#define REG_WAVESEQ1   0x04
#define REG_WAVESEQ2   0x05
#define REG_GO         0x0C
#define REG_OVERDRIVE  0x0D
#define REG_SUSTAINPOS 0x0E
#define REG_SUSTAINNEG 0x0F
#define REG_BREAK      0x10
#define REG_AUDIOMAX   0x13
#define REG_FEEDBACK   0x1A

#define MODE_ACTIVE  0x00  // internal trigger
#define MODE_STANDBY 0x40
#define LIBRARY_LRA  6
#define STANDBY_AFTER_US (1500 * 1000)  // longer than the longest effect we play

static bool s_ready;
static bool s_standby;
static int64_t s_last_play_us;

static void write_reg(uint8_t reg, uint8_t value)
{
    i2c_bus_write(DRV2605_ADDR, reg, &value, 1);
}

static uint8_t read_reg(uint8_t reg)
{
    uint8_t value = 0;
    i2c_bus_read(DRV2605_ADDR, reg, &value, 1);
    return value;
}

bool haptics_init(void)
{
    uint8_t status;
    if (i2c_bus_read(DRV2605_ADDR, REG_STATUS, &status, 1) != ESP_OK) {
        return false;
    }
    write_reg(REG_MODE, MODE_ACTIVE);
    write_reg(REG_RTP_INPUT, 0x00);
    write_reg(REG_WAVESEQ1, 1);
    write_reg(REG_WAVESEQ2, 0);
    write_reg(REG_OVERDRIVE, 0);
    write_reg(REG_SUSTAINPOS, 0);
    write_reg(REG_SUSTAINNEG, 0);
    write_reg(REG_BREAK, 0);
    write_reg(REG_AUDIOMAX, 0x64);
    write_reg(REG_FEEDBACK, read_reg(REG_FEEDBACK) | 0x80);  // LRA motor
    write_reg(REG_LIBRARY, LIBRARY_LRA);
    write_reg(REG_MODE, MODE_STANDBY);
    s_standby = true;
    s_ready = true;
    return true;
}

void haptics_play(uint8_t effect)
{
    if (!s_ready) {
        return;
    }
    if (s_standby) {
        write_reg(REG_MODE, MODE_ACTIVE);
        s_standby = false;
    }
    write_reg(REG_WAVESEQ1, effect);
    write_reg(REG_WAVESEQ2, 0);
    write_reg(REG_GO, 1);
    s_last_play_us = esp_timer_get_time();
}

void haptics_idle(void)
{
    if (s_ready && !s_standby && esp_timer_get_time() - s_last_play_us > STANDBY_AFTER_US) {
        write_reg(REG_MODE, MODE_STANDBY);
        s_standby = true;
    }
}
