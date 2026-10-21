#pragma once

// Newline-delimited JSON over USB serial and Bluetooth LE at the same time.
// Replies go out on the active link: USB if the bridge has used it recently, else Bluetooth.

enum Link {
    LINK_USB,
    LINK_BLE,
};

void link_init(void);

// Calls on_line once per complete line received on either link.
void link_poll(void (*on_line)(const char *line));

// Sends one line (without the trailing newline). `broadcast` sends on every link instead of the reply link.
void link_send(const char *line, bool broadcast);

Link link_active(void);
bool link_ble_connected(void);
