#!/usr/bin/env python3
"""Regenerate poxchat icons by applying a green glow to the source cauldron.

Source image: poxchat-cauldron-256.png (this directory)
Outputs (overwritten):
  ../poxchat-{48,64,128,256}.png
  ../poxchat.png
  ../poxchat.ico   (embedded sizes: 16, 32, 48, 64, 128, 256)

To revisit the design, edit GLOW_COLOR / BLUR_RADIUS / INTENSITY and re-run
from this directory:  python3 apply_glow.py
"""
from PIL import Image, ImageFilter
from pathlib import Path

HERE = Path(__file__).resolve().parent
SRC = HERE / "poxchat-cauldron-256.png"
OUT_DIR = HERE.parent

GLOW_COLOR = (140, 255, 140)   # mint/lime — matches the brew highlights
BLUR_RADIUS = 16               # measured in 256-px source pixels
INTENSITY = 1.8                # alpha multiplier on the blurred halo

def add_glow(img, color, blur_radius, intensity):
    alpha = img.split()[-1]
    glow_layer = Image.new("RGBA", img.size, color + (0,))
    glow_layer.putalpha(alpha)
    glow_layer = glow_layer.filter(ImageFilter.GaussianBlur(blur_radius))
    r, g, b, a = glow_layer.split()
    a = a.point(lambda v: min(255, int(v * intensity)))
    glow_layer = Image.merge("RGBA", (r, g, b, a))
    out = Image.new("RGBA", img.size, (0, 0, 0, 0))
    out = Image.alpha_composite(out, glow_layer)
    out = Image.alpha_composite(out, img)
    return out

src = Image.open(SRC).convert("RGBA")
glow_256 = add_glow(src, GLOW_COLOR, BLUR_RADIUS, INTENSITY)

# Downscale from the 256 glow so the halo stays proportionally consistent
for size in (48, 64, 128, 256):
    glow_256.resize((size, size), Image.LANCZOS).save(OUT_DIR / f"poxchat-{size}.png")
glow_256.save(OUT_DIR / "poxchat.png")
glow_256.save(OUT_DIR / "poxchat.ico", format="ICO",
              sizes=[(16, 16), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])

print(f"Updated icons in {OUT_DIR}")
