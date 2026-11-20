"""Pictures for itch.io's own page round the game (its theme and the launch frame), so it matches ours:
 art/page/itch-background.png: the block wall (web/tiles.png, as the page draws it, dimmed the same), seamless, to
   tile as the theme's background image.
 art/page/itch-launch.png: the DOS start screen, 960x720 (made by tools/webtest/dos_shot.js from the real page)."""
import os, random
from PIL import Image

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
SRC, CELL, KINDS, DIM, N = 24, 48, 7, 0.68, 12
tiles = Image.open(os.path.join(ROOT, 'web', 'tiles.png')).convert('RGB')
random.seed(7)
im = Image.new('RGB', (CELL * N, CELL * N))
for y in range(N):
    for x in range(N):
        k = random.randrange(KINDS)
        im.paste(tiles.crop((k * SRC, 0, (k + 1) * SRC, SRC)).resize((CELL, CELL), Image.NEAREST), (x * CELL, y * CELL))
im = Image.blend(im, Image.new('RGB', im.size, (12, 11, 10)), DIM)
out = os.path.join(ROOT, 'art', 'page', 'itch-background.png')
im.save(out, optimize=True)
print(os.path.relpath(out, ROOT), im.size)
