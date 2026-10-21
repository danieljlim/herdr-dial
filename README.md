# SpeedDial

SpeedDial is a desk dial for your coding agents. It shows every [herdr](https://herdr.dev) agent at a glance, buzzes when one needs you, and jumps you straight to it.

It runs on the [Waveshare ESP32-S3-Knob-Touch-LCD-1.8](https://www.waveshare.com/esp32-s3-knob-touch-lcd-1.8.htm), a round touchscreen with a knob, a haptic motor and a battery. It connects to your PC over USB or Bluetooth.

<!-- demo video / gif here -->

## What it does

- **The rim shows your agents.** There's one segment per agent, coloured by status:

  | Colour | Status |
  |---|---|
  | Red | needs you (blocked) |
  | Green | done, and you haven't looked yet |
  | Blue | working |
  | Grey | idle |

- **Turn the knob to scroll through agents.** The centre shows the selected agent's status and what it's working on (its terminal title).
- **Tap the screen to go to that agent.** This focuses its pane in herdr and brings the terminal window to the front.
- **The haptic motor tells you what changed.**
  - A long buzz means an agent got blocked.
  - A double click means one finished.
  - If you haven't touched the knob for 8 seconds, the dial also jumps to a newly blocked agent.
- **Wireless.** Unplug it and it carries on over Bluetooth LE. The status bar shows the link and the battery level.
- **Saves battery.** On battery, the screen dims after 30 seconds and turns off after 3 minutes. Turning the knob, tapping the screen, or an agent needing you all wake it.
- **The same dial lives in the system tray.**
  - **Tray icon:** a tiny ring of status colours.
  - **Left-click:** opens a desktop copy of the dial. Scroll to turn it, hover over a segment to select that agent, click to go to it.
  - **Right-click:** lists every agent.

The device only ever asks the bridge to *focus* a pane. It can't type into agents or send them keys.

## How it works

```
herdr ──named pipe──> bridge (Python, Windows) ──USB serial / Bluetooth LE, JSON lines──> knob
      <── agent.focus ──                       <── {"t":"focus","id":...}, {"t":"bat",...} ──
```

herdr only exposes a local socket, so the bridge runs on the same PC as herdr.

- **Status updates:** it polls `agent.list` every half second and sends a compact snapshot to the knob.
- **Taps:** when you tap the knob, the bridge calls `agent.focus` and raises the terminal window.

The bridge talks to the knob over USB and Bluetooth at the same time, and prefers USB when both are up. It reconnects to herdr and to the knob on its own.

## What you need

- **The knob:** a Waveshare ESP32-S3-Knob-Touch-LCD-1.8. A LiPo battery is optional.
- **herdr:** running natively on Windows.
- **Python 3.11 or newer.**
- **Bluetooth (optional):** a Bluetooth LE adapter.

## Setup (Windows)

```powershell
git clone https://github.com/danieljlim/herdr-dial
cd herdr-dial
python -m venv .venv
.venv\Scripts\pip install -r bridge\requirements.txt platformio
```

### 1. Flash the firmware

The board's USB-C port connects to one of two chips, depending on which way round the plug is. Plug it in so that an Espressif device (VID `303A`) shows up in `.venv\Scripts\pio device list`, then run:

```powershell
.venv\Scripts\pio run -d firmware -t upload
```

- **Upload doesn't start:** hold BOOT while you plug the board in.
- **Knob moves the wrong way:** if turning it clockwise moves the selection backwards, flip `KNOB_DIRECTION` in `firmware/src/main.cpp`.

### 2. Run the bridge

```powershell
.venv\Scripts\python bridge\speeddial_bridge.py            # finds the knob over USB and Bluetooth
.venv\Scripts\python bridge\speeddial_bridge.py --dry-run  # just print what would be sent
```

| Option | What it does |
|---|---|
| `--transport auto\|usb\|ble` | Which links to use. The default, `auto`, uses both. |
| `--port COM6` | Serial port. Default: the first Espressif USB device. |
| `--ble-address <addr>` | Only connect to this knob. Default: the first one advertising. |
| `--socket <path>` | herdr socket. Default: herdr's own default; honours `HERDR_SOCKET_PATH` / `HERDR_SESSION`. |
| `--interval 0.5` | Poll interval, in seconds. |
| `--no-raise` | Focus the pane, but don't bring the terminal window to the front. |
| `--no-tray` | No system tray icon. |
| `--battery-log <csv>` | Where battery reports go. Default: `logs/battery.csv`; `''` turns it off. |

### 3. Start it with Windows (optional)

```powershell
powershell -ExecutionPolicy Bypass -File bridge\install_startup.ps1
```

- **What it does:** adds a shortcut to your Startup folder and starts the bridge now.
- **No console window:** output goes to `logs\bridge.log`.
- **One copy at a time:** only one bridge runs, so running the script again is harmless.
- **Quit it:** use the tray icon.
- **Remove it:** run the same script with `-Uninstall`.

### Tip: show what each agent is doing in herdr too

The dial shows each agent's terminal title, which is usually a description of the task. To show the same thing in herdr's sidebar, add this to herdr's `config.toml`:

```toml
[ui.sidebar.agents]
rows = [["state_icon", "terminal_title_stripped"], ["agent", "tab"]]
```

Then run `herdr server reload-config`.

## Bluetooth security

Bluetooth is unpaired. Anything nearby could connect to the knob and show it fake data, or pretend to be the knob and focus herdr panes.

To pin the bridge to your knob, pass `--ble-address`. The bridge prints the address when it connects.

## Battery

- **No fuel gauge:** the board doesn't have one, so the percentage is estimated from the system rail voltage.
- **On USB power:** the charger holds that rail above any cell voltage, so the knob shows a charging bolt and the percentage only appears once it's running on battery.
- **Low battery:** below 15% the indicator turns red, the motor buzzes once, and herdr shows an "SpeedDial battery low" notification.

Power saving in the default build:

- CPU at 80 MHz.
- The screen turns off after 3 minutes idle on battery. On any power, it turns off a minute after the bridge goes away (for example, when the PC sleeps).
- Slower Bluetooth connection timing once the link has settled, with 5-second heartbeats.
- The haptic driver goes into standby between effects.

The `knob-baseline` build turns all of that off, for comparison.

### Measuring battery life

The bridge appends every battery report (one every 30 seconds) to `logs/battery.csv`. To measure:

1. Charge the knob fully.
2. Unplug it and use it normally.
3. Run:

   ```powershell
   .venv\Scripts\python bridge\battery_report.py
   ```

It prints each on-battery run with its drain rate and projected runtime. Runs of an hour or more give useful numbers.

### The second chip

The board has a second ESP32, which drives the audio DAC and a second encoder. SpeedDial doesn't use it.

`esp32-sleep/` holds firmware that keeps that chip in deep sleep, which saves a little battery. To flash it, plug in the USB-C cable the other way round, so that a CH340 serial port appears:

```powershell
.venv\Scripts\pio run -d esp32-sleep -t upload --upload-port COM5
```

To put the factory image back, use Waveshare's demo package: it's at `Firmware/ESP32-KNOB_ESP32_0.bin`.

## Layout

```
bridge/
  speeddial_bridge.py   herdr <-> knob relay: polling, links, heartbeats, battery log
  window_focus.py        brings the terminal running herdr to the front
  tray.py                system tray icon and menu
  dial_popup.py          the desktop copy of the dial
  battery_report.py      drain rate and runtime from logs/battery.csv
  install_startup.ps1    start the bridge at sign-in
firmware/                ESP32-S3 firmware (PlatformIO, Arduino 3 / ESP-IDF 5, LVGL 8)
  src/main.cpp           protocol, knob, screen dimming, main loop
  src/ui.cpp             the dial UI
  src/link.cpp           USB serial and Bluetooth LE transport
  src/battery.cpp        battery voltage to percentage
  src/display.c, i2c_bus.c, haptics.c   board bring-up: QSPI panel, CST816 touch, DRV2605 haptics
  src/vendor/            Espressif/Waveshare drivers and the panel init table
esp32-sleep/             keeps the unused second ESP32 asleep
```

## License

MIT. See [LICENSE](LICENSE).

The files in `firmware/src/vendor/` keep their own licences:

- **Espressif drivers:** Apache-2.0.
- **Panel init table:** from Waveshare's demo code.
