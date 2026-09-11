"""Captures the three app icons that are pictures of the app's own screen.

The water, moon and earth icons in main/assets/ are the running app shrunk to
120 px. They have to be captured with the buttons hidden — the '#' command —
or the back button is baked into the icon as a smudge on the rim.

    python3 sim/build.py
    python3 tools/capture-icons.py     # writes shots/icon_*.png
    python3 tools/mkassets.py          # bakes them into main/assets/

Everything else mkassets.py emits it draws itself and needs nothing captured.
"""
import subprocess, os, struct, zlib, tempfile
D=os.path.dirname(os.path.abspath(__file__)) + "/.."
ERR=open(os.path.join(tempfile.gettempdir(), 'capture-icons.log'), 'w')
p=subprocess.Popen([D+'/sim/badge_sim','--serve'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=ERR,cwd=D)
def send(c): p.stdin.write(c.encode()); p.stdin.flush()
def rd(n):
    b=b''
    while len(b)<n:
        c=p.stdout.read(n-len(b))
        if not c: raise SystemExit("died")
        b+=c
    return b
def line():
    b=b''
    while not b.endswith(b'\n'):
        c=p.stdout.read(1)
        if not c: raise SystemExit("no answer")
        b+=c
    return b.decode().strip()
def frame(name):
    send("F\n"); h=line(); n=int(h.split()[1]); raw=rd(n)
    W=H=466
    rows=b''.join(b'\x00'+raw[y*W*3:(y+1)*W*3] for y in range(H))
    def chunk(t,d): return struct.pack('>I',len(d))+t+d+struct.pack('>I',zlib.crc32(t+d)&0xffffffff)
    png=b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',W,H,8,2,0,0,0))+chunk(b'IDAT',zlib.compress(rows,6))+chunk(b'IEND',b'')
    open(name,'wb').write(png); print("wrote",name)
def tap(x,y):
    send(f"T {x} {y} 1\n"); send("P 60\n"); send(f"T {x} {y} 0\n"); send("P 300\n")
# orbs
for name,y in (("moon",134),("earth",200)):
    send("H\n"); send("P 500\n"); send("A 7\n"); send("P 600\n")
    tap(233,y); send("P 1200\n"); send("#\n"); send("P 200\n")
    frame(f'shots/icon_{name}.png')
# water
send("H\n"); send("P 500\n"); send("A 6\n"); send("P 1500\n"); send("#\n"); send("P 200\n")
frame('shots/icon_water.png')
send("Q\n"); p.wait(timeout=5)
