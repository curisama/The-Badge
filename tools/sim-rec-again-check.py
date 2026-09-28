#!/usr/bin/env python3
"""Does a second recording start after the first one — pressed for real.

After one recording is stopped, tapping REC again must start a new one,
even with finished recordings still waiting on the device, and even when
the taps come quickly right after stop (the mic task may still be
finishing — it used to be refused and show "!").

  python3 tools/sim-rec-again-check.py
"""
import os, subprocess, sys, tempfile

D = os.path.dirname(os.path.abspath(__file__)) + "/.."
SIM = os.path.join(D, "sim", "badge_sim.exe")
if not os.path.exists(SIM):
    SIM = os.path.join(D, "sim", "badge_sim")
LOG = os.path.join(tempfile.gettempdir(), "sim-rec-again-check.log")
MID = (233, 233)                        # the big centre button


def run(gap_ms):
    err = open(LOG, "w")
    env = dict(os.environ)
    p = subprocess.Popen([SIM, "--serve"], stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE, stderr=err, cwd=D, env=env)

    def send(c):
        p.stdin.write(c.encode()); p.stdin.flush()

    def rd(n):
        b = b""
        while len(b) < n:
            c = p.stdout.read(n - len(b))
            if not c:
                raise SystemExit("* simulator died")
            b += c
        return b

    def frame():
        send("F\n")
        h = p.stdout.readline().split()
        if not h:
            raise SystemExit("* simulator stopped answering")
        return rd(int(h[1]))

    def step(ms):
        send("P %d\n" % ms)

    def wait(ms):
        for _ in range(max(1, ms // 50)):
            step(50); frame()

    def tap():
        x, y = MID
        send("T %d %d 1\n" % (x, y)); step(60); frame()
        send("T %d %d 0\n" % (x, y)); step(60); frame()

    wait(800)
    send("K 0\n"); wait(100)            # unlock
    send("A 1\n"); wait(600)
    for _ in range(3):
        tap(); wait(1500)               # record
        tap(); wait(gap_ms)             # stop -> OK
        tap(); wait(gap_ms)             # OK -> idle
    send("Q\n"); p.wait(timeout=5)
    err.close()
    return open(LOG, encoding="utf-8", errors="replace").read().count("recording started")


bad = 0
for gap in (600, 60):
    n = run(gap)
    ok = n == 3
    print("  gap %4dms - %d recordings started %s" % (gap, n, "" if ok else "* expected 3"))
    bad += not ok
sys.exit(1 if bad else 0)
