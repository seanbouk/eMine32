"""Title screen: art/title/title.png (4:3) -> game/textures/title.art, which replaces Duke's BETASCREEN tile (2493).
Run: python tools/mktitle.py

Made exactly like the one it replaces: 320x200 (Duke's 4:3 screen, tall pixels), in Duke's title palette (TITLEPAL,
from LOOKUP.DAT), so it shows the same way: same resolution, same palette, black bars on wider screens. It's an ART
file rather than a PNG because the engine converts PNG tiles to the game palette, not the title one.
"""
import os, struct, sys
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, '..')
SRC = os.path.join(ROOT, 'art', 'title', 'title.png')
OUT = os.path.join(ROOT, 'game', 'textures', 'title.art')
PREVIEW = os.path.join(ROOT, 'art', 'title', 'title_320x200.png')  # what the game shows, for looking at
TILE = 2493  # BETASCREEN


def title_palette():
    sys.path.insert(0, HERE)
    import mksprite
    d = mksprite.grp_file(mksprite.GRP, 'LOOKUP.DAT')
    n = d[0]
    base = 1 + n * 257 + 2 * 768  # after the lookups: water, slime, then the title palette
    return np.frombuffer(d[base:base + 768], np.uint8).reshape(256, 3).astype(np.int32) << 2


def main():
    pal = title_palette()
    im = Image.open(SRC).convert('RGB').resize((320, 200), Image.LANCZOS)  # 4:3 shown with tall pixels
    p = Image.new('P', (1, 1))
    flat = pal[:255].astype(np.uint8).flatten().tolist()  # 255 is see-through in Build
    p.putpalette(flat + flat[-3:])
    q = im.quantize(palette=p, dither=Image.Dither.FLOYDSTEINBERG)
    idx = np.asarray(q, np.uint8)
    assert idx.max() < 255
    # ART: version, tile count, first, last; x sizes, y sizes, anim words; pixels column by column
    with open(OUT, 'wb') as f:
        f.write(struct.pack('<4i', 1, 1, TILE, TILE))
        f.write(struct.pack('<h', 320) + struct.pack('<h', 200) + struct.pack('<I', 0))
        f.write(idx.T.tobytes())
    Image.fromarray(pal[idx].astype(np.uint8)).resize((640, 480), Image.NEAREST).save(PREVIEW)
    print(OUT)


if __name__ == '__main__':
    main()
