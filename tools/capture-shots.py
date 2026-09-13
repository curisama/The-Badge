#!/usr/bin/env python3
"""Takes the screenshots the docs page uses, straight from the simulator.

Run it after changing anything the page shows, or the pictures quietly drift
away from the firmware:

    python3 sim/build.py && python3 tools/capture-shots.py

🚨 It sends `!` (hide the home handle), never `#`. `#` hides every lv_button on
   the screen, which is right for baking app icons and wrong here — with `#`
   the timer loses its 5/15/25 presets, the games menu comes out empty and the
   calculator has no keys.

🚨 Games and the marble need a tap to start, and that tap also takes the
   current attitude as level. So the tilt is set flat first, then the start
   tap, then the tilt that actually plays it. The marble's start tap must not
   land dead centre either — the marble itself sits there and eats it.
"""
import os, struct, subprocess, sys, tempfile, zlib

D = os.path.dirname(os.path.abspath(__file__)) + "/.."
OUT = os.path.join(D, "docs", "img")
ERR = open(os.path.join(tempfile.gettempdir(), "capture-shots.log"), "w")
p = subprocess.Popen([D + "/sim/badge_sim", "--serve"], stdin=subprocess.PIPE,
                     stdout=subprocess.PIPE, stderr=ERR, cwd=D)


def send(c):
    p.stdin.write(c.encode())
    p.stdin.flush()


def rd(n):
    b = b""
    while len(b) < n:
        c = p.stdout.read(n - len(b))
        if not c:
            raise SystemExit("★ the simulator died — see " + ERR.name)
        b += c
    return b


def shot(name):
    send("F\n")
    head = b""
    while not head.endswith(b"\n"):
        head += p.stdout.read(1)
    raw = rd(int(head.split()[1]))
    w = h = 466
    rows = b"".join(b"\x00" + raw[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(rows, 9))
           + chunk(b"IEND", b""))
    path = os.path.join(OUT, name + ".png")
    open(path, "wb").write(png)
    print("  " + os.path.relpath(path, D))


def wait(ms):  send("P %d\n" % ms)
def bare():    send("!\n"); wait(200)
# 🚨 Two different doors, and they are not interchangeable. 'G'
#    (launcher_show_home) is the only one that works from the lock screen,
#    where the simulator starts — but it *builds* home, so an 'A' sent straight
#    after it lands on the screen being replaced and the app never opens.
#    'H' is the physical home button: useless on the lock screen, right
#    everywhere else. So 'G' once at the top, 'H' between apps.
def home():    send("H\n"); wait(700)
def tilt(x, y, ms):  send("I %d %d 0\n" % (x, y)); wait(ms)


def tap(x, y, hold=80, after=450):
    send("T %d %d 1\n" % (x, y)); wait(hold)
    send("T %d %d 0\n" % (x, y)); wait(after)


def swipe(x0, y0, x1, y1, n=10):
    send("T %d %d 1\n" % (x0, y0)); wait(30)
    for i in range(1, n + 1):
        send("T %d %d 1\n" % (x0 + (x1 - x0) * i // n, y0 + (y1 - y0) * i // n))
        wait(25)
    send("T %d %d 0\n" % (x1, y1)); wait(700)


def app(n):
    """The sim's A command: 0 mouse, 2 keys, 3 calc, 4 games, 5 clock,
    6 water, 7 orbs, 8 settings."""
    home(); send("A %d\n" % n); wait(1200)


GAME_Y = {"bricks": 98, "pinball": 188, "marble": 278, "pop": 368}


def game(which, play, tap_at=(233, 233)):
    app(4)
    tap(233, GAME_Y[which], 80, 900)
    tilt(0, 1000, 400)              # a level attitude before the start tap
    tap(tap_at[0], tap_at[1], 80, 120)
    play()
    bare(); shot(which)


wait(800)
send("K 0\n")          # never turn the screen off on us mid-capture
print("capturing into docs/img/")

# ── lock, home ────────────────────────────────────────────────
shot("lock")           # the simulator starts here
send("G\n"); wait(900); bare(); shot("home")

# ── the clock app's four pages ────────────────────────────────
app(5); bare()
shot("clock")
swipe(233, 380, 233, 110); wait(600); bare(); shot("timer")
swipe(233, 380, 233, 110); wait(400)
tap(233, 233, 80, 2600)                       # run it
send("T 233 233 1\n"); wait(1200)             # hold = lap
send("T 233 233 0\n"); wait(1500)
bare(); shot("stopwatch")
swipe(233, 380, 233, 110); wait(600); bare(); shot("alarm")

# ── the rest of the apps ──────────────────────────────────────
app(0); shot("mouse")
app(2); bare(); shot("keys")
# 🚨 Put the screen-off delay back to the default first. The K 0 above keeps
#    the screen alive through the capture, but settings writes the live value
#    into its own row, so with it left at 0 the picture says "never".
send("K 30\n"); wait(100)
app(8); bare(); shot("settings")
send("K 0\n"); wait(100)
app(3); bare()
for x, y in [(65, 178), (405, 243), (172, 178), (285, 395)]:   # 7 x 8 =
    tap(x, y)
bare(); shot("calc")

# 🚨 Gravity on screen is -x, not +y: "I 0 1000 0" pours the water out to the
#    left. Settle it at the bottom first, then tip it enough for the boat to
#    ride the slope.
app(6); tilt(-1000, 0, 3000); tilt(-950, 300, 700); bare(); shot("water")
app(7); tap(233, 200); wait(1500); send("L 0.62\n"); wait(200); bare(); shot("orbit")

app(1)                                          # the recorder
bare(); tap(233, 233, 80, 3000); shot("rec")

# ── the games ─────────────────────────────────────────────────
game("bricks", lambda: [tilt(-160, 990, 400), tilt(140, 990, 400),
                        tilt(-90, 995, 400)])
game("pinball", lambda: [tilt(-300, 900, 2200), tap(90, 380, 120, 300),
                         tap(376, 380, 120, 300), wait(600)])
game("marble", lambda: [tilt(260, 260, 450), tilt(-180, 330, 450),
                        tilt(300, -120, 400)], tap_at=(150, 340))
game("pop", lambda: [tap(x, y, 60, 150) for x, y in
                     [(150, 180), (210, 150), (280, 200),
                      (180, 260), (300, 300), (240, 330)]])
app(4); bare(); shot("games")

send("Q\n")
p.wait(timeout=10)
print("done. They are written full size — squeeze them before committing:")
print("  for f in docs/img/*.png; do convert \"$f\" -strip -colors 255 "
      "-define png:compression-level=9 \"$f\"; done")
