#!/usr/bin/env python3
"""Generate mask.png — 900x900 grayscale circular mask with feathered edge."""
import math
from PIL import Image, ImageFilter

S = 900
R = S / 2.0

img = Image.new("L", (S, S), 0)
for y in range(S):
    for x in range(S):
        dx = x - R + 0.5
        dy = y - R + 0.5
        d = math.sqrt(dx * dx + dy * dy)
        if d <= R - 3:
            v = 255
        elif d >= R + 3:
            v = 0
        else:
            t = (d - (R - 3)) / 6.0
            v = int(255 * (1.0 - t * t * t))
        img.putpixel((x, y), v)

img = img.filter(ImageFilter.GaussianBlur(radius=4))
img.save("mask.png")
print("Created mask.png (900x900 grayscale)")
