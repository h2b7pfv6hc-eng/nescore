# Builds a contact sheet of the text area of screen-only test results.
import glob, os, sys
from PIL import Image, ImageDraw
src, out = sys.argv[1], sys.argv[2]
fs = sorted(glob.glob(src + "/*.ppm"))
cols = 4
rows = (len(fs) + cols - 1) // cols
sheet = Image.new("RGB", (cols * 256, rows * 112), "white")
d = ImageDraw.Draw(sheet)
for i, f in enumerate(fs):
    im = Image.open(f)
    bbox = im.point(lambda v: 255 if v > 40 else 0).getbbox() or (0, 0, 256, 100)
    top = max(0, min(bbox[1] - 4, 240 - 100))
    x, y = (i % cols) * 256, (i // cols) * 112
    sheet.paste(im.crop((0, top, 256, top + 100)), (x, y + 12))
    d.text((x + 2, y), os.path.basename(f)[:40], fill="black")
sheet.save(out)
