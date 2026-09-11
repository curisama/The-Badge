#!/usr/bin/env python3
"""판마다 들어갔다 홈으로 나오기를 해본다.

🚨 나가는 길에서 두 번 죽었다(달·지구 0909, 물 0909). 원인이 같다 —
   그림이 가리키는 메모리를 그림보다 먼저 놓으면, 지워지기 전에 한 번 더
   그리면서 이미 놓은 자리를 읽는다. 눈으로는 "홈 누르면 끊긴다"로만 보인다.
   새 판을 넣을 때마다 이걸 돌려라.

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
        if not c: raise SystemExit("★ 시뮬이 죽었다")
        b+=c
    return b
def frame():
    send("R\n"); h=p.stdout.readline().split()
    if not h: raise SystemExit("★ 시뮬이 죽었다(응답 없음)")
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
    send("H\n")                      # 파워 버튼 = 홈
    for _ in range(15): step(50); frame()
    print(f"{name} → 홈 통과")
print("전부 통과")
