#!/usr/bin/env python3
"""Bakes the moon and earth maps in from photographs.

The maps in the code today were made in code. The moon is convincing, but the
earth's continents are the wrong shape, so it is only an "earth-like planet".
For the real thing, fetch an equirectangular photograph and run this.

  python3 tools/make-orb-texture.py moon    ~/Downloads/moon_8k.jpg
  python3 tools/make-orb-texture.py earth   ~/Downloads/blue_marble.jpg
  python3 tools/make-orb-texture.py sun     ~/Downloads/sun.jpg
  python3 tools/make-orb-texture.py jupiter ~/Downloads/jupiter.jpg

Where to get them (public sources):
  moon   https://svs.gsfc.nasa.gov/4720   (NASA LROC, public domain)
  earth  https://visibleearth.nasa.gov/collection/1484/blue-marble
  both   https://www.solarsystemscope.com/textures/  (CC BY 4.0, from NASA data)
  used here: three.js (earth, moon) · homer-jay/solar-system-textures (sun)
             jeromeetienne/threex.planets (jupiter)

The picture has to span 360 degrees of longitude across and 180 of latitude
down. A half-height picture, or one with the poles cropped, smears at the poles.

The result goes to main/assets/orb_tex_<name>.h and orb.c picks it up on its own.
That is 256 KB of flash each (512x256 RGB565).
"""
import sys, os
from PIL import Image

W, H = 512, 256          # has to match TEX_W / TEX_H in orb.c

def main():
    if len(sys.argv) != 3 or sys.argv[1] not in ("moon", "earth", "sun", "jupiter"):
        print(__doc__); sys.exit(1)
    kind, src = sys.argv[1], sys.argv[2]
    im = Image.open(src).convert("RGB")
    if abs(im.width / im.height - 2.0) > 0.15:
        print(f"⚠ the aspect is {im.width}x{im.height}. An equirectangular map has to be 2:1.")
        print("  Going ahead anyway will smear at the poles.")
    im = im.resize((W, H), Image.LANCZOS)
    px = im.load()

    name = f"ORB_TEX_{kind.upper()}"
    out = os.path.join(os.path.dirname(__file__), "..", "main", "assets",
                       f"orb_tex_{kind}.h")
    with open(out, "w") as f:
        f.write(f"/* An equirectangular {W}x{H} RGB565 map, baked from {os.path.basename(src)}.\n")
        f.write(f" * Made by tools/make-orb-texture.py — do not edit by hand. */\n")
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
    print("  now run idf.py build and orb.c picks this picture up on its own.")

main()
