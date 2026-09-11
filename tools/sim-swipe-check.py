#!/usr/bin/env python3
"""Checks that swiping left and right on home turns the page.

🚨 A gesture only reaches the 'pressed object'. Swiping across an icon is taken
   by the icon, never reaches the screen, and the page does not turn (09-09).
   So it swipes across an icon too.
"""
import subprocess, os, sys, tempfile
D = os.path.dirname(os.path.abspath(__file__)) + "/.."
ERR = open(os.path.join(tempfile.gettempdir(), "sim-swipe.log"), "w")
p = subprocess.Popen([D+'/sim/badge_sim','--serve'], stdin=subprocess.PIPE,
                     stdout=subprocess.PIPE, stderr=ERR, cwd=D)
def send(c): p.stdin.write(c.encode()); p.stdin.flush()
def rd(n):
    b=b''
    while len(b)<n:
        c=p.stdout.read(n-len(b))
        if not c: raise SystemExit("★ died")
        b+=c
    return b
def frame():
    send("R\n"); h=p.stdout.readline().split()
    if not h: raise SystemExit("★ no answer")
    return rd(int(h[1]))
def step(ms): send(f"P {ms}\n")
def swipe(x0, y0, x1, y1, n=8):
    send(f"T {x0} {y0} 1\n"); step(30); frame()
    for i in range(1, n+1):
        send(f"T {int(x0+(x1-x0)*i/n)} {int(y0+(y1-y0)*i/n)} 1\n"); step(25); frame()
    send(f"T {x1} {y1} 0\n"); step(60); frame()
    for _ in range(8): step(50); frame()

for _ in range(8): step(100); frame()
send("K 0\n"); step(100); frame()
# 🚨 The simulator starts on the lock screen. It has to be sent home before swipes can be tried.
send("G\n")
for _ in range(10): step(60); frame()
base = frame()
# Leaving the screens as pictures makes a failure visible.
# 🚨 Some Pythons have no PIL (ESP-IDF's does not). Missing, it is simply skipped —
#    a check must never fail to run over a tool.
try:
    from PIL import Image
    def save(raw, name):
        im = Image.new('RGB', (466, 466)); px = im.load()
        for i in range(466*466):
            v = raw[2*i] | (raw[2*i+1] << 8)
            px[i%466, i//466] = (((v>>11)&31)*255//31, ((v>>5)&63)*255//63, (v&31)*255//31)
        im.save('/tmp/' + name + '.png')
except ImportError:
    def save(raw, name): pass
save(base, 'swipe_before')
fails = 0
for name, y in (("on the bare background", 233), ("across an icon", 111)):
    swipe(360, y, 110, y)
    a = frame()
    save(a, 'swipe_after_'+name[:2])
    moved = sum(1 for i in range(0, 466*466, 29) if a[2*i:2*i+2] != base[2*i:2*i+2]) > 300
    print(f"  {name}, swipe left → {'turned' if moved else '★ did not turn'}")
    if not moved: fails += 1
    swipe(110, y, 360, y)          # back again
    b = frame()
    back = sum(1 for i in range(0, 466*466, 29) if b[2*i:2*i+2] != base[2*i:2*i+2]) < 300
    print(f"  {name}, swipe back right → {'returned' if back else '★ did not return'}")
    if not back: fails += 1
print("all passed" if fails == 0 else f"★ {fails} failed")
sys.exit(1 if fails else 0)
