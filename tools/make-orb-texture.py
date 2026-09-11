#!/usr/bin/env python3
"""Bakes a planet map in from a photograph.

orb.c draws every planet in code when no map is present — convincing for the
Moon, less so for the Earth, whose continents are simply not Earth's. A real
photograph replaces that. Fetch an equirectangular one and run this.

  python3 tools/make-orb-texture.py moon    ~/Downloads/moon_8k.jpg
  python3 tools/make-orb-texture.py earth   ~/Downloads/blue_marble.jpg
  python3 tools/make-orb-texture.py sun     ~/Downloads/sun.jpg
  python3 tools/make-orb-texture.py jupiter ~/Downloads/jupiter.jpg

The four maps that ship in main/assets/ came from NASA, and NOTICE lists them
with their credits:
  moon    https://svs.gsfc.nasa.gov/4720        lroc_color_poles_2k.tif
  earth   https://visibleearth.nasa.gov/images/73909
                                                world.topo.bathy.200412.3x5400x2700.jpg
  jupiter https://photojournal.jpl.nasa.gov/catalog/PIA07782   PIA07782.jpg
  sun     https://svs.gsfc.nasa.gov/30362/      euvi_aia304_2012_carrington.tif

🚨 Take what you bake from a source whose licence you can name. NASA content is
   generally not copyrighted, but NASA also hosts other people's work and marks
   it with their name in the caption — read the caption. Textures that float
   around the 3D world (three.js examples, threex.planets and the rest) mostly
   trace back to planetpixelemporium, which is somebody's copyrighted work used
   with permission, and that permission is not yours.

The picture has to span 360 degrees of longitude across and 180 of latitude
down. A half-height picture, or one with the poles cropped, smears at the poles.

The result goes to main/assets/orb_tex_<name>.h and orb.c picks it up on its own.
That is 256 KB of flash each (512x256 RGB565).
"""
import sys, os
from PIL import Image, ImageEnhance

W, H = 512, 256          # has to match TEX_W / TEX_H in orb.c

# 🚨 A photograph taken through a telescope is not a texture yet. Blue Marble
# is dark, because the real Earth is dark; on a 1.75-inch panel at 45%
# brightness it reads as a black ball with a green smear.
#
# Raising the gamma alone was tried first and the answer was that it now looked
# washed out — gamma lifts the dark areas toward grey and pulls the colour out
# with them. The fix is a smaller gamma lift, then contrast, then saturation.
#
# These live here, not in a hand-edit afterwards. The header used to say it had
# been "adjusted after baking", which meant nobody could reproduce it.
ADJUST = {                  # gamma, contrast, saturation
    "earth":   (0.62, 1.22, 1.60),
    "moon":    (0.90, 1.10, 1.00),   # LROC is nearly grey; a little contrast is all
    "sun":     (1.00, 1.00, 1.00),   # 304 Angstrom is already vivid
    "jupiter": (0.88, 1.12, 1.18),
}

def adjust(im, kind):
    g, c, sat = ADJUST.get(kind, (1.0, 1.0, 1.0))
    if g != 1.0:
        lut = [min(255, int(((i / 255.0) ** g) * 255 + 0.5)) for i in range(256)]
        im = im.point(lut * 3)
    if c != 1.0:
        im = ImageEnhance.Contrast(im).enhance(c)
    if sat != 1.0:
        im = ImageEnhance.Color(im).enhance(sat)
    return im

def main():
    if len(sys.argv) != 3 or sys.argv[1] not in ("moon", "earth", "sun", "jupiter"):
        print(__doc__); sys.exit(1)
    kind, src = sys.argv[1], sys.argv[2]
    im = Image.open(src).convert("RGB")
    if abs(im.width / im.height - 2.0) > 0.15:
        print(f"⚠ the aspect is {im.width}x{im.height}. An equirectangular map has to be 2:1.")
        print("  Going ahead anyway will smear at the poles.")
    im = im.resize((W, H), Image.LANCZOS)
    im = adjust(im, kind)
    px = im.load()

    name = f"ORB_TEX_{kind.upper()}"
    out = os.path.join(os.path.dirname(__file__), "..", "main", "assets",
                       f"orb_tex_{kind}.h")
    with open(out, "w") as f:
        f.write(f"/* An equirectangular {W}x{H} RGB565 map, baked from {os.path.basename(src)}.\n")
        g, c, sat = ADJUST.get(kind, (1.0, 1.0, 1.0))
        f.write(f" * Made by tools/make-orb-texture.py — do not edit by hand.\n")
        f.write(f" * gamma {g}, contrast {c}, saturation {sat} (tools/make-orb-texture.py ADJUST). */\n")
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
