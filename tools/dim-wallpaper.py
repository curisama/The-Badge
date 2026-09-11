#!/usr/bin/env python3
"""홈 배경을 원하는 밝기로 다시 굽는다.

AMOLED 는 켜진 픽셀만큼 전기를 먹는다 — 검은 픽셀은 아예 안 켜진다.
그래서 배경을 눌러 놓으면 홈 화면 소비가 실제로 준다. 그릴 때마다
계산해서 어둡게 할 수도 있지만, 그러면 아끼려다 CPU 를 더 쓴다.
파일에 미리 구워두는 게 맞다.

  python3 tools/dim-wallpaper.py 0.6    # 60% 밝기 (지금 값)
  python3 tools/dim-wallpaper.py 1.0    # 원본으로

🚨 이미 어둡게 구운 파일에 또 돌리면 그만큼 더 어두워진다. 되돌리려면
   git 에서 받아 오거나, 지금 계수의 역수를 곱해야 한다.
"""
import re, sys, os

K = float(sys.argv[1]) if len(sys.argv) > 1 else 0.6
p = os.path.join(os.path.dirname(__file__), "..", "main", "assets", "home_bg.c")
src = open(p).read()
m = re.search(r'home_bg_map\[\]\s*=\s*\{(.*?)\n\};', src, re.S)
vals = re.findall(r'0x([0-9a-fA-F]{2})', m.group(1))
out = []
for i in range(0, len(vals), 2):
    c = (int(vals[i+1], 16) << 8) | int(vals[i], 16)      # 리틀엔디안 RGB565
    r = int(((c >> 11) & 0x1F) * K)
    g = int(((c >> 5) & 0x3F) * K)
    b = int((c & 0x1F) * K)
    c2 = (r << 11) | (g << 5) | b
    out.append(f"0x{c2 & 0xFF:02x}, 0x{(c2 >> 8) & 0xFF:02x},")
lines = ["    " + " ".join(out[i:i+12]) for i in range(0, len(out), 12)]
open(p, "w").write(src[:m.start(1)] + "\n" + "\n".join(lines) + src[m.end(1):])
print(f"✓ 배경을 {K*100:.0f}% 밝기로 구웠다 ({len(vals)//2} 픽셀)")
