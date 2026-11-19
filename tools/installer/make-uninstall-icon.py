# Draws assets/uninstall.ico: the icon of the "Uninstall Fluid Wallpaper" shortcut, in the family of
# assets/skip-next.ico (a rounded square with a diagonal gradient and a white glyph with a soft shadow), but red to
# orange with a bin, so it cannot be mistaken for the on-off shortcut (the app's own swirl) or "next".
#   python tools/installer/make-uninstall-icon.py [out.ico]      (needs Pillow)
import sys
from PIL import Image, ImageDraw, ImageFilter

N = 1024
out = sys.argv[1] if len(sys.argv) > 1 else 'assets/uninstall.ico'

# diagonal gradient, top-left red -> bottom-right orange
c0, c1 = (214, 48, 74), (247, 148, 40)
grad = Image.new('RGB', (N, N))
px = grad.load()
for y in range(N):
    for x in range(N):
        t = (x + y) / (2.0 * (N - 1))
        px[x, y] = tuple(int(round(c0[k] + (c1[k] - c0[k]) * t)) for k in range(3))
plate = Image.new('L', (N, N), 0)
ImageDraw.Draw(plate).rounded_rectangle((36, 36, N - 36, N - 36), radius=190, fill=255)

# the bin, as a mask: handle, lid, body with three slots
g = Image.new('L', (N, N), 0)
d = ImageDraw.Draw(g)
d.rounded_rectangle((428, 232, 596, 316), radius=34, fill=255)        # handle (outer)
d.rounded_rectangle((466, 268, 558, 330), radius=14, fill=0)          # handle (hole)
d.rounded_rectangle((268, 300, 756, 372), radius=30, fill=255)        # lid
d.polygon([(318, 404), (706, 404), (676, 770), (348, 770)], fill=255)  # body
d.rounded_rectangle((346, 700, 678, 800), radius=44, fill=255)        # rounded bottom
for cx in (426, 512, 598):                                            # slots
    d.rounded_rectangle((cx - 19, 466, cx + 19, 724), radius=19, fill=0)

icon = Image.new('RGBA', (N, N), (0, 0, 0, 0))
icon.paste(grad, (0, 0), plate)
shadow = Image.new('RGBA', (N, N), (0, 0, 0, 0))
shadow.paste((60, 20, 30, 110), (14, 16), g)                           # soft shadow down-right, as on skip-next
shadow = shadow.filter(ImageFilter.GaussianBlur(3))
icon = Image.alpha_composite(icon, Image.composite(shadow, Image.new('RGBA', (N, N), (0, 0, 0, 0)), plate))
white = Image.new('RGBA', (N, N), (255, 255, 255, 255))
icon.paste(white, (0, 0), g)
icon.save(out, sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
print('wrote', out)
