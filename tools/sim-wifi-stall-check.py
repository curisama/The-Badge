#!/usr/bin/env python3
"""Does the WiFi screen get stuck when the scan never answers - by pressing it.

🚨 On 09-13 the device sat on "looking around..." and never moved on. When the
   layer below holds the result at -1 for ever, the screen had no bound and
   asked again every 200 ms until a reboot. The back button worked, but coming
   back in landed on the same screen.

   The shape of the source cannot catch this (a check that looks for a timeout
   constant goes stale the moment the value changes). BADGE_SIM_SCAN_STALL=1
   blocks the answer, and this watches whether the screen finds its way out.

  python3 tools/sim-wifi-stall-check.py
"""
import os, subprocess, sys, tempfile

D = os.path.dirname(os.path.abspath(__file__)) + "/.."
SIM = os.path.join(D, "sim", "badge_sim.exe")
if not os.path.exists(SIM):
    SIM = os.path.join(D, "sim", "badge_sim")
ERR = open(os.path.join(tempfile.gettempdir(), "sim-wifi-stall-check.log"), "w")

WIFI_ROW = (233, 246)      # the Wi-Fi row, after the settings list is dragged up
NOTE     = (233, 233)      # the middle, where the note sits


def run():
    env = dict(os.environ, BADGE_SIM_SCAN_STALL="1")
    p = subprocess.Popen([SIM, "--serve"], stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE, stderr=ERR, cwd=D, env=env)

    def send(c):
        p.stdin.write(c.encode()); p.stdin.flush()

    def rd(n):
        b = b""
        while len(b) < n:
            c = p.stdout.read(n - len(b))
            if not c:
                raise SystemExit("* the simulator died")
            b += c
        return b

    def frame():
        send("F\n")
        h = p.stdout.readline().split()
        if not h:
            raise SystemExit("* the simulator stopped answering")
        return rd(int(h[1]))

    def step(ms, chunk=40):
        for _ in range(max(1, ms // chunk)):
            send("P %d\n" % chunk); frame()

    def tap(x, y):
        send("T %d %d 1\n" % (x, y)); step(120)
        send("T %d %d 0\n" % (x, y)); step(300)

    def drag(x, y0, y1, n=12):
        send("T %d %d 1\n" % (x, y0)); step(50)
        for i in range(1, n + 1):
            send("T %d %d 1\n" % (x, y0 + (y1 - y0) * i // n)); step(40)
        send("T %d %d 0\n" % (x, y1)); step(300)

    def red_at(buf, xy):
        i = (xy[1] * 466 + xy[0]) * 3
        r, g, b = buf[i], buf[i + 1], buf[i + 2]
        return r > 120 and r > g + 40 and r > b + 40

    step(800)
    send("K 0\n"); step(400)
    send("A 8\n"); step(600)
    drag(233, 380, 140)
    tap(*WIFI_ROW)

    # Run past the 20 s bound. Stuck means the grey "looking around..." is still there.
    step(24000, chunk=200)
    buf = frame()
    # Red text on the note line means it found its way out.
    row = any(red_at(buf, (x, NOTE[1])) for x in range(80, 390, 2))
    send("Q\n")
    try:
        p.wait(timeout=5)
    except Exception:
        p.kill()
    return row


if run():
    print("  it leaves the screen even with no answer (and says to try again)")
    sys.exit(0)
print("  * stuck on the scan screen - no way out when no answer comes")
sys.exit(1)
