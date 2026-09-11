#!/usr/bin/env python3
"""PNG/SVG → LVGL 9 C 배열.

pypng 의존을 피하려고 직접 굽는다. 지정 초기화자를 쓰니 비트필드 순서는 신경 안 써도 된다.
  RGB565     불투명 배경
  RGB565A8   알파가 필요한 것 (바늘 등) — RGB565 블록 뒤에 A8 블록이 붙는다
  A8         단색 아이콘. 색은 LVGL 쪽에서 recolor 로 입힌다
"""
import os, sys, subprocess, math
from PIL import Image, ImageDraw

# 🚨 예전엔 홈서버 경로가 그대로 박혀 있어서 다른 기계에선 아예 못 돌았다
# (0909 회사 윈도우 PC 에서 걸림). 레포 기준으로 잡고, 밖에 있는 것만
# 환경변수로 받는다. 없으면 그 부분만 건너뛴다 — 도구가 통째로 죽지 않게.
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(REPO, "main", "assets")
SHOTS = os.path.join(REPO, "sim", "shots")

# 시계 다이얼 원본은 이 레포 밖에 있다(watchface-s5 작업물).
# 없으면 시계 자산은 건너뛴다.


def rgb565(px):
    r, g, b = px[0] >> 3, px[1] >> 2, px[2] >> 3
    return (r << 11) | (g << 5) | b


ICON = 120                                  # home icon side, in pixels
ART = os.path.join(os.path.dirname(os.path.abspath(__file__)), "art")

BAYER = [[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]


def emit(name, img, fmt, dither=False):
    """fmt: 'RGB565' | 'RGB565A8' | 'A8'"""
    w, h = img.size
    body = bytearray()
    if fmt == "A8":
        a = img.convert("RGBA").split()[3]
        body += a.tobytes()
        stride, cf = w, "LV_COLOR_FORMAT_A8"
    else:
        rgba = img.convert("RGBA")
        px = rgba.load()
        for y in range(h):
            for x in range(w):
                p = px[x, y]
                if dither:
                    # 5/6비트로 자를 때 버려지는 하위 비트만큼 미리 흔들어준다
                    t = BAYER[y & 3][x & 3]
                    def cl(v):          # 아래로도 넘칠 수 있다. 양끝을 다 막는다
                        return 0 if v < 0 else (255 if v > 255 else v)
                    p = (cl(p[0] + (t - 8) // 2),
                         cl(p[1] + (t - 8) // 4),
                         cl(p[2] + (t - 8) // 2), p[3])
                v = rgb565(p)
                body += bytes((v & 0xFF, v >> 8))       # 리틀엔디안
        stride = w * 2
        cf = "LV_COLOR_FORMAT_RGB565"
        if fmt == "RGB565A8":
            body += rgba.split()[3].tobytes()
            cf = "LV_COLOR_FORMAT_RGB565A8"

    lines = [f"/* 자동 생성 — tools/mkassets.py. 손으로 고치지 말 것. */",
             '#include "lvgl.h"', "",
             f"static const uint8_t {name}_map[] = {{"]
    for i in range(0, len(body), 16):
        lines.append("    " + "".join("0x%02X," % b for b in body[i:i + 16]))
    lines.append("};")
    lines.append(f"""
const lv_image_dsc_t {name} = {{
    .header = {{
        .magic  = LV_IMAGE_HEADER_MAGIC,
        .cf     = {cf},
        .flags  = 0,
        .w      = {w},
        .h      = {h},
        .stride = {stride},
    }},
    .data_size = sizeof({name}_map),
    .data      = {name}_map,
}};""")
    path = os.path.join(OUT, f"{name}.c")
    open(path, "w").write("\n".join(lines) + "\n")
    print(f"{name:14} {w}x{h:<4} {fmt:9} {len(body)//1024:5}KB")
    return w, h


def svg(name, w, h):
    out = f"/tmp/art_{name}.png"
    subprocess.run(["rsvg-convert", "-w", str(w), "-h", str(h),
                    f"{ART}/{name}.svg", "-o", out], check=True)
    return Image.open(out).convert("RGBA")


def circle_mask(size):
    m = Image.new("L", (size * 4, size * 4), 0)
    ImageDraw.Draw(m).ellipse((0, 0, size * 4 - 1, size * 4 - 1), fill=255)
    return m.resize((size, size), Image.LANCZOS)


def disc(color):
    """가장자리가 매끈한 원판"""
    im = Image.new("RGBA", (ICON, ICON), color + (255,))
    im.putalpha(circle_mask(ICON))
    return im


def clock_icon():
    """A plain analog clock, drawn four times over and shrunk down."""
    SS = 8
    n = ICON * SS
    im = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    pad = 2 * SS
    d.ellipse([pad, pad, n - pad, n - pad], fill="#F2F0EA",
              outline="#C9C5BA", width=3 * SS)

    c = n / 2
    for i in range(12):
        a = math.radians(i * 30 - 90)
        r0 = c - (12 if i % 3 == 0 else 9) * SS
        r1 = c - 20 * SS
        d.line([c + math.cos(a) * r0, c + math.sin(a) * r0,
                c + math.cos(a) * r1, c + math.sin(a) * r1],
               fill="#1B1D22" if i % 3 == 0 else "#8A8780",
               width=(3 if i % 3 == 0 else 2) * SS)

    def hand(deg, length, width, colour):
        r = math.radians(deg - 90)
        # A short tail past the centre is what makes a hand read as a hand.
        d.line([c - math.cos(r) * length * 0.18, c - math.sin(r) * length * 0.18,
                c + math.cos(r) * length,        c + math.sin(r) * length],
               fill=colour, width=width)

    hand(10 * 30 + 9 * 0.5, c * 0.50, 6 * SS, "#1B1D22")   # hour
    hand(9 * 6,             c * 0.72, 4 * SS, "#1B1D22")   # minute
    hand(30 * 6,            c * 0.76, 2 * SS, "#C8453A")   # second
    d.ellipse([c - 4 * SS, c - 4 * SS, c + 4 * SS, c + 4 * SS], fill="#1B1D22")

    im = im.resize((ICON, ICON), Image.LANCZOS)
    im.putalpha(circle_mask(ICON))
    return im


def make_app_icons():
    """앱 아이콘은 글리프보다 앱 자체를 축소해 보여주는 쪽이 알아보기 쉽다."""
    # 마우스 — 커서가 지나간 잔상. 뒤로 갈수록 흐려진다.
    m = disc((0x4E, 0x8C, 0xF5))
    cur = svg("cursor", 54, 54)
    # 잔상이 원 밖으로 나가면 잘려서 지저분하다. 안쪽으로 모은다.
    ghosts = [(-24, -24, 55, 40), (-16, -16, 95, 46), (-8, -8, 150, 50)]
    for dx, dy, a, sz in ghosts:
        g = svg("cursor", sz, sz)
        g.putalpha(g.split()[3].point(lambda v: v * a // 255))
        m.alpha_composite(g, (48 + dx, 46 + dy))
    m.alpha_composite(cur, (48, 46))
    emit("app_icon_mouse", m, "RGB565A8")

    # Clock. Drawn here rather than shrunk from a screenshot: the app's own
    # face is a digital panel, and a tiny one reads as a smudge at 120 px.
    # Hands sit at 10:09:30 for the reason watch advertising does it — the two
    # hands make a symmetric V and leave the middle of the dial clear.
    emit("app_icon_clock", clock_icon(), "RGB565A8")

    emit("icon_gear", gear_icon(40), "A8")

    # 키 — 키보드를 확 당겨서 자판 몇 개만 보이는 그림
    S4 = ICON * 4
    k = Image.new("RGBA", (S4, S4), (0x15, 0x18, 0x20, 255))
    dk = ImageDraw.Draw(k)
    cap, gap = S4 * 0.255, S4 * 0.045
    cols, rows_n = 3, 2
    tw = cols * cap + (cols - 1) * gap
    th = rows_n * cap + (rows_n - 1) * gap
    ox, oy = (S4 - tw) / 2, (S4 - th) / 2
    for r in range(rows_n):
        for c in range(cols):
            x = ox + c * (cap + gap)
            y = oy + r * (cap + gap)
            hot = (r == 1 and c == 1)                 # 가운데 아래 한 알만 강조
            top  = (0x8A, 0xB4, 0xF8) if hot else (0xDF, 0xE4, 0xEA)
            side = (0x4E, 0x7C, 0xC0) if hot else (0x9A, 0xA2, 0xAE)
            rad = S4 * 0.035
            # 아래쪽에 살짝 두꺼운 몸통을 먼저 그려 입체로 보이게
            dk.rounded_rectangle((x, y + S4 * 0.018, x + cap, y + cap), radius=rad, fill=side + (255,))
            dk.rounded_rectangle((x, y, x + cap, y + cap - S4 * 0.022), radius=rad, fill=top + (255,))
    k = k.resize((ICON, ICON), Image.LANCZOS)
    k.putalpha(circle_mask(ICON))
    emit("app_icon_keys", k, "RGB565A8")

    # 타이핑기 — 점 세 개 (가려진 비밀번호)
    ty = disc((0x5A, 0x6E, 0x86))
    dt = ImageDraw.Draw(ty)
    for i in range(3):
        cx = ICON / 2 + (i - 1) * 24
        dt.ellipse((cx - 8, ICON / 2 - 8, cx + 8, ICON / 2 + 8), fill=(255, 255, 255, 255))
    emit("app_icon_type", ty, "RGB565A8")

    # 프레젠터 — 슬라이드 한 장과 화살표
    pr = disc((0x3E, 0x8E, 0x78))
    dp = ImageDraw.Draw(pr)
    dp.rounded_rectangle((26, 34, 82, 74), radius=5, outline=(255, 255, 255, 255), width=5)
    dp.polygon([(64, 88), (98, 88), (81, 104)], fill=(255, 255, 255, 255))
    emit("app_icon_present", pr, "RGB565A8")

    # 계산기 — 등호와 더하기
    ca = disc((0xC8, 0x8E, 0x33))
    dc2 = ImageDraw.Draw(ca)
    dc2.rounded_rectangle((30, 44, 90, 52), radius=4, fill=(255, 255, 255, 255))
    dc2.rounded_rectangle((30, 66, 90, 74), radius=4, fill=(255, 255, 255, 255))
    emit("app_icon_calc", ca, "RGB565A8")

    # 게임 — 지금 들어 있는 게임(벽돌깨기) 그대로. 벽돌·공·곡선 판.
    S4 = ICON * 4
    gm = Image.new("RGBA", (S4, S4), (0x12, 0x16, 0x20, 255))
    dg = ImageDraw.Draw(gm)
    rows = [(0x7F, 0xB0, 0xFF), (0x5B, 0xD4, 0x8A), (0xE0, 0xA3, 0x3A)]
    bw, bh, gap = S4 * 0.20, S4 * 0.072, S4 * 0.022
    for r, col in enumerate(rows):
        n = 3 if r == 0 else 4
        total = n * bw + (n - 1) * gap
        x = (S4 - total) / 2
        y = S4 * 0.17 + r * (bh + gap)
        for k in range(n):
            dg.rounded_rectangle((x, y, x + bw, y + bh), radius=S4 * 0.014, fill=col + (255,))
            x += bw + gap
    # 공
    cx, cy, rr = S4 * 0.5, S4 * 0.62, S4 * 0.05
    dg.ellipse((cx - rr, cy - rr, cx + rr, cy + rr), fill=(255, 255, 255, 255))
    # 곡선 판 — 아래쪽 호
    pad = S4 * 0.13
    dg.arc((pad, pad, S4 - pad, S4 - pad), 58, 122,
           fill=(255, 255, 255, 255), width=int(S4 * 0.055))
    gm = gm.resize((ICON, ICON), Image.LANCZOS)
    gm.putalpha(circle_mask(ICON))
    emit("app_icon_games", gm, "RGB565A8")


    # 회의 — 마이크. 폰이 녹음하는 거지만 손이 찾는 그림은 마이크다.
    S5 = ICON * 4
    mt = Image.new("RGBA", (S5, S5), (0x18, 0x10, 0x14, 255))
    dm = ImageDraw.Draw(mt)
    # 캡슐
    cw, ch = S5 * 0.20, S5 * 0.38
    cx0, cy0 = (S5 - cw) / 2, S5 * 0.17
    dm.rounded_rectangle((cx0, cy0, cx0 + cw, cy0 + ch), radius=cw / 2,
                         fill=(0xFF, 0x5B, 0x5B, 255))
    # 받침 호 + 대
    pad = S5 * 0.27
    dm.arc((pad, S5 * 0.24, S5 - pad, S5 * 0.74), 20, 160,
           fill=(255, 255, 255, 255), width=int(S5 * 0.045))
    dm.rounded_rectangle((S5 * 0.485, S5 * 0.68, S5 * 0.515, S5 * 0.82),
                         radius=S5 * 0.015, fill=(255, 255, 255, 255))
    mt = mt.resize((ICON, ICON), Image.LANCZOS)
    mt.putalpha(circle_mask(ICON))
    emit("app_icon_meet", mt, "RGB565A8")

    # 물·달·지구 — 화면을 그대로 줄인다. 다른 아이콘과 같은 방식이다.
    # 🚨 시뮬 화면을 미리 찍어둬야 한다: sim/shots/icon_{water,moon,earth}.png
    for nm, crop in (("water", None), ("moon", None), ("earth", None)):
        f = os.path.join(SHOTS, f"icon_{nm}.png")
        if not os.path.exists(f):
            print(f"  ⚠ {f} 가 없다 — 아이콘 건너뜀")
            continue
        im = Image.open(f).convert("RGB")
        if crop: im = im.crop(crop)
        im = im.resize((ICON, ICON), Image.LANCZOS).convert("RGBA")
        im.putalpha(circle_mask(ICON))
        emit(f"app_icon_{nm}", im, "RGB565A8")

    # 타이머 — 줄어드는 링 그 자체
    S = ICON * 4
    tm = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    dr = ImageDraw.Draw(tm)
    dr.ellipse((0, 0, S - 1, S - 1), fill=(0x14, 0x17, 0x1D, 255))
    pad, wdt = S * 0.17, int(S * 0.075)
    dr.arc((pad, pad, S - pad, S - pad), -90, 270, fill=(0x24, 0x26, 0x2C, 255), width=wdt)
    dr.arc((pad, pad, S - pad, S - pad), -90, 155, fill=(0x7F, 0xB0, 0xFF, 255), width=wdt)
    tm = tm.resize((ICON, ICON), Image.LANCZOS)
    tm.putalpha(circle_mask(ICON))
    emit("app_icon_timer", tm, "RGB565A8")


def gear_icon(size):
    """톱니는 SVG 패스로 그리면 모양이 잘 안 잡힌다. 이가 8개인 걸 그대로 계산해서 그린다."""
    S = size * 4
    im = Image.new("RGBA", (S, S), (255, 255, 255, 0))
    d = ImageDraw.Draw(im)
    c = S / 2
    r_body, r_tooth, r_hole = S * 0.30, S * 0.42, S * 0.135
    half = math.radians(11)

    for i in range(8):
        a = math.radians(i * 45)
        pts = []
        for sign, rad in ((-1, r_body * 0.98), (1, r_body * 0.98)):
            pass
        # 사다리꼴 이 하나: 안쪽 넓고 바깥쪽 살짝 좁게
        for ang, rad in ((a - half * 1.35, r_body * 0.95), (a - half, r_tooth),
                         (a + half, r_tooth), (a + half * 1.35, r_body * 0.95)):
            pts.append((c + math.sin(ang) * rad, c - math.cos(ang) * rad))
        d.polygon(pts, fill=(255, 255, 255, 255))

    d.ellipse((c - r_body, c - r_body, c + r_body, c + r_body), fill=(255, 255, 255, 255))
    d.ellipse((c - r_hole, c - r_hole, c + r_hole, c + r_hole), fill=(255, 255, 255, 0))
    return im.resize((size, size), Image.LANCZOS)


def main():
    """Bakes what this repo actually ships: the app icons and the gear glyph.

    It used to bake two watch dials and a wallpaper as well. Those were other
    people's artwork, so they are gone — the clock is drawn in code now
    (main/lcdface.c) and the home background is a gradient."""
    os.makedirs(OUT, exist_ok=True)
    defs = []

    make_app_icons()

    hdr = ["/* generated by tools/mkassets.py */", "#pragma once",
           '#include "lvgl.h"', ""]
    for n in ("app_icon_mouse", "app_icon_clock", "app_icon_timer",
              "app_icon_keys", "app_icon_type", "app_icon_present",
              "app_icon_calc", "app_icon_games", "app_icon_meet",
              "app_icon_water", "app_icon_moon", "app_icon_earth",
              "icon_gear"):
        hdr.append(f"extern const lv_image_dsc_t {n};")
    hdr += [""] + defs + [""]
    open(os.path.join(OUT, "assets.h"), "w").write("\n".join(hdr) + "\n")
    print("assets.h 작성")
    subprocess.run([sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), "sync_cmake.py")], check=True)



if __name__ == "__main__":
    main()

