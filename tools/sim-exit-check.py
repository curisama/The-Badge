#!/usr/bin/env python3
"""Enters each board and comes back home.

🚨 It died on the way out twice (the orbs 09-09, water 09-09). The cause was the
   same both times — freeing the memory a picture points at before the picture
   itself means one more draw happens before deletion and reads what was
   already freed. To the eye it only looks like "pressing home cuts out".
   Run this every time a new board goes in.

  python3 tools/sim-exit-check.py
"""
import subprocess, sys, os, tempfile
D = os.path.dirname(os.path.abspath(__file__)) + "/.."
ERR = open(os.path.join(tempfile.gettempdir(), "sim-exit-check.log"), "w")
p=subprocess.Popen([D+'/sim/badge_sim','--serve'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=ERR,cwd=D)
def send(c): p.stdin.write(c.encode()); p.stdin.flush()
def rd(n):
    b=b''
    while len(b)<n:
        c=p.stdout.read(n-len(b))
        if not c: raise SystemExit("★ the simulator died")
        b+=c
    return b
def frame():
    send("R\n"); h=p.stdout.readline().split()
    if not h: raise SystemExit("★ the simulator died (no answer)")
    return rd(int(h[1]))
def step(ms): send(f"P {ms}\n")
def tap(x,y,hold=140):
    send(f"T {x} {y} 1\n"); step(60); frame(); step(hold); frame()
    send(f"T {x} {y} 0\n"); step(60); frame(); step(120); frame()
GRID={"Bricks":(233-84,233-88),"Marble":(233+84,233-88),"Pop":(233-92,233),
      "Water":(233+92,233),"Moon":(233-84,233+88),"Earth":(233+84,233+88)}
for _ in range(8): step(100); frame()
send("K 0\n"); step(100); frame()
for name in ["Water","Moon","Earth","Pop","Bricks","Marble"]:
    send("A 4\n"); step(400); frame()
    x,y=GRID[name]; tap(x,y); step(500); frame()
    for _ in range(10): step(33); frame()
    send("H\n")                      # power button = home
    for _ in range(15): step(50); frame()
    print(f"{name} → home passed")
print("all passed")
