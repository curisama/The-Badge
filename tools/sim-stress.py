#!/usr/bin/env python3
"""Runs the six boards under the worst conditions — a shaken clock, and
entering and leaving at random.

🚨 In the simulator time flows as evenly as a ruler and memory is unlimited.
   Left that way it catches nothing that only breaks on the hardware. 'J' shakes
   the clock, and building with SIM_TIGHT=1 narrows LVGL's memory to what the
   hardware has (87 KB, or 13 KB with BLE on).
"""
import subprocess, sys, os, tempfile, random
D = os.path.dirname(os.path.abspath(__file__)) + "/.."
ERR = open(os.path.join(tempfile.gettempdir(), "sim-stress.log"), "w")
p = subprocess.Popen([D + '/sim/badge_sim', '--serve'], stdin=subprocess.PIPE,
                     stdout=subprocess.PIPE, stderr=ERR, cwd=D)
def send(c): p.stdin.write(c.encode()); p.stdin.flush()
def rd(n):
    b = b''
    while len(b) < n:
        c = p.stdout.read(n - len(b))
        if not c: raise SystemExit("★ the simulator died")
        b += c
    return b
def frame():
    send("R\n"); h = p.stdout.readline().split()
    if not h: raise SystemExit("★ the simulator died (no answer)")
    return rd(int(h[1]))
def step(ms): send(f"P {ms}\n")
def line(cmd):
    send(cmd); return p.stdout.readline().decode().strip()
def tap(x, y, hold=140):
    send(f"T {x} {y} 1\n"); step(60); frame(); step(hold); frame()
    send(f"T {x} {y} 0\n"); step(60); frame(); step(120); frame()

GRID = {"Bricks": (149, 145), "Marble": (317, 145), "Pop": (141, 233),
        "Water": (325, 233), "Moon": (149, 321), "Earth": (317, 321)}

for _ in range(8): step(100); frame()
send("K 0\n"); step(100); frame()
print(line("J 250\n"))            # shake the clock the way the hardware does
random.seed(3)
worst = 0
import sys
ROUNDS = int(sys.argv[1]) if len(sys.argv) > 1 else 3
for rnd in range(ROUNDS):
    for name, (x, y) in GRID.items():
        send("A 4\n"); step(400); frame()
        tap(x, y); step(400); frame()
        print(line("D 0\n"), end="\r")
        # random smearing
        for k in range(18):
            a, b = random.randint(60, 400), random.randint(60, 400)
            send(f"T {a} {b} 1\n"); step(40); frame()
            send(f"T {a} {b} 0\n"); step(40); frame()
        for _ in range(20): step(33); frame()
        d = line("D 1\n").split()
        fps = float([v for v in d if v.startswith('fpscap=')][0].split('=')[1])
        if worst == 0 or fps < worst: worst = fps
        send("H\n")
        for _ in range(12): step(50); frame()
    print(f"  round {rnd+1} passed")
print(f"all passed · lowest fps the bandwidth allows {worst:.1f}")
