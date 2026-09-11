#!/usr/bin/env python3
"""Does a long press survive the screen being rebuilt — measured on the host list.

🚨 LVGL **ignores an input device until release once the pressed object is
   deleted** (`lv_indev_wait_release` in lv_obj_tree.c). So on a screen that
   periodically rebuilds its list, a long press never lands — holding longer
   does not help. On 09-11 that is what stopped Bluetooth hosts being renamed.

   No pattern (regex) catches it. It presses for real and watches for the
   keypad. The press start times are scattered, to see whether it survives
   landing on the moment of a rebuild.

  python3 tools/sim-hold-check.py
"""
import os, struct, subprocess, sys, tempfile

D = os.path.dirname(os.path.abspath(__file__)) + "/.."
SIM = os.path.join(D, "sim", "badge_sim.exe")
if not os.path.exists(SIM):
    SIM = os.path.join(D, "sim", "badge_sim")
ERR = open(os.path.join(tempfile.gettempdir(), "sim-hold-check.log"), "w")

# Where to look for the keypad — the middle of the confirm (✓) button, top right.
# On the list screen that spot is black.
PROBE = (335, 61)


def once(wait_ms, hold_ms):
    p = subprocess.Popen([SIM, "--serve"], stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE, stderr=ERR, cwd=D)

    def send(c):
        p.stdin.write(c.encode()); p.stdin.flush()

    def rd(n):
        b = b""
        while len(b) < n:
            c = p.stdout.read(n - len(b))
            if not c:
                raise SystemExit("★ the simulator died")
            b += c
        return b

    def frame():
        send("F\n")
        h = p.stdout.readline().split()
        if not h:
            raise SystemExit("★ the simulator is not answering")
        return rd(int(h[1]))

    def step(ms):
        send("P %d\n" % ms)

    def tap(x, y):
        send("T %d %d 1\n" % (x, y)); step(60); frame(); step(120); frame()
        send("T %d %d 0\n" % (x, y)); step(60); frame(); step(250); frame()

    def drag(x, y0, y1, n=12):
        send("T %d %d 1\n" % (x, y0)); step(50); frame()
        for i in range(1, n + 1):
            send("T %d %d 1\n" % (x, y0 + (y1 - y0) * i // n)); step(40); frame()
        send("T %d %d 0\n" % (x, y1)); step(60); frame(); step(300); frame()

    for _ in range(8):
        step(100); frame()
    send("K 0\n"); step(100); frame()
    send("A 8\n")                       # settings
    for _ in range(10):
        step(50); frame()
    drag(233, 380, 120)                 # scroll the list to bring Bluetooth out
    tap(233, 317)                       # Bluetooth
    for _ in range(10):
        step(50); frame()
    for _ in range(wait_ms // 40):      # drift out of step with the rebuild
        step(40); frame()
    send("T 233 194 1\n")               # long-press the second row
    for _ in range(hold_ms // 40):
        step(40); frame()
    send("T 233 194 0\n"); step(60); frame(); step(300); frame()
    buf = frame()
    send("Q\n"); p.wait(timeout=5)

    i = (PROBE[1] * 466 + PROBE[0]) * 3
    return buf[i], buf[i + 1], buf[i + 2]


bad = 0
for wait, hold in ((0, 1600), (800, 1600), (1200, 3000), (1500, 1600), (1800, 1600)):
    r, g, b = once(wait, hold)
    up = (r + g + b) > 150              # not black means the keypad is up
    print("  held %4dms after %4dms → %s" % (hold, wait, "keypad" if up else "★ nothing happened"))
    if not up:
        bad += 1

sys.exit(1 if bad else 0)
