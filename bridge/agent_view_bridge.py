"""Relay herdr agent state to the agent-view knob over USB serial or Bluetooth LE.

herdr only listens on a local socket (a named pipe on Windows), so this bridge
runs on the same machine, polls `agent.list`, and streams compact snapshots to
the device as newline-delimited JSON. The device sends back intents such as
`{"t":"focus","id":"w1:p2"}`, which the bridge turns into herdr API calls.

The device can only ask for the actions handled in `handle_device_message`;
nothing it sends is ever forwarded to an agent as text.
"""

import argparse
import asyncio
import csv
import datetime
import json
import os
import queue
import socket
import sys
import threading
import time
import unicodedata

import serial
from serial.tools import list_ports

from window_focus import raise_herdr_terminal

ESPRESSIF_USB_VID = 0x303A
MAX_AGENTS = 16          # must match MAX_AGENTS in firmware/src/ui.h
MAX_NAME_LEN = 48
HEARTBEAT_SECS = 5.0     # device shows "offline" after ~15s of silence
RETRY_SECS = 2.0
LOOP_SLEEP_SECS = 0.03

# Must match firmware/src/link.cpp.
BLE_SERVICE_UUID = "8b5b0001-6a3e-4c55-9d1e-6f1c2a7d3e90"
BLE_RX_UUID = "8b5b0002-6a3e-4c55-9d1e-6f1c2a7d3e90"  # bridge -> device, write
BLE_TX_UUID = "8b5b0003-6a3e-4c55-9d1e-6f1c2a7d3e90"  # device -> bridge, notify
BLE_CONNECT_TIMEOUT_SECS = 25

PING = {"t": "ping"}


def log(msg):
    print(time.strftime("%H:%M:%S"), msg, flush=True)


def default_socket_path():
    if os.environ.get("HERDR_SOCKET_PATH"):
        return os.environ["HERDR_SOCKET_PATH"]
    if os.name == "nt":
        base = os.path.join(os.environ["APPDATA"], "herdr")
    else:
        base = os.path.join(os.path.expanduser("~"), ".config", "herdr")
    if os.environ.get("HERDR_SESSION"):
        return os.path.join(base, "sessions", os.environ["HERDR_SESSION"], "herdr.sock")
    return os.path.join(base, "herdr.sock")


class HerdrError(Exception):
    pass


class Herdr:
    """Minimal client for herdr's newline-delimited JSON socket API."""

    def __init__(self, path):
        self.path = path
        self._conn = None
        self._buf = b""
        self._seq = 0

    def _connect(self):
        if os.name == "nt":
            pipe = self.path if self.path.startswith("\\\\.\\pipe\\") else "\\\\.\\pipe\\" + self.path
            self._conn = open(pipe, "r+b", buffering=0)
        else:
            sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            sock.connect(self.path)
            self._conn = sock.makefile("rwb", buffering=0)
        self._buf = b""

    def close(self):
        if self._conn is not None:
            try:
                self._conn.close()
            except OSError:
                pass
        self._conn = None

    def _readline(self):
        while b"\n" not in self._buf:
            chunk = self._conn.read(65536)
            if not chunk:
                raise OSError("herdr closed the connection")
            self._buf += chunk
        line, self._buf = self._buf.split(b"\n", 1)
        return line

    def _roundtrip(self, method, params):
        if self._conn is None:
            self._connect()
        self._seq += 1
        req_id = f"agent-view-{self._seq}"
        self._conn.write(json.dumps({"id": req_id, "method": method, "params": params}).encode() + b"\n")
        while True:
            msg = json.loads(self._readline())
            if msg.get("id") == req_id:
                return msg

    def request(self, method, params=None):
        params = params or {}
        try:
            msg = self._roundtrip(method, params)
        except (OSError, ValueError):
            # Stale connection (herdr restarted, or it closed an idle pipe): retry once fresh.
            self.close()
            try:
                msg = self._roundtrip(method, params)
            except (OSError, ValueError) as e:
                self.close()
                raise HerdrError(f"cannot reach herdr at {self.path}: {e}") from e
        if "error" in msg:
            err = msg["error"]
            raise HerdrError(f"{method}: {err.get('code')}: {err.get('message')}")
        return msg["result"]


def ascii_safe(text, limit):
    # The device font only covers printable ASCII.
    text = unicodedata.normalize("NFKD", text or "").encode("ascii", "ignore").decode()
    text = " ".join(text.split())
    return text if len(text) <= limit else text[: limit - 3] + "..."


def agent_label(agent):
    title = agent.get("terminal_title_stripped") or ""
    if title.strip():
        return title
    cwd = (agent.get("cwd") or "").rstrip("\\/")
    return os.path.basename(cwd) or agent.get("agent") or agent.get("pane_id", "?")


def build_snapshot(herdr):
    try:
        agents = herdr.request("agent.list")["agents"]
    except HerdrError as e:
        return {"t": "agents", "a": [], "e": "herdr not reachable"}, str(e)
    snapshot = {
        "t": "agents",
        "a": [
            {
                "id": a["pane_id"],
                "n": ascii_safe(agent_label(a), MAX_NAME_LEN),
                "k": ascii_safe(a.get("agent") or "", 15),
                "s": a.get("agent_status") or "unknown",
                "f": 1 if a.get("focused") else 0,
            }
            for a in agents[:MAX_AGENTS]
        ],
    }
    return snapshot, None


BATTERY_CSV_FIELDS = ["time", "firmware", "link", "mv", "percent", "known", "ext", "low", "screen", "uptime_s", "cpu_mhz"]


class BatteryWatch:
    """Logs battery reports (to the console and a CSV) and raises a herdr notification when the knob goes low."""

    def __init__(self, csv_path):
        self.low = False
        self.percent = None
        self.firmware = None
        self.csv_path = csv_path

    def _append_csv(self, msg, link_name):
        new_file = not os.path.exists(self.csv_path)
        os.makedirs(os.path.dirname(self.csv_path) or ".", exist_ok=True)
        with open(self.csv_path, "a", newline="") as f:
            writer = csv.writer(f)
            if new_file:
                writer.writerow(BATTERY_CSV_FIELDS)
            writer.writerow([
                datetime.datetime.now().isoformat(timespec="seconds"), self.firmware or "", link_name,
                msg.get("mv"), msg.get("p"), msg.get("known", ""), msg.get("ext"), msg.get("low"),
                msg.get("scr", ""), msg.get("up", ""), msg.get("cpu", ""),
            ])

    def update(self, msg, herdr, link_name):
        self.firmware = msg.get("v") or self.firmware
        if self.csv_path:
            self._append_csv(msg, link_name)
        percent, low, ext = msg.get("p"), bool(msg.get("low")), bool(msg.get("ext"))
        if not msg.get("known", 1):
            percent = None
        if percent != self.percent:
            if percent is None:
                log(f"knob on external power, battery level unknown ({msg.get('mv')} mV)")
            else:
                log(f"knob battery {percent}% ({msg.get('mv')} mV{', external power' if ext else ''})")
            self.percent = percent
        if low and not self.low:
            try:
                herdr.request("notification.show", {
                    "title": "agent-view battery low",
                    "body": f"{percent}% left. Plug in the knob.",
                    "sound": "request",
                })
            except HerdrError as e:
                log(f"battery notification failed: {e}")
        self.low = low


def handle_device_message(msg, herdr, raise_window, battery, link_name):
    """Returns True when herdr state probably changed and should be re-polled."""
    kind = msg.get("t")
    if kind == "hello":
        battery.firmware = msg.get("v")
        log(f"device connected over {link_name} (firmware {msg.get('v', '?')})")
        return True
    if kind == "focus" and isinstance(msg.get("id"), str):
        try:
            herdr.request("agent.focus", {"target": msg["id"]})
            log(f"focused {msg['id']}")
        except HerdrError as e:
            log(f"focus failed: {e}")
            return True
        if raise_window and not raise_herdr_terminal():
            log("could not bring the herdr terminal window to the front")
        return True
    if kind == "bat":
        battery.update(msg, herdr, link_name)
        return False
    log(f"ignoring device message: {msg}")
    return False


class LineParser:
    """Splits a byte stream into JSON messages, skipping non-JSON lines (boot logs on the USB console)."""

    def __init__(self):
        self._buf = b""

    def reset(self):
        self._buf = b""

    def feed(self, data):
        self._buf += data
        messages = []
        while b"\n" in self._buf:
            line, self._buf = self._buf.split(b"\n", 1)
            line = line.strip()
            if not line.startswith(b"{"):
                continue
            try:
                messages.append(json.loads(line))
            except ValueError:
                pass
        return messages


def encode(obj):
    return json.dumps(obj, separators=(",", ":")).encode() + b"\n"


class SerialLink:
    """USB serial link to the knob. Reconnects whenever it is unplugged or reset."""

    name = "usb"

    def __init__(self, port):
        self.port_arg = port
        self.ser = None
        self.last_sent = None
        self.last_send_time = 0.0
        self._parser = LineParser()
        self._next_attempt = 0.0

    @property
    def connected(self):
        return self.ser is not None

    def ensure_open(self):
        """Returns True when the link has just (re)connected."""
        if self.ser is not None or time.monotonic() < self._next_attempt:
            return False
        self._next_attempt = time.monotonic() + RETRY_SECS
        port = self.port_arg or next((p.device for p in list_ports.comports() if p.vid == ESPRESSIF_USB_VID), None)
        if not port:
            return False
        try:
            self.ser = serial.Serial(port, 115200, timeout=0, write_timeout=1)
        except serial.SerialException as e:
            log(f"cannot open {port}: {e}")
            return False
        self._parser.reset()
        log(f"usb: opened {port}")
        return True

    def _drop(self, err):
        log(f"usb: link lost: {err}")
        try:
            self.ser.close()
        except serial.SerialException:
            pass
        self.ser = None

    def send(self, obj):
        if self.ser is None:
            return
        try:
            self.ser.write(encode(obj))
        except (serial.SerialException, OSError) as e:
            self._drop(e)

    def read_messages(self):
        if self.ser is None:
            return []
        try:
            data = self.ser.read(self.ser.in_waiting or 1)
        except (serial.SerialException, OSError) as e:
            self._drop(e)
            return []
        return self._parser.feed(data)


class BleLink:
    """Bluetooth LE link to the knob. bleak is async, so it runs on its own thread and event loop."""

    name = "ble"

    def __init__(self, address):
        self.address = address
        self.connected = False
        self.last_sent = None
        self.last_send_time = 0.0
        self._parser = LineParser()
        self._incoming = queue.Queue()
        self._loop = None
        self._outgoing = None
        self._just_connected = threading.Event()
        threading.Thread(target=lambda: asyncio.run(self._main()), daemon=True).start()

    def ensure_open(self):
        """Returns True when the link has just (re)connected."""
        if self._just_connected.is_set():
            self._just_connected.clear()
            return True
        return False

    def send(self, obj):
        if self.connected and self._loop is not None:
            self._loop.call_soon_threadsafe(self._outgoing.put_nowait, encode(obj))

    def read_messages(self):
        messages = []
        while True:
            try:
                messages += self._parser.feed(self._incoming.get_nowait())
            except queue.Empty:
                return messages

    async def _main(self):
        self._loop = asyncio.get_running_loop()
        self._outgoing = asyncio.Queue()
        while True:
            try:
                await self._session()
            except Exception as e:  # bleak raises a variety of OS-specific errors
                log(f"ble: {type(e).__name__}: {e}")
            self.connected = False
            await asyncio.sleep(RETRY_SECS)

    async def _session(self):
        from bleak import BleakClient, BleakScanner

        if self.address:
            device = await BleakScanner.find_device_by_address(self.address, timeout=10)
        else:
            device = await BleakScanner.find_device_by_filter(
                lambda d, adv: BLE_SERVICE_UUID in [u.lower() for u in adv.service_uuids], timeout=10)
        if device is None:
            return

        disconnected = asyncio.Event()
        # Windows takes 3-14s to connect and discover services (measured), past bleak's 10s default.
        # The knob's services never change, so Windows' service cache is safe to use and a little faster.
        async with BleakClient(device, timeout=BLE_CONNECT_TIMEOUT_SECS, winrt={"use_cached_services": True},
                               disconnected_callback=lambda _: disconnected.set()) as client:
            await client.start_notify(BLE_TX_UUID, lambda _, data: self._incoming.put(bytes(data)))
            chunk = max(20, min(client.mtu_size - 3, 244))
            while not self._outgoing.empty():
                self._outgoing.get_nowait()  # anything queued was meant for a previous connection
            self._parser.reset()
            self.connected = True
            self._just_connected.set()
            log(f"ble: connected to {device.address} (mtu {client.mtu_size})")
            while not disconnected.is_set():
                try:
                    data = await asyncio.wait_for(self._outgoing.get(), timeout=1)
                except asyncio.TimeoutError:
                    continue
                for i in range(0, len(data), chunk):
                    await client.write_gatt_char(BLE_RX_UUID, data[i:i + chunk], response=True)
        self.connected = False
        log("ble: disconnected")


def run_dry(herdr, interval):
    last = None
    while True:
        snapshot, err = build_snapshot(herdr)
        if snapshot != last:
            print(json.dumps(snapshot, separators=(",", ":")), flush=True)
            if err:
                log(err)
            last = snapshot
        time.sleep(interval)


def run(herdr, links, interval, raise_window, battery, tray, quit_event):
    next_poll = 0.0
    last_err = None
    snapshot = None
    last_primary = None
    while not quit_event.is_set():
        fresh = {link for link in links if link.ensure_open()}
        force = bool(fresh)
        for link in links:
            for msg in link.read_messages():
                force |= handle_device_message(msg, herdr, raise_window, battery, link.name)
        if tray:
            for agent_id in tray.take_focus_requests():
                force |= handle_device_message({"t": "focus", "id": agent_id}, herdr, raise_window, battery, "tray")

        now = time.monotonic()
        if force or now >= next_poll:
            next_poll = now + interval
            snapshot, err = build_snapshot(herdr)
            if err != last_err:
                log(err or "herdr reachable")
                last_err = err
            if tray:
                tray.update(snapshot)

        # Only talk on one link: USB when it's up (links are ordered by preference), Bluetooth otherwise.
        # The other link stays connected so it can take over immediately.
        primary = next((link for link in links if link.connected), None)
        if primary is not None and primary is not last_primary:
            fresh.add(primary)
            if last_primary is not None:
                log(f"now using {primary.name}")
        last_primary = primary

        for link in links:
            if not link.connected or snapshot is None:
                continue
            if link is primary and (force or link in fresh or snapshot != link.last_sent):
                link.send(snapshot)
                link.last_sent = snapshot
                link.last_send_time = now
            elif now - link.last_send_time >= HEARTBEAT_SECS:
                # Standby links get pings too: the knob drops a Bluetooth connection that goes silent,
                # which clears connections Windows keeps open after we've given up on them.
                link.send(PING)
                link.last_send_time = now

        time.sleep(LOOP_SLEEP_SECS)


LOG_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "logs")


def redirect_output_if_windowless():
    """pythonw (used when starting with Windows) has no console, so print() would fail: log to a file."""
    if sys.stdout is not None:
        return
    os.makedirs(LOG_DIR, exist_ok=True)
    log_file = open(os.path.join(LOG_DIR, "bridge.log"), "a", buffering=1, encoding="utf-8")
    sys.stdout = sys.stderr = log_file


def claim_single_instance():
    """Returns False if another bridge is already running; two would fight over the knob and the tray."""
    if os.name != "nt":
        return True
    import ctypes
    ERROR_ALREADY_EXISTS = 183
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.CreateMutexW.restype = ctypes.c_void_p
    # Kept for the life of the process; Windows releases it when we exit.
    claim_single_instance.handle = kernel32.CreateMutexW(None, False, "Local\\agent-view-bridge")
    return ctypes.get_last_error() != ERROR_ALREADY_EXISTS


def main():
    redirect_output_if_windowless()
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--transport", choices=["auto", "usb", "ble"], default="auto",
                        help="auto (default) uses USB and Bluetooth LE at the same time")
    parser.add_argument("--port", help="serial port (default: first Espressif USB device)")
    parser.add_argument("--ble-address", help="only connect to this knob (default: first one advertising)")
    parser.add_argument("--socket", default=default_socket_path(), help="herdr socket / pipe path")
    parser.add_argument("--interval", type=float, default=0.5, help="herdr poll interval in seconds")
    parser.add_argument("--dry-run", action="store_true", help="print snapshots instead of talking to a device")
    parser.add_argument("--no-raise", action="store_true", help="don't bring the herdr terminal window to the front on focus")
    parser.add_argument("--no-tray", action="store_true", help="don't show the agents in the system tray")
    parser.add_argument("--battery-log", default=os.path.join(LOG_DIR, "battery.csv"),
                        help="CSV file that battery reports are appended to ('' to disable)")
    args = parser.parse_args()

    herdr = Herdr(args.socket)
    try:
        if args.dry_run:
            run_dry(herdr, args.interval)
            return 0
        if not claim_single_instance():
            log("another agent-view bridge is already running; exiting")
            return 0
        links = []
        if args.transport in ("auto", "usb"):
            links.append(SerialLink(args.port))
        if args.transport in ("auto", "ble"):
            links.append(BleLink(args.ble_address))
        battery_log = os.path.normpath(args.battery_log) if args.battery_log else None
        log(f"herdr socket: {args.socket}; transports: {', '.join(link.name for link in links)}; battery log: {battery_log}")
        quit_event = threading.Event()
        tray = None
        if not args.no_tray:
            from tray import TrayView  # optional dependency: only needed when the tray is on
            tray = TrayView(on_quit=quit_event.set)
        try:
            run(herdr, links, args.interval, not args.no_raise, BatteryWatch(battery_log), tray, quit_event)
        finally:
            if tray:
                tray.stop()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
