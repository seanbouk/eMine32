"""Title screen: art/title/title.png (4:3) -> game/textures/title.art, which replaces Duke's BETASCREEN tile (2493);
and the game's name flying in over it, from art/title/logo.jpg ("SHIFT" over "HAPPENS" on black) -> logo_shift.art
and logo_happens.art, replacing DUKENUKEM (2497) and THREEDEE (2498). Run: python tools/mktitle.py

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
LOGO = os.path.join(ROOT, 'art', 'title', 'logo.jpg')
# Each word: its band of rows in logo.jpg, the tile it replaces, where its letters should land on the 320x200 screen
# (yellow face width, centre y; from the layout mockup) and where Duke's title code draws that tile (centre y; both
# finish at zoom 15/16, screens.cpp). The tile is padded so the letters sit at their centre, not the tile's.
WORDS = [('shift', (258, 1011), 2497, 151, 92.5, 104), ('happens', (1046, 1531), 2498, 142, 130, 129)]
ZOOM = 60 / 64
PIXEL_ASPECT = 1.2  # 320x200 shown at 4:3: pixels are 1.2 times taller than wide


def title_palette():
    sys.path.insert(0, HERE)
    import mksprite
    d = mksprite.grp_file(mksprite.GRP, 'LOOKUP.DAT')
    n = d[0]
    base = 1 + n * 257 + 2 * 768  # after the lookups: water, slime, then the title palette
    return np.frombuffer(d[base:base + 768], np.uint8).reshape(256, 3).astype(np.int32) << 2


def write_art(path, tile, idx):
    """One-tile ART file: version, tile count, first, last; x size, y size, anim word; pixels column by column."""
    h, w = idx.shape
    with open(path, 'wb') as f:
        f.write(struct.pack('<4i', 1, 1, tile, tile))
        f.write(struct.pack('<hhI', w, h, 0))
        f.write(idx.T.tobytes())


def to_palette(rgb, pal):
    # 255 is see-through in Build, and Duke's shade table sends 239 to another colour even at full brightness (it
    # was made for the game palette, where the two look alike): give both a copy of 254, so they're never chosen
    usable = pal.copy()
    usable[239] = usable[255] = pal[254]
    p = Image.new('P', (1, 1))
    p.putpalette(usable.astype(np.uint8).flatten().tolist())
    idx = np.asarray(rgb.quantize(palette=p, dither=Image.Dither.FLOYDSTEINBERG), np.uint8)
    return np.where((idx == 239) | (idx == 255), 254, idx).astype(np.uint8)


def logo_word(src, band, face_w, want_y, drawn_y, pal):
    from scipy import ndimage
    a = np.asarray(src, np.float32)[band[0]:band[1] + 1]
    lum = a.mean(2)
    alpha = ndimage.binary_closing(lum > 18, np.ones((5, 5)))
    ys, xs = np.nonzero(alpha)
    a, alpha = a[ys.min():ys.max() + 1, xs.min():xs.max() + 1], alpha[ys.min():ys.max() + 1, xs.min():xs.max() + 1]
    # scale so the yellow faces come out face_w wide on screen, keeping the letters' shape on tall pixels
    yel = (a[..., 0] > 150) & (a[..., 1] > 110) & (a[..., 2] < 110)
    fx = np.nonzero(yel.sum(0) > 2)[0]
    s = face_w / ZOOM / (fx.max() - fx.min() + 1)
    w, h = round(a.shape[1] * s), round(a.shape[0] * s / PIXEL_ASPECT)
    rgb = Image.fromarray(a.astype(np.uint8)).resize((w, h), Image.LANCZOS)
    al = np.asarray(Image.fromarray((alpha * 255).astype(np.uint8)).resize((w, h), Image.LANCZOS)) >= 128
    idx = to_palette(rgb, pal)
    idx = np.where(al, idx, 255).astype(np.uint8)
    # pad: the tile's centre is drawn at drawn_y; the letters' centre should be at want_y
    pad = round(2 * (drawn_y - want_y) / ZOOM)
    blank = np.full((abs(pad), w), 255, np.uint8)
    return np.vstack([idx, blank] if pad > 0 else [blank, idx])


def main():
    pal = title_palette()
    im = Image.open(SRC).convert('RGB').resize((320, 200), Image.LANCZOS)  # 4:3 shown with tall pixels
    idx = to_palette(im, pal)
    assert idx.max() < 255
    write_art(OUT, TILE, idx)
    print(OUT)

    # the name, and a preview of the finished screen (the words at their final size and place)
    screen = idx.copy()
    logo = Image.open(LOGO).convert('RGB')
    for name, band, tile, face_w, want_y, drawn_y in WORDS:
        word = logo_word(logo, band, face_w, want_y, drawn_y, pal)
        path = os.path.join(ROOT, 'game', 'textures', 'logo_%s.art' % name)
        write_art(path, tile, word)
        print(path, '%dx%d' % (word.shape[1], word.shape[0]))
        big = Image.fromarray(word).resize((round(word.shape[1] * ZOOM), round(word.shape[0] * ZOOM)), Image.NEAREST)
        wz = np.asarray(big)
        y0, x0 = round(drawn_y - wz.shape[0] / 2), round(160 - wz.shape[1] / 2)
        region = screen[y0:y0 + wz.shape[0], x0:x0 + wz.shape[1]]
        region[...] = np.where(wz != 255, wz, region)
    Image.fromarray(pal[screen].astype(np.uint8)).resize((640, 480), Image.NEAREST).save(PREVIEW)


if __name__ == '__main__':
    main()
