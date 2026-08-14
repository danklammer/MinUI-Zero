#!/usr/bin/env python3
# h700 boot screens, both 640x480x24 BMP on the p2 RESOURCE partition (mounted /mnt/boot):
#
#   bootlogo.bmp          — shown by the boot chain on a normal boot. The OFFICIAL MinUI logo
#                           (skeleton/SYSTEM/tg5040/dat/bootlogo.bmp, the exact MIN/UI mark the
#                           Brick/Smart Pro use), centered. Scale 0.8 = HALF the previous 1.6:
#                           the 1.6 render filled the panel edge-to-edge; half matches the OG
#                           MinUI proportions (workspace/_unmaintained/rg35xxplus/boot/
#                           bootlogo.bmp is a small centered mark) (Dan 2026-08-10).
#
#   bat/battery_charge.bmp — shown by the boot chain when powered on WHILE CHARGING. The stock
#                           file was the muOS gold badge ("THEMED BY Bitter Bizarro") straight
#                           from the donor dump — the mystery "muOS logo still shows sometimes"
#                           (Dan 2026-08-10). Replaced with a minimal battery outline in the
#                           same style as MinUI's own in-OS ChargingScreen: no branding, just
#                           a quiet white battery glyph on black.
from PIL import Image, ImageDraw
import os

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
W, H = 640, 480

# ---- bootlogo.bmp: official MinUI mark at half the old size ----
logo = Image.open(os.path.join(REPO, "skeleton/SYSTEM/tg5040/dat/bootlogo.bmp")).convert("RGB")
canvas = Image.new("RGB", (W, H), (0, 0, 0))
# 0.625 makes the mark occupy the SAME fraction of the screen as it does on the Brick, which is the
# only defensible target: 216px on the Brick 1024 panel = 21.1% of width, and 216*0.625 = 135px on
# this 640 panel = 21.1%. History: 1.6 filled the panel edge to edge, 0.8 (2026-08-10) halved it but
# still read ~28% oversized against the Brick because 640x480 is a much smaller canvas than
# 1024x768, so the same nominal scale eats proportionally more screen (Dan spotted it 2026-08-13).
scale = 0.625
lw, lh = int(logo.width * scale), int(logo.height * scale)
canvas.paste(logo.resize((lw, lh), Image.LANCZOS), ((W - lw) // 2, (H - lh) // 2))
out = os.path.join(HERE, "bootlogo.bmp")
canvas.save(out)
print("saved", out, Image.open(out).size)

# ---- bat/battery_charge.bmp: minimal MinUI-style battery outline ----
chg = Image.new("RGB", (W, H), (0, 0, 0))
d = ImageDraw.Draw(chg)
# vertical battery outline, centered — proportions echo MinUI's ChargingScreen glyph
bw, bh, r, t = 96, 168, 14, 8            # body w/h, corner radius, line thickness
cx, cy = W // 2, H // 2
x0, y0 = cx - bw // 2, cy - bh // 2 + 10  # nudge down to make room for the cap
x1, y1 = x0 + bw, y0 + bh
d.rounded_rectangle([x0, y0, x1, y1], radius=r, outline=(255, 255, 255), width=t)
# cap on top
capw, caph = 40, 16
d.rounded_rectangle([cx - capw // 2, y0 - caph - 4, cx + capw // 2, y0 - 4],
                    radius=5, fill=(255, 255, 255))
os.makedirs(os.path.join(HERE, "bat"), exist_ok=True)
out2 = os.path.join(HERE, "bat", "battery_charge.bmp")
chg.save(out2)
print("saved", out2, Image.open(out2).size)
