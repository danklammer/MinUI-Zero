from PIL import Image, ImageDraw, ImageFont

W, H = 640, 480
img = Image.new("RGB", (W, H), (0, 0, 0))  # black, MinUI's minimalist ground
d = ImageDraw.Draw(img)

FONT = "/Users/dk/Sites/MinUI/skeleton/SYSTEM/res/BPreplayBold-unhinted.otf"
big = ImageFont.truetype(FONT, 64)
small = ImageFont.truetype(FONT, 22)

def center(text, font, y, fill):
    b = d.textbbox((0, 0), text, font=font)
    w = b[2] - b[0]
    d.text(((W - w) // 2, y), text, font=font, fill=fill)

# wordmark: "MinUI" white, "Zero" in a cool accent so the fork reads at a glance
title = "MinUI Zero"
b = d.textbbox((0, 0), title, font=big)
tw = b[2] - b[0]
x = (W - tw) // 2
y = 190
# draw "MinUI " white then "Zero" accented
minui = "MinUI "
d.text((x, y), minui, font=big, fill=(255, 255, 255))
mw = d.textbbox((0, 0), minui, font=big)[2]
d.text((x + mw, y), "Zero", font=big, fill=(120, 200, 255))

# tagline in muted grey — the thesis, understated
center("lowest clock that holds the frame", small, y + 90, (120, 120, 128))

# thin accent rule under the wordmark
d.rectangle([(W // 2 - 140, y + 78), (W // 2 + 140, y + 80)], fill=(40, 60, 80))

img.save("/private/tmp/claude-501/-Users-dk-Sites-MinUI/bcd06d6b-2cc9-417f-83ee-02bf03bfc472/scratchpad/bootlogo.bmp")
# verify 24-bit
im2 = Image.open("/private/tmp/claude-501/-Users-dk-Sites-MinUI/bcd06d6b-2cc9-417f-83ee-02bf03bfc472/scratchpad/bootlogo.bmp")
print("saved", im2.size, im2.mode)
