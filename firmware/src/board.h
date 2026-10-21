// Pin map for the Waveshare ESP32-S3-Knob-Touch-LCD-1.8, taken from Waveshare's demo sources.
#pragma once

// Build the `knob-baseline` environment (POWER_SAVING=0) to measure battery life without the
// power optimisations, for comparison with the default build.
#ifndef POWER_SAVING
#define POWER_SAVING 1
#endif

#define LCD_H_RES 360
#define LCD_V_RES 360

#define PIN_LCD_CS    14
#define PIN_LCD_PCLK  13
#define PIN_LCD_DATA0 15
#define PIN_LCD_DATA1 16
#define PIN_LCD_DATA2 17
#define PIN_LCD_DATA3 18
#define PIN_LCD_RST   21
#define PIN_LCD_BL    47

// Shared by the CST816 touch controller (0x15) and the DRV2605 haptic driver (0x5A).
#define PIN_I2C_SDA 11
#define PIN_I2C_SCL 12

// Battery voltage through a 1:2 divider (ADC1 channel 0).
#define PIN_BATTERY_ADC 1

// The knob is a bidirectional switch, not a quadrature encoder: A pulses one way, B the other.
#define PIN_KNOB_A 8
#define PIN_KNOB_B 7
