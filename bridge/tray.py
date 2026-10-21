"""System tray view of the agents, next to the Windows clock.

The icon is a tiny version of the knob's dial: one ring segment per agent in its status colour, with
the centre showing the most urgent status. Left-click opens a desktop copy of the dial
(dial_popup.py); right-click lists every agent, and picking one focuses it just like a tap on the knob.

pystray runs its own message loop on a background thread. Everything that touches herdr stays on
the bridge's main thread: menu clicks only queue a request, which `take_focus_requests` hands back.
"""

import queue
import threading

import pystray
from PIL import Image, ImageDraw

# Same colours as the firmware (firmware/src/ui.cpp).
STATUS_COLOURS = {
    "working": (59, 130, 246),
    "blocked": (239, 68, 68),
    "done": (34, 197, 94),
    "idle": (107, 114, 128),
}
UNKNOWN_COLOUR = (55, 65, 81)
OFFLINE_COLOUR = (75, 85, 99)
STATUS_LABELS = {"working": "working", "blocked": "needs you", "done": "done", "idle": "idle"}
# Most urgent first: this order picks the left-click target and the centre colour.
URGENCY = ["blocked", "done", "working", "idle"]

ICON_SIZE = 64
RING_WIDTH = 12
SEGMENT_GAP_DEG = 8
MENU_NAME_LEN = 50


def _urgency(status):
    return URGENCY.index(status) if status in URGENCY else len(URGENCY)


def _draw_icon(agents, offline):
    image = Image.new("RGBA", (ICON_SIZE, ICON_SIZE), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)
    box = (2, 2, ICON_SIZE - 3, ICON_SIZE - 3)
    if offline or not agents:
        draw.ellipse(box, outline=OFFLINE_COLOUR, width=RING_WIDTH)
        return image

    segment = 360 / len(agents)
    gap = SEGMENT_GAP_DEG if len(agents) > 1 else 0
    for i, agent in enumerate(agents):
        start = -90 + i * segment + gap / 2  # PIL measures from 3 o'clock; start the ring at 12
        draw.arc(box, start, start + segment - gap, fill=STATUS_COLOURS.get(agent["s"], UNKNOWN_COLOUR),
                 width=RING_WIDTH)

    top = min(agents, key=lambda a: _urgency(a["s"]))
    inset = RING_WIDTH + 6
    draw.ellipse((inset, inset, ICON_SIZE - 1 - inset, ICON_SIZE - 1 - inset),
                 fill=STATUS_COLOURS.get(top["s"], UNKNOWN_COLOUR))
    return image


def _summary(agents, error):
    if error:
        return f"agent-view: {error}"
    if not agents:
        return "agent-view: no agents"
    counts = {}
    for agent in agents:
        counts[agent["s"]] = counts.get(agent["s"], 0) + 1
    parts = [f"{counts[s]} {STATUS_LABELS[s]}" for s in URGENCY if counts.get(s)]
    return "agent-view: " + ", ".join(parts)  # Windows truncates tooltips at 127 characters


def _menu_name(agent):
    name = agent["n"] or agent["id"]
    return name if len(name) <= MENU_NAME_LEN else name[:MENU_NAME_LEN - 3] + "..."


class TrayView:
    def __init__(self, on_quit):
        from dial_popup import DialPopup, _enable_dpi_awareness  # imports this module's colours

        # Without this Windows draws our menus at 100% and stretches them, which looks blurry.
        _enable_dpi_awareness()
        self._agents = []
        self._error = None
        self._last = None
        self._requests = queue.Queue()
        self._on_quit = on_quit
        self._popup = DialPopup(on_focus=self._requests.put)
        self._icon = pystray.Icon("agent-view", _draw_icon([], True), "agent-view: starting",
                                  menu=pystray.Menu(self._menu_items))
        threading.Thread(target=self._icon.run, daemon=True).start()

    def update(self, snapshot):
        if snapshot == self._last:
            return
        self._last = snapshot
        self._agents = snapshot.get("a", [])
        self._error = snapshot.get("e")
        self._icon.icon = _draw_icon(self._agents, bool(self._error))
        self._icon.title = _summary(self._agents, self._error)
        self._icon.update_menu()
        self._popup.update(self._agents, self._error)

    def take_focus_requests(self):
        ids = []
        while True:
            try:
                ids.append(self._requests.get_nowait())
            except queue.Empty:
                return ids

    def stop(self):
        self._icon.stop()

    def _focus_action(self, agent_id):
        return lambda icon, item: self._requests.put(agent_id)

    def _menu_items(self):
        agents = self._agents
        # The default item is what a left-click on the icon does.
        yield pystray.MenuItem("Open dial", lambda icon, item: self._popup.toggle(), default=True)
        yield pystray.Menu.SEPARATOR
        if self._error or not agents:
            yield pystray.MenuItem(self._error or "No agents", None, enabled=False)
        else:
            for agent in sorted(agents, key=lambda a: _urgency(a["s"])):
                label = STATUS_LABELS.get(agent["s"], "unknown")
                marker = "  (on screen)" if agent.get("f") else ""
                yield pystray.MenuItem(f"{label.upper()}  {_menu_name(agent)}{marker}",
                                       self._focus_action(agent["id"]))
        yield pystray.Menu.SEPARATOR
        yield pystray.MenuItem("Quit agent-view bridge", lambda icon, item: self._on_quit())
