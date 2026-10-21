"""Summarise knob battery runs from the bridge's battery CSV.

A run is a stretch of reports where the knob was on battery (level known, no external power), with
no gap longer than MAX_GAP. For each run this prints the drain rate and a projected full runtime.

    python bridge/battery_report.py [logs/battery.csv]

Percentages come from a voltage curve, so short runs (under an hour) and runs that start straight
off the charger read pessimistic: a LiPo sheds voltage quickly for the first few minutes.
"""

import csv
import datetime
import os
import sys

MAX_GAP = datetime.timedelta(minutes=5)
SCREEN_NAMES = {"0": "off", "1": "dim", "2": "bright"}


def load(path):
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            row["time"] = datetime.datetime.fromisoformat(row["time"])
            yield row


def on_battery(row):
    return row["known"] == "1" and row["ext"] == "0"


def split_runs(rows):
    runs, current = [], []
    for row in rows:
        if on_battery(row) and (not current or row["time"] - current[-1]["time"] <= MAX_GAP):
            current.append(row)
            continue
        if len(current) > 1:
            runs.append(current)
        current = [row] if on_battery(row) else []
    if len(current) > 1:
        runs.append(current)
    return runs


def describe(run):
    first, last = run[0], run[-1]
    hours = (last["time"] - first["time"]).total_seconds() / 3600
    dropped = int(first["percent"]) - int(last["percent"])
    firmware = sorted({r["firmware"] or "?" for r in run})
    cpu = sorted({r["cpu_mhz"] for r in run if r["cpu_mhz"]})
    screens = {}
    for r in run:
        name = SCREEN_NAMES.get(r["screen"], "?")
        screens[name] = screens.get(name, 0) + 1
    screen_mix = ", ".join(f"{name} {100 * n // len(run)}%" for name, n in sorted(screens.items()))

    print(f"{first['time']:%Y-%m-%d %H:%M} -> {last['time']:%H:%M}  ({hours:.2f} h)")
    print(f"  firmware {', '.join(firmware)}; cpu {', '.join(cpu) or '?'} MHz; screen time: {screen_mix}")
    print(f"  {first['percent']}% ({first['mv']} mV) -> {last['percent']}% ({last['mv']} mV)")
    if hours > 0 and dropped > 0:
        rate = dropped / hours
        print(f"  {rate:.1f} %/h -> about {100 / rate:.1f} h from full")
    else:
        print("  not enough drop to estimate a rate yet")
    print()


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "logs", "battery.csv")
    if not os.path.exists(path):
        print(f"no battery log at {path}")
        return 1
    runs = split_runs(load(path))
    if not runs:
        print("no on-battery runs in the log yet")
        return 0
    for run in runs:
        describe(run)
    return 0


if __name__ == "__main__":
    sys.exit(main())
