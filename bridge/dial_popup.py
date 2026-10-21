"""A desktop copy of the knob's dial, shown above the system tray.

Same layout as the firmware UI (firmware/src/ui.cpp): one ring segment per agent, the selected
agent's status, title and position in the middle. The mouse wheel turns it like the knob, hovering a
segment selects it, a click focuses that agent, and Esc or clicking elsewhere closes it.

The dial is drawn with Pillow (Tk's canvas doesn't anti-alias on Windows) and shown in a borderless
Tk window. Tk lives entirely on its own thread: other threads talk to it through `show`, `toggle`
and `update`, which only touch thread-safe state.
"""

import ctypes
import math
import os
import queue
import threading
import time
import tkinter as tk

from PIL import Image, ImageDraw, ImageFont, ImageTk

from tray import STATUS_COLOURS, STATUS_LABELS, UNKNOWN_COLOUR, URGENCY

SIZE = 320              # logical pixels; scaled by the monitor's DPI
RING_MARGIN = 10
RING_WIDTH = 12
RING_WIDTH_SELECTED = 22
SEGMENT_GAP_DEG = 4
SUPERSAMPLE = 3         # draw the ring larger and shrink it, for smooth edges
SCREEN_MARGIN = 12      # gap between the popup and the taskbar / screen edge
POLL_MS = 50
REOPEN_GUARD_SECS = 0.4

BACKGROUND = (12, 12, 16)
TEXT = (237, 235, 230)
MUTED = (156, 163, 175)
DIM = (107, 114, 128)

FONT_DIR = os.path.join(os.environ.get("WINDIR", r"C:\Windows"), "Fonts")


def _font(name, size):
    try:
        return ImageFont.truetype(os.path.join(FONT_DIR, name), size)
    except OSError:
        return ImageFont.load_default(size)


def _wrap(draw, text, font, width, max_lines):
    lines, current = [], ""
    for word in text.split():
        candidate = f"{current} {word}".strip()
        if draw.textlength(candidate, font=font) <= width:
            current = candidate
            continue
        if current:
            lines.append(current)
        current = word
    if current:
        lines.append(current)
    if len(lines) > max_lines:
        lines = lines[:max_lines]
        while lines[-1] and draw.textlength(lines[-1] + "...", font=font) > width:
            lines[-1] = lines[-1][:-1]
        lines[-1] = lines[-1].rstrip() + "..."
    return lines


def _segment_angles(index, count):
    """Start and end angle in PIL's convention (degrees clockwise from 3 o'clock), ring starting at 12."""
    segment = 360 / count
    gap = SEGMENT_GAP_DEG if count > 1 else 0
    start = -90 + index * segment + gap / 2
    return start, start + segment - gap


def render(agents, error, selected, scale):
    size = round(SIZE * scale)
    big = Image.new("RGB", (size * SUPERSAMPLE, size * SUPERSAMPLE), BACKGROUND)
    draw = ImageDraw.Draw(big)
    margin = RING_MARGIN * scale * SUPERSAMPLE
    box = (margin, margin, size * SUPERSAMPLE - margin, size * SUPERSAMPLE - margin)
    if error or not agents:
        draw.ellipse(box, outline=(55, 65, 81), width=round(RING_WIDTH * scale * SUPERSAMPLE))
    for i, agent in enumerate(agents):
        width = RING_WIDTH_SELECTED if i == selected else RING_WIDTH
        colour = STATUS_COLOURS.get(agent["s"], UNKNOWN_COLOUR)
        if i != selected:
            colour = tuple(int(c * 0.6 + b * 0.4) for c, b in zip(colour, BACKGROUND))
        start, end = _segment_angles(i, len(agents))
        draw.arc(box, start, end, fill=colour, width=round(width * scale * SUPERSAMPLE))
    image = big.resize((size, size), Image.LANCZOS)

    # Text is drawn at final resolution so it stays sharp.
    draw = ImageDraw.Draw(image)
    centre = size / 2

    def text(y, value, font, fill):
        draw.text((centre, y * scale), value, font=font, fill=fill, anchor="mm")

    small = _font("segoeui.ttf", round(13 * scale))
    if error or not agents:
        text(SIZE / 2 - 22, "NO HERDR" if error else "NO AGENTS", _font("segoeuib.ttf", round(24 * scale)), MUTED)
        text(SIZE / 2 + 12, error or "Start an agent in herdr", small, DIM)
        return image

    counts = {}
    for agent in agents:
        counts[agent["s"]] = counts.get(agent["s"], 0) + 1
    # Idle agents are left out: the summary is about what's happening, and it has to fit the circle.
    summary = " · ".join(f"{counts[s]} {STATUS_LABELS[s]}" for s in URGENCY if counts.get(s) and s != "idle")
    text(74, summary or f"{len(agents)} idle", _font("segoeui.ttf", round(12 * scale)), MUTED)

    agent = agents[selected]
    status = agent["s"]
    text(108, STATUS_LABELS.get(status, "unknown").upper(), _font("segoeuib.ttf", round(26 * scale)),
         STATUS_COLOURS.get(status, UNKNOWN_COLOUR))

    title_font = _font("segoeui.ttf", round(17 * scale))
    lines = _wrap(draw, agent["n"] or agent["id"], title_font, 210 * scale, 3)
    line_height = 22
    top = 166 - (len(lines) - 1) * line_height / 2
    for i, line in enumerate(lines):
        text(top + i * line_height, line, title_font, TEXT)

    text(226, f"{agent['k'] or 'agent'}  {selected + 1}/{len(agents)}", small, MUTED)
    text(252, "on screen" if agent.get("f") else "click to show", small, DIM)
    return image


def _enable_dpi_awareness():
    try:
        ctypes.windll.user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))  # per-monitor v2
    except (AttributeError, OSError):
        pass


def _round_corners(hwnd):
    DWMWA_WINDOW_CORNER_PREFERENCE, DWMWCP_ROUND = 33, 2
    try:
        value = ctypes.c_int(DWMWCP_ROUND)
        ctypes.windll.dwmapi.DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE,
                                                   ctypes.byref(value), ctypes.sizeof(value))
    except (AttributeError, OSError):
        pass  # older Windows: square corners


def _work_area():
    """The desktop minus the taskbar, in physical pixels."""
    rect = (ctypes.c_long * 4)()
    SPI_GETWORKAREA = 0x0030
    ctypes.windll.user32.SystemParametersInfoW(SPI_GETWORKAREA, 0, rect, 0)
    return rect[0], rect[1], rect[2], rect[3]


class DialPopup:
    def __init__(self, on_focus):
        self._on_focus = on_focus
        self._commands = queue.Queue()
        self._lock = threading.Lock()
        self._agents = []
        self._error = None
        self._version = 0
        threading.Thread(target=self._run, daemon=True).start()

    # --- called from other threads -------------------------------------------------------------

    def update(self, agents, error):
        with self._lock:
            self._agents = list(agents)
            self._error = error
            self._version += 1

    def toggle(self):
        self._commands.put("toggle")

    # --- Tk thread -----------------------------------------------------------------------------

    def _run(self):
        _enable_dpi_awareness()
        self._root = root = tk.Tk()
        root.withdraw()
        root.overrideredirect(True)
        root.attributes("-topmost", True)
        root.configure(bg="#%02x%02x%02x" % BACKGROUND)
        self._scale = root.winfo_fpixels("1i") / 96
        self._label = tk.Label(root, bd=0, highlightthickness=0, bg=root["bg"])
        self._label.pack()
        self._photo = None
        self._shown = False
        self._hidden_at = 0.0
        self._selected = 0
        self._selected_id = None
        self._drawn_version = -1

        self._label.bind("<MouseWheel>", self._on_wheel)
        self._label.bind("<Motion>", self._on_motion)
        self._label.bind("<Button-1>", self._on_click)
        root.bind("<Escape>", lambda e: self._hide())
        root.bind("<FocusOut>", lambda e: self._hide())
        root.after(POLL_MS, self._poll)
        root.mainloop()

    def _snapshot(self):
        with self._lock:
            return self._agents, self._error, self._version

    def _poll(self):
        while True:
            try:
                command = self._commands.get_nowait()
            except queue.Empty:
                break
            if command == "toggle":
                # Clicking the tray icon while the dial is open first takes focus away from the dial,
                # which closes it; don't let the same click then reopen it.
                if self._shown:
                    self._hide()
                elif time.monotonic() - self._hidden_at > REOPEN_GUARD_SECS:
                    self._show()
        if self._shown:
            self._redraw()
        self._root.after(POLL_MS, self._poll)

    def _show(self):
        agents, _, _ = self._snapshot()
        # Open on the agent that most needs you, like the tray's left-click used to jump to.
        if agents:
            top = min(range(len(agents)), key=lambda i: URGENCY.index(agents[i]["s"]) if agents[i]["s"] in URGENCY else 99)
            self._selected, self._selected_id = top, agents[top]["id"]
        self._drawn_version = -1
        self._redraw()
        size = round(SIZE * self._scale)
        left, top, right, bottom = _work_area()
        margin = round(SCREEN_MARGIN * self._scale)
        self._root.geometry(f"{size}x{size}+{right - size - margin}+{bottom - size - margin}")
        self._root.deiconify()
        self._root.update_idletasks()
        _round_corners(ctypes.windll.user32.GetParent(self._root.winfo_id()))
        self._root.focus_force()
        self._shown = True

    def _hide(self):
        if self._shown:
            self._root.withdraw()
            self._shown = False
            self._hidden_at = time.monotonic()

    def _redraw(self, force=False):
        agents, error, version = self._snapshot()
        if not force and version == self._drawn_version:
            return
        # Keep the same agent selected when the list changes underneath us.
        ids = [a["id"] for a in agents]
        if self._selected_id in ids:
            self._selected = ids.index(self._selected_id)
        self._selected = min(self._selected, max(len(agents) - 1, 0))
        self._selected_id = agents[self._selected]["id"] if agents else None
        self._photo = ImageTk.PhotoImage(render(agents, error, self._selected, self._scale))
        self._label.configure(image=self._photo)
        self._drawn_version = version

    def _select(self, index):
        agents, _, _ = self._snapshot()
        if not agents or index == self._selected:
            return
        self._selected = index % len(agents)
        self._selected_id = agents[self._selected]["id"]
        self._redraw(force=True)

    def _on_wheel(self, event):
        agents, _, _ = self._snapshot()
        if agents:
            self._select(self._selected + (1 if event.delta < 0 else -1))

    def _segment_at(self, x, y):
        agents, _, _ = self._snapshot()
        if not agents:
            return None
        size = SIZE * self._scale
        dx, dy = x - size / 2, y - size / 2
        if math.hypot(dx, dy) < size / 2 - (RING_MARGIN + RING_WIDTH_SELECTED + 8) * self._scale:
            return None  # inside the ring: the centre, not a segment
        angle = (math.degrees(math.atan2(dy, dx)) + 90) % 360  # clockwise from 12 o'clock
        return int(angle // (360 / len(agents)))

    def _on_motion(self, event):
        index = self._segment_at(event.x, event.y)
        if index is not None:
            self._select(index)

    def _on_click(self, event):
        agents, _, _ = self._snapshot()
        if not agents:
            return
        index = self._segment_at(event.x, event.y)
        agent_id = agents[self._selected if index is None else index]["id"]
        self._hide()
        self._on_focus(agent_id)
