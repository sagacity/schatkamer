#!/usr/bin/env python3
"""launcher_art.py OUT_DIR -- draws the player's icon0.png (512x512) and splash pic1.png (1920x1080)."""
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

FONTS = Path(__file__).resolve().parent.parent / 'third_party' / 'fonts'
BACK = (11, 13, 18)
INK = (242, 244, 255)
ACCENT = (255, 179, 71)


def wordmark(draw, cx, cy, size):
    font = ImageFont.truetype(str(FONTS / 'Inter-SemiBold.ttf'), size)
    left, right = 'Schat', 'kamer'
    wl = draw.textlength(left, font=font)
    wr = draw.textlength(right, font=font)
    x = cx - (wl + wr) / 2
    draw.text((x, cy), left, font=font, fill=INK, anchor='ls')
    draw.text((x + wl, cy), right, font=font, fill=ACCENT, anchor='ls')


def play_mark(draw, cx, cy, r):
    draw.ellipse((cx - r, cy - r, cx + r, cy + r), outline=ACCENT, width=max(4, r // 12))
    t = r * 0.42
    draw.polygon([(cx - t * 0.7, cy - t), (cx - t * 0.7, cy + t), (cx + t, cy)], fill=ACCENT)


out = Path(sys.argv[1])
icon = Image.new('RGB', (512, 512), BACK)
d = ImageDraw.Draw(icon)
play_mark(d, 256, 220, 110)
wordmark(d, 256, 420, 64)
icon.save(out / 'icon0.png')

splash = Image.new('RGB', (1920, 1080), BACK)
d = ImageDraw.Draw(splash)
play_mark(d, 960, 440, 120)
wordmark(d, 960, 680, 96)
splash.save(out / 'pic1.png')
print('wrote', out / 'icon0.png', out / 'pic1.png')
