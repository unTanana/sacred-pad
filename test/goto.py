#!/usr/bin/env python3
"""Steer the hero toward an object with the left stick (test helper). usage: goto.py REF [stop_px]"""
import math, subprocess, sys, time, os
T = os.path.dirname(os.path.abspath(__file__))
LOG, CTL = f"{T}/../sandbox/sacredpad.log", f"{T}/../logs/ctl.txt"
ref, stop = sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 150

def where():
    n = sum(1 for _ in open(LOG))
    open(CTL, "w").write("DUMP"); time.sleep(0.3); open(CTL, "w").write("")
    for line in open(LOG).readlines()[n:]:
        if f"ref={ref} " in line and "screen=" in line:
            x, y = line.split("screen=")[1].split()[0].split(",")
            return int(x) - 512, int(y) - 384
    return None

last, turn = None, 0
for step in range(80):
    p = where()
    if not p: sys.exit(f"ref {ref} not found")
    d = math.hypot(*p)
    print(f"step {step}: {d:.0f}px", flush=True)
    if d < stop: sys.exit(0)
    if last is not None and last - d < 60: turn += 1  # stuck: try another heading
    elif last is not None: turn = 0
    a = math.atan2(-p[1], p[0]) + [0, 1.0, -1.0, 1.8, -1.8, 2.6, -2.6][turn % 7]
    open(CTL, "w").write(f"LX={math.cos(a):.2f} LY={math.sin(a):.2f}")
    time.sleep(1.5)
    open(CTL, "w").write(""); last = d
sys.exit("gave up")
