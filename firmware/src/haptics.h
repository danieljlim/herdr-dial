#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Effect ids from the DRV2605 waveform library (datasheet table 11.2; same ids for the LRA library).
#define HAPTIC_SHARP_TICK   26  // 60%: knob detent
#define HAPTIC_CLICK        1   // tap acknowledged
#define HAPTIC_DOUBLE_CLICK 10  // an agent finished
#define HAPTIC_ALERT        15  // 750 ms alert: an agent is blocked

bool haptics_init(void);
void haptics_play(uint8_t effect);

// Call regularly: puts the driver into standby once the last effect has finished.
void haptics_idle(void);

#ifdef __cplusplus
}
#endif
