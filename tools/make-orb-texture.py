#!/usr/bin/env python3
"""달·지구 전개도를 사진에서 구워 넣는다.

지금 코드에 든 전개도는 코드로 만든 것이다. 달은 그럴듯하지만 지구는
대륙 모양이 실제와 달라서 "지구 비슷한 행성"일 뿐이다. 진짜를 쓰려면
적도 전개도(equirectangular) 사진을 받아서 이걸 돌려라.

  python3 tools/make-orb-texture.py moon    ~/Downloads/moon_8k.jpg
  python3 tools/make-orb-texture.py earth   ~/Downloads/blue_marble.jpg
  python3 tools/make-orb-texture.py sun     ~/Downloads/sun.jpg
  python3 tools/make-orb-texture.py jupiter ~/Downloads/jupiter.jpg

받을 곳(공개 자료):
  달   https://svs.gsfc.nasa.gov/4720   (NASA LROC, 퍼블릭 도메인)
  지구 https://visibleearth.nasa.gov/collection/1484/blue-marble
  둘 다 https://www.solarsystemscope.com/textures/  (CC BY 4.0, NASA 자료 기반)
  받아 쓴 곳: three.js(지구·달) · homer-jay/solar-system-textures(태양)
             jeromeetienne/threex.planets(목성)

가로가 경도 360도, 세로가 위도 180도인 그림이어야 한다. 세로로 반쪽이거나
극지방이 잘린 그림을 넣으면 극이 늘어져 보인다.

결과는 main/assets/orb_tex_<이름>.h 로 나가고, orb.c 가 있으면 알아서 쓴다.
플래시는 하나당 256KB (512x256 RGB565).
"""
import sys, os
from PIL import Image

W, H = 512, 256          # orb.c 의 TEX_W / TEX_H 와 같아야 한다

def main():
    if len(sys.argv) != 3 or sys.argv[1] not in ("moon", "earth", "sun", "jupiter"):
        print(__doc__); sys.exit(1)
    kind, src = sys.argv[1], sys.argv[2]
    im = Image.open(src).convert("RGB")
    if abs(im.width / im.height - 2.0) > 0.15:
        print(f"⚠ 가로:세로가 {im.width}x{im.height} 다. 적도 전개도는 2:1 이어야 한다.")
        print("  그대로 진행하면 극지방이 늘어져 보인다.")
    im = im.resize((W, H), Image.LANCZOS)
    px = im.load()

    name = f"ORB_TEX_{kind.upper()}"
    out = os.path.join(os.path.dirname(__file__), "..", "main", "assets",
                       f"orb_tex_{kind}.h")
    with open(out, "w") as f:
        f.write(f"/* {os.path.basename(src)} 에서 구운 적도 전개도 {W}x{H} RGB565.\n")
        f.write(f" * tools/make-orb-texture.py 가 만든다 — 손으로 고치지 말 것. */\n")
        f.write("#pragma once\n#include <stdint.h>\n\n")
        f.write(f"static const uint16_t {name}[{W * H}] = {{\n")
        vals = []
        for y in range(H):
            for x in range(W):
                r, g, b = px[x, y]
                vals.append(f"0x{((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3):04X}")
        for i in range(0, len(vals), 12):
            f.write("    " + ",".join(vals[i:i+12]) + ",\n")
        f.write("};\n")
    print(f"✓ {out}  ({W*H*2//1024}KB)")
    print("  이제 idf.py build 하면 orb.c 가 알아서 이 사진을 쓴다.")

main()
