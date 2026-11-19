"""The page background's blocks: art/page/fal/tiles.png (fal's 3x3 sheet, prompt in art/page/fal_prompts.json) cut
into its nine squares, each shrunk to TILE pixels (drawn at twice that on the page: chunky, like the game) ->
web/tiles.png, one row. scripts/build-web.sh puts it into the page as a data URL.
Order: ammo, TNT, nuclear drum, bricks, pipes, hazard stripes, atomic health, slime, vent."""
import os
import numpy as np
from PIL import Image

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
TILE = 24

im = Image.open(os.path.join(ROOT, 'art', 'page', 'fal', 'tiles.png')).convert('RGB')
a = np.asarray(im).astype(int)
W, H = im.size
out = Image.new('RGB', (TILE * 9, TILE))
for k in range(9):
    i, j = k % 3, k // 3
    x0, y0, x1, y1 = i * W // 3, j * H // 3, (i + 1) * W // 3, (j + 1) * H // 3
    cell = a[y0:y1, x0:x1]
    on = cell.max(2) > 40                      # inside the black gutters
    rows, cols = np.nonzero(on.any(1))[0], np.nonzero(on.any(0))[0]
    box = (x0 + cols[0], y0 + rows[0], x0 + cols[-1] + 1, y0 + rows[-1] + 1)
    side = min(box[2] - box[0], box[3] - box[1])
    cx, cy = (box[0] + box[2]) // 2, (box[1] + box[3]) // 2
    sq = im.crop((cx - side // 2, cy - side // 2, cx - side // 2 + side, cy - side // 2 + side))
    out.paste(sq.resize((TILE, TILE), Image.LANCZOS), (k * TILE, 0))
dst = os.path.join(ROOT, 'web', 'tiles.png')
out.save(dst, optimize=True)
print('web/tiles.png', out.size, os.path.getsize(dst), 'bytes')
