#!/usr/bin/env python3
"""Re-bakes the home wallpaper at whatever brightness is wanted.

An AMOLED draws current for the pixels that are lit — a black pixel is not lit
at all. So holding the wallpaper down genuinely cuts what the home screen
costs. It could be dimmed in the calculation at draw time, but then saving
power would cost more CPU. Baking it into the file is the right answer.

  python3 tools/dim-wallpaper.py 0.6    # 60% brightness (what it is now)
  python3 tools/dim-wallpaper.py 1.0    # back to the original

🚨 Running it again on an already-dimmed file dims it that much further. Getting
   back means fetching from git, or multiplying by the reciprocal of the factor.
"""
import re, sys, os

K = float(sys.argv[1]) if len(sys.argv) > 1 else 0.6
p = os.path.join(os.path.dirname(__file__), "..", "main", "assets", "home_bg.c")
src = open(p).read()
m = re.search(r'home_bg_map\[\]\s*=\s*\{(.*?)\n\};', src, re.S)
vals = re.findall(r'0x([0-9a-fA-F]{2})', m.group(1))
out = []
for i in range(0, len(vals), 2):
    c = (int(vals[i+1], 16) << 8) | int(vals[i], 16)      # little-endian RGB565
    r = int(((c >> 11) & 0x1F) * K)
    g = int(((c >> 5) & 0x3F) * K)
    b = int((c & 0x1F) * K)
    c2 = (r << 11) | (g << 5) | b
    out.append(f"0x{c2 & 0xFF:02x}, 0x{(c2 >> 8) & 0xFF:02x},")
lines = ["    " + " ".join(out[i:i+12]) for i in range(0, len(out), 12)]
open(p, "w").write(src[:m.start(1)] + "\n" + "\n".join(lines) + src[m.end(1):])
print(f"✓ wallpaper baked at {K*100:.0f}% brightness ({len(vals)//2} pixels)")
