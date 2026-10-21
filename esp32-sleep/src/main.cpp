// Deep sleep with no wake-up source configured: the chip stays asleep until the board is power-cycled
// or reset, and then goes straight back to sleep.
#include <Arduino.h>

#include "esp_sleep.h"

void setup()
{
    esp_deep_sleep_start();
}

void loop()
{
}
