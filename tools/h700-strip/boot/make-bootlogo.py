#!/usr/bin/env python3
# h700 boot logo = the OFFICIAL MinUI logo (skeleton/SYSTEM/tg5040/dat/bootlogo.bmp, the exact
# MIN/UI mark the Brick/Smart Pro use) composited centered on the h700's 640x480 boot screen.
# Same logo as every other MinUI variant — no fork branding.
from PIL import Image
import os

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
logo = Image.open(os.path.join(REPO, "skeleton/SYSTEM/tg5040/dat/bootlogo.bmp")).convert("RGB")

W, H = 640, 480
canvas = Image.new("RGB", (W, H), (0, 0, 0))
scale = 1.6
lw, lh = int(logo.width * scale), int(logo.height * scale)
logo = logo.resize((lw, lh), Image.LANCZOS)
canvas.paste(logo, ((W - lw) // 2, (H - lh) // 2))

out = os.path.join(os.path.dirname(__file__), "bootlogo.bmp")
canvas.save(out)
print("saved", out, Image.open(out).size)
