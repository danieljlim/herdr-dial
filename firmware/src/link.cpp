#include "link.h"

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>

#include "board.h"
#include "driver/usb_serial_jtag.h"
#include "freertos/stream_buffer.h"

// Must match bridge/speeddial_bridge.py.
#define BLE_DEVICE_NAME "SpeedDial"
#define BLE_SERVICE_UUID "8b5b0001-6a3e-4c55-9d1e-6f1c2a7d3e90"
#define BLE_RX_UUID      "8b5b0002-6a3e-4c55-9d1e-6f1c2a7d3e90"  // bridge -> device, write
#define BLE_TX_UUID      "8b5b0003-6a3e-4c55-9d1e-6f1c2a7d3e90"  // device -> bridge, notify
#define BLE_MTU 247
#define BLE_NOTIFY_CHUNK 20  // device -> bridge messages are tiny; stay within the minimum MTU
#define LINK_RECENT_MS 12000  // the bridge heartbeats every 5s on every link it is connected on

// Windows can keep a Bluetooth link open after the bridge has given up on it, and while we think we're
// connected we don't advertise, so the bridge can never find us again. The bridge pings every connected
// link, so a silent connection is a dead one: drop it and go back to advertising.
#define BLE_SILENT_DISCONNECT_MS 20000
#define BLE_ADVERTISING_CHECK_MS 5000

// Power saving: a slower connection (units of 1.25ms). Worst-case delay for an update is
// (latency + 1) * max interval, about 0.45s. Advertising stays at the default rate: it only runs while
// disconnected, and slowing it made Windows take up to 20s to find the knob for almost no saving.
#define BLE_CONN_MIN_INTERVAL 60    // 75ms
#define BLE_CONN_MAX_INTERVAL 120   // 150ms
#define BLE_CONN_LATENCY 2          // events the knob may skip when it has nothing to send
#define BLE_CONN_TIMEOUT 400        // 4s, in units of 10ms
// Asking for slower timing while Windows is still discovering services can make its connect time out,
// so wait until the connection has settled.
#define BLE_CONN_PARAMS_DELAY_MS 5000

struct LineBuffer {
    char data[6144];
    size_t len;
    bool overflow;
};

static LineBuffer s_usb_line;
static LineBuffer s_ble_line;
static StreamBufferHandle_t s_ble_rx;  // filled by the BLE host task, drained by link_poll()
static BLECharacteristic *s_tx;
static BLEServer *s_server;
static volatile bool s_ble_connected;
static volatile uint16_t s_ble_conn_handle;
static volatile uint32_t s_ble_connected_ms;
static volatile bool s_ble_params_requested;
static uint32_t s_last_rx_ms[2];  // indexed by Link; 0 = never
static uint32_t s_last_advertising_check_ms;

class ServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer *) override { s_ble_connected = true; }
    void onConnect(BLEServer *, ble_gap_conn_desc *desc) override
    {
        s_ble_conn_handle = desc->conn_handle;
        s_ble_connected_ms = millis();
        s_ble_params_requested = false;
    }
    void onDisconnect(BLEServer *) override
    {
        s_ble_connected = false;
        BLEDevice::startAdvertising();
    }
};

class RxCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *characteristic) override
    {
        const String value = characteristic->getValue();
        xStreamBufferSend(s_ble_rx, value.c_str(), value.length(), 0);
    }
};

void link_init(void)
{
    Serial.setRxBufferSize(8192);
    Serial.begin(115200);

    s_ble_rx = xStreamBufferCreate(8192, 1);

    BLEDevice::init(BLE_DEVICE_NAME);
    BLEDevice::setMTU(BLE_MTU);
    BLEServer *server = BLEDevice::createServer();
    server->setCallbacks(new ServerCallbacks());
    s_server = server;

    BLEService *service = server->createService(BLE_SERVICE_UUID);
    BLECharacteristic *rx = service->createCharacteristic(
        BLE_RX_UUID, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
    rx->setCallbacks(new RxCallbacks());
    s_tx = service->createCharacteristic(BLE_TX_UUID, BLECharacteristic::PROPERTY_NOTIFY);
    service->start();

    BLEAdvertising *advertising = BLEDevice::getAdvertising();
    advertising->addServiceUUID(BLE_SERVICE_UUID);
    advertising->setScanResponse(true);  // the name doesn't fit next to a 128-bit UUID
    BLEDevice::startAdvertising();
}

static void feed(LineBuffer &buf, char c, Link link, void (*on_line)(const char *))
{
    if (c == '\n') {
        if (!buf.overflow) {
            buf.data[buf.len] = '\0';
            s_last_rx_ms[link] = millis() | 1;
            on_line(buf.data);
        }
        buf.len = 0;
        buf.overflow = false;
    } else if (buf.len < sizeof(buf.data) - 1) {
        buf.data[buf.len++] = c;
    } else {
        buf.overflow = true;  // drop the whole line rather than parse half of it
    }
}

static void maintain_ble(void)
{
    const uint32_t now = millis();
    if (s_ble_connected) {
#if POWER_SAVING
        if (!s_ble_params_requested && now - s_ble_connected_ms > BLE_CONN_PARAMS_DELAY_MS) {
            // Windows picks a fast connection interval by default; ask for a slower one.
            s_ble_params_requested = true;
            s_server->requestConnParams(s_ble_conn_handle, BLE_CONN_MIN_INTERVAL, BLE_CONN_MAX_INTERVAL,
                                        BLE_CONN_LATENCY, BLE_CONN_TIMEOUT);
        }
#endif
        const uint32_t last_heard = max(s_last_rx_ms[LINK_BLE], (uint32_t)s_ble_connected_ms);
        if (now - last_heard > BLE_SILENT_DISCONNECT_MS) {
            s_ble_connected_ms = now;  // don't retry every loop while the disconnect goes through
            s_server->disconnect(s_ble_conn_handle);
        }
    } else if (now - s_last_advertising_check_ms > BLE_ADVERTISING_CHECK_MS) {
        s_last_advertising_check_ms = now;
        if (!BLEDevice::getAdvertising()->isAdvertising()) {
            BLEDevice::startAdvertising();
        }
    }
}

void link_poll(void (*on_line)(const char *line))
{
    maintain_ble();
    while (Serial.available()) {
        feed(s_usb_line, (char)Serial.read(), LINK_USB, on_line);
    }
    uint8_t chunk[256];
    size_t n;
    while ((n = xStreamBufferReceive(s_ble_rx, chunk, sizeof(chunk), 0)) > 0) {
        for (size_t i = 0; i < n; i++) {
            feed(s_ble_line, (char)chunk[i], LINK_BLE, on_line);
        }
    }
}

static void send_ble(const char *line)
{
    if (!s_ble_connected) {
        return;
    }
    String framed = String(line) + "\n";
    for (size_t off = 0; off < framed.length(); off += BLE_NOTIFY_CHUNK) {
        const size_t len = min((size_t)BLE_NOTIFY_CHUNK, framed.length() - off);
        s_tx->setValue((uint8_t *)framed.c_str() + off, len);
        s_tx->notify();
    }
}

static void send_usb(const char *line)
{
    Serial.print(line);
    Serial.print('\n');
}

static bool heard_recently(Link link)
{
    return s_last_rx_ms[link] && millis() - s_last_rx_ms[link] < LINK_RECENT_MS;
}

void link_send(const char *line, bool broadcast)
{
    const bool ble = broadcast || link_active() == LINK_BLE;
    const bool usb = broadcast || !ble;
    if (ble) send_ble(line);
    if (usb) send_usb(line);
}

Link link_active(void)
{
    // The bridge prefers USB, so recent USB traffic wins even while Bluetooth is still connected.
    // Bluetooth recency isn't checked: Windows can bunch up heartbeats, which made the choice flap.
    if (heard_recently(LINK_USB) && usb_serial_jtag_is_connected()) return LINK_USB;
    return s_ble_connected ? LINK_BLE : LINK_USB;
}

bool link_ble_connected(void)
{
    return s_ble_connected;
}
