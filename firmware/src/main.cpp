// SpeedDial: a herdr agent dial. Talks newline-delimited JSON to bridge/speeddial_bridge.py
// over USB serial or Bluetooth LE (see link.h).
//
//   bridge -> device  {"t":"agents","a":[{"id":"w1:p2","n":"title","k":"claude","s":"blocked","f":0}],"e":"..."}
//                     {"t":"ping"}                         heartbeat when nothing changed
//   device -> bridge  {"t":"hello","v":"0.3.0"}
//                     {"t":"focus","id":"w1:p2"}
//                     {"t":"bat","mv":3912,"p":72,"ext":0,"low":0,"known":1,"scr":2,"up":1234,"cpu":80,"v":"0.3.0"}
#include <Arduino.h>
#include <ArduinoJson.h>

#include <atomic>

#include "battery.h"
#include "board.h"
#include "display.h"
#include "haptics.h"
#include "i2c_bus.h"
#include "link.h"
#include "lvgl.h"
#include "ui.h"
#include "vendor/bidi_switch_knob.h"

#if POWER_SAVING
#define FIRMWARE_VERSION "0.3.0"
#else
#define FIRMWARE_VERSION "0.3.0-baseline"
#endif

#define KNOB_DIRECTION 1          // flip to -1 if turning clockwise moves the selection backwards
#define OFFLINE_AFTER_MS 15000    // bridge heartbeats every 5s
#define HELLO_INTERVAL_MS 3000

#define BACKLIGHT_BRIGHT 200
#define BACKLIGHT_DIM 30
#define DIM_AFTER_MS 30000              // on battery only; on external power the screen stays bright
#define SCREEN_OFF_AFTER_MS 180000      // on battery only, POWER_SAVING builds
#define OFFLINE_SCREEN_OFF_MS 60000     // bridge gone (PC asleep): nothing to show, POWER_SAVING builds
#define CPU_MHZ_POWER_SAVING 80         // the lowest frequency Bluetooth allows

#define BATTERY_SAMPLE_MS 5000
#define BATTERY_REPORT_MS 30000

// Ordered so that a lower value is a lower-power state.
enum Screen : uint8_t {
    SCREEN_OFF,
    SCREEN_DIM,
    SCREEN_BRIGHT,
};

static std::atomic<int> s_knob_steps{0};

static bool s_online;
static uint32_t s_last_rx_ms;
static uint32_t s_offline_since_ms;
static uint32_t s_last_hello_ms;

static BatteryState s_battery;
static uint32_t s_last_battery_sample_ms;
static uint32_t s_last_battery_report_ms;

static Screen s_screen = SCREEN_BRIGHT;
static uint32_t s_last_activity_ms;

// Knob callbacks run in the esp_timer task, so they only record steps for loop() to apply.
static void knob_left_cb(void *, void *) { s_knob_steps -= KNOB_DIRECTION; }
static void knob_right_cb(void *, void *) { s_knob_steps += KNOB_DIRECTION; }

static void send_json(const JsonDocument &doc, bool broadcast = false)
{
    char line[256];
    serializeJson(doc, line, sizeof(line));
    link_send(line, broadcast);
}

static void send_hello()
{
    JsonDocument doc;
    doc["t"] = "hello";
    doc["v"] = FIRMWARE_VERSION;
    send_json(doc, true);
    s_last_hello_ms = millis();
}

static void send_battery()
{
    JsonDocument doc;
    doc["t"] = "bat";
    doc["mv"] = s_battery.millivolts;
    doc["p"] = s_battery.percent;
    doc["ext"] = s_battery.external_power ? 1 : 0;
    doc["low"] = s_battery.low ? 1 : 0;
    doc["known"] = s_battery.known ? 1 : 0;
    doc["scr"] = (int)s_screen;
    doc["up"] = millis() / 1000;
    doc["cpu"] = getCpuFrequencyMhz();
    doc["v"] = FIRMWARE_VERSION;  // the hello may have gone out before the bridge was listening
    send_json(doc);
    s_last_battery_report_ms = millis();
}

static void set_screen(Screen screen)
{
    if (screen == s_screen) {
        return;
    }
    if (s_screen == SCREEN_OFF) {
        display_set_panel_on(true);
    }
    display_set_brightness(screen == SCREEN_BRIGHT ? BACKLIGHT_BRIGHT : screen == SCREEN_DIM ? BACKLIGHT_DIM : 0);
    if (screen == SCREEN_OFF) {
        display_set_panel_on(false);
    }
    s_screen = screen;
}

// Returns true if the screen wasn't fully bright, in which case the input that woke it should do nothing else.
static bool wake()
{
    s_last_activity_ms = millis();
    if (s_screen == SCREEN_BRIGHT) {
        return false;
    }
    set_screen(SCREEN_BRIGHT);
    return true;
}

static void update_screen(uint32_t now)
{
    const uint32_t idle = now - s_last_activity_ms;
    Screen target = SCREEN_BRIGHT;
    if (POWER_SAVING && !s_online && now - s_offline_since_ms > OFFLINE_SCREEN_OFF_MS && idle > OFFLINE_SCREEN_OFF_MS) {
        target = SCREEN_OFF;
    } else if (s_battery.external_power) {
        target = SCREEN_BRIGHT;
    } else if (POWER_SAVING && idle > SCREEN_OFF_AFTER_MS) {
        target = SCREEN_OFF;
    } else if (idle > DIM_AFTER_MS) {
        target = SCREEN_DIM;
    }
    // Inactivity only ever lowers the screen state; brightening needs input, an alert, or external power.
    if (target < s_screen || s_battery.external_power) {
        set_screen(target);
    }
}

static void update_status_bar()
{
    const char *battery_icon = s_battery.external_power ? LV_SYMBOL_CHARGE
                               : s_battery.percent > 80 ? LV_SYMBOL_BATTERY_FULL
                               : s_battery.percent > 55 ? LV_SYMBOL_BATTERY_3
                               : s_battery.percent > 30 ? LV_SYMBOL_BATTERY_2
                               : s_battery.percent > BATTERY_LOW_PERCENT ? LV_SYMBOL_BATTERY_1
                                                                         : LV_SYMBOL_BATTERY_EMPTY;
    const char *link_icon = !s_online ? "" : link_active() == LINK_BLE ? LV_SYMBOL_BLUETOOTH "  " : LV_SYMBOL_USB "  ";
    char text[48];
    if (s_battery.known) {
        snprintf(text, sizeof(text), "%s%s %d%%", link_icon, battery_icon, s_battery.percent);
    } else {
        snprintf(text, sizeof(text), "%s" LV_SYMBOL_CHARGE, link_icon);
    }
    ui_set_status(text, s_battery.low);
}

static void on_tap(const char *agent_id)
{
    if (wake()) {
        return;
    }
    JsonDocument doc;
    doc["t"] = "focus";
    doc["id"] = agent_id;
    send_json(doc);
    haptics_play(HAPTIC_CLICK);
}

static AgentStatus parse_status(const char *s)
{
    if (strcmp(s, "working") == 0) return STATUS_WORKING;
    if (strcmp(s, "blocked") == 0) return STATUS_BLOCKED;
    if (strcmp(s, "done") == 0) return STATUS_DONE;
    if (strcmp(s, "idle") == 0) return STATUS_IDLE;
    return STATUS_UNKNOWN;
}

static void handle_line(const char *line)
{
    JsonDocument doc;
    if (deserializeJson(doc, line) != DeserializationError::Ok) {
        return;
    }

    // Any message from the bridge, including a ping, proves the link is alive.
    s_last_rx_ms = millis();
    if (!s_online) {
        s_online = true;
        ui_set_offline(false);
        wake();
        send_battery();
    }
    update_status_bar();

    if (strcmp(doc["t"] | "", "agents") != 0) {
        return;
    }

    static Agent agents[MAX_AGENTS];
    int count = 0;
    for (JsonObject a : doc["a"].as<JsonArray>()) {
        if (count == MAX_AGENTS) {
            break;
        }
        Agent &agent = agents[count++];
        strlcpy(agent.id, a["id"] | "", sizeof(agent.id));
        strlcpy(agent.name, a["n"] | "", sizeof(agent.name));
        strlcpy(agent.kind, a["k"] | "", sizeof(agent.kind));
        agent.status = parse_status(a["s"] | "");
        agent.focused = (a["f"] | 0) != 0;
    }

    switch (ui_set_agents(agents, count, doc["e"].as<const char *>())) {
    case ALERT_BLOCKED:
        wake();
        haptics_play(HAPTIC_ALERT);
        break;
    case ALERT_DONE:
        wake();
        haptics_play(HAPTIC_DOUBLE_CLICK);
        break;
    case ALERT_NONE:
        break;
    }
}

static void poll_battery(uint32_t now)
{
    if (s_last_battery_sample_ms && now - s_last_battery_sample_ms < BATTERY_SAMPLE_MS) {
        return;
    }
    s_last_battery_sample_ms = now;
    const bool was_low = s_battery.low;
    const bool changed = battery_update(&s_battery);
    if (changed) {
        update_status_bar();
    }
    if (s_battery.low && !was_low) {
        wake();
        haptics_play(HAPTIC_ALERT);
    }
    if (s_online && ((changed && s_battery.low != was_low) || now - s_last_battery_report_ms >= BATTERY_REPORT_MS)) {
        send_battery();
    }
}

void setup()
{
#if POWER_SAVING
    setCpuFrequencyMhz(CPU_MHZ_POWER_SAVING);
#endif
    link_init();
    i2c_bus_init();
    haptics_init();
    display_init();
    ui_init(on_tap);
    poll_battery(millis());
    display_set_brightness(BACKLIGHT_BRIGHT);
    s_last_activity_ms = millis();
    s_offline_since_ms = millis();

    knob_config_t knob_cfg;
    knob_cfg.gpio_encoder_a = PIN_KNOB_A;
    knob_cfg.gpio_encoder_b = PIN_KNOB_B;
    knob_handle_t knob = iot_knob_create(&knob_cfg);
    iot_knob_register_cb(knob, KNOB_LEFT, knob_left_cb, NULL);
    iot_knob_register_cb(knob, KNOB_RIGHT, knob_right_cb, NULL);

    send_hello();
}

void loop()
{
    link_poll(handle_line);

    const int steps = s_knob_steps.exchange(0);
    if (steps && !wake()) {
        ui_rotate(steps);
        haptics_play(HAPTIC_SHARP_TICK);
    }

    const uint32_t now = millis();
    if (s_online && now - s_last_rx_ms > OFFLINE_AFTER_MS) {
        s_online = false;
        s_offline_since_ms = now;
        ui_set_offline(true);
        update_status_bar();
    }
    if (!s_online && now - s_last_hello_ms > HELLO_INTERVAL_MS) {
        send_hello();
    }

    poll_battery(now);
    update_screen(now);
#if POWER_SAVING
    haptics_idle();
#endif

    lv_timer_handler();
    delay(5);
}
