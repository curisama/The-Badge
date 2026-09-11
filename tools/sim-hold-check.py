#!/usr/bin/env python3
"""길게 누르기가 화면 다시 짓기에 안 죽나 — 호스트 목록으로 재본다.

🚨 LVGL 은 **눌린 객체가 지워지면 손을 뗄 때까지 그 입력장치를 무시한다**
   (lv_obj_tree.c 의 `lv_indev_wait_release`). 그래서 목록을 주기적으로 다시
   짓는 화면에서는 길게 누르기가 영영 안 걸린다 — 더 오래 눌러도 소용없다.
   0911 에 블투 호스트 이름 짓기가 이것 때문에 안 됐다.

   모양(정규식)으로는 못 잡는다. 실제로 눌러보고 키패드가 뜨는지 본다.
   누르기 시작 시점을 여러 개로 흩어서, 다시 짓는 순간과 겹쳐도 살아남는지
   확인한다.

  python3 tools/sim-hold-check.py
"""
import os, struct, subprocess, sys, tempfile

D = os.path.dirname(os.path.abspath(__file__)) + "/.."
SIM = os.path.join(D, "sim", "badge_sim.exe")
if not os.path.exists(SIM):
    SIM = os.path.join(D, "sim", "badge_sim")
ERR = open(os.path.join(tempfile.gettempdir(), "sim-hold-check.log"), "w")

# 키패드가 떴는지 보는 자리 — 오른쪽 위 확인(✓) 단추 한가운데.
# 목록 화면에서는 거기가 까맣다.
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
                raise SystemExit("★ 시뮬이 죽었다")
            b += c
        return b

    def frame():
        send("F\n")
        h = p.stdout.readline().split()
        if not h:
            raise SystemExit("★ 시뮬이 응답을 안 한다")
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
    send("A 8\n")                       # 설정
    for _ in range(10):
        step(50); frame()
    drag(233, 380, 120)                 # 목록을 굴려 Bluetooth 를 꺼낸다
    tap(233, 317)                       # Bluetooth
    for _ in range(10):
        step(50); frame()
    for _ in range(wait_ms // 40):      # 다시 짓는 순간과 어긋나게 흘려보낸다
        step(40); frame()
    send("T 233 194 1\n")               # 둘째 줄을 길게
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
    up = (r + g + b) > 150              # 까맣지 않으면 키패드가 떠 있다
    print("  %4dms 뒤 %4dms 누름 → %s" % (wait, hold, "키패드" if up else "★ 아무 일도 안 일어남"))
    if not up:
        bad += 1

sys.exit(1 if bad else 0)
