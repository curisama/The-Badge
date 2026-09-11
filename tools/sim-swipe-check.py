#!/usr/bin/env python3
"""홈에서 좌우로 쓸어 쪽이 넘어가나 확인한다.

🚨 제스처는 '눌린 객체' 에게만 간다. 아이콘 위에서 쓸면 아이콘이 먹고
   화면까지 안 올라와 쪽이 안 넘어간다(0909). 아이콘 위에서도 쓸어본다.
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
        if not c: raise SystemExit("★ 죽음")
        b+=c
    return b
def frame():
    send("R\n"); h=p.stdout.readline().split()
    if not h: raise SystemExit("★ 응답없음")
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
# 🚨 시뮬은 잠금화면으로 시작한다. 홈으로 보내야 쓸기를 시험할 수 있다.
send("G\n")
for _ in range(10): step(60); frame()
base = frame()
# 화면을 그림으로 남겨두면 왜 실패했는지 눈으로 본다.
# 🚨 PIL 이 없는 파이썬도 있다(ESP-IDF 것). 없으면 그냥 건너뛴다 —
#    검사가 도구 때문에 못 도는 일은 없어야 한다.
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
for name, y in (("빈 배경에서", 233), ("아이콘 위에서", 111)):
    swipe(360, y, 110, y)
    a = frame()
    save(a, 'swipe_after_'+name[:2])
    moved = sum(1 for i in range(0, 466*466, 29) if a[2*i:2*i+2] != base[2*i:2*i+2]) > 300
    print(f"  {name} 왼쪽으로 쓸기 → {'넘어감' if moved else '★ 안 넘어감'}")
    if not moved: fails += 1
    swipe(110, y, 360, y)          # 되돌린다
    b = frame()
    back = sum(1 for i in range(0, 466*466, 29) if b[2*i:2*i+2] != base[2*i:2*i+2]) < 300
    print(f"  {name} 오른쪽으로 되돌리기 → {'돌아옴' if back else '★ 안 돌아옴'}")
    if not back: fails += 1
print("전부 통과" if fails == 0 else f"★ {fails}건 실패")
sys.exit(1 if fails else 0)
