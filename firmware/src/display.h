#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Brings up the QSPI panel, the CST816 touch controller and LVGL. Call i2c_bus_init() first.
void display_init(void);
void display_set_brightness(uint8_t duty);

// Panel display on/off. The panel keeps its frame memory while off, so turning it back on is instant.
void display_set_panel_on(bool on);

#ifdef __cplusplus
}
#endif
