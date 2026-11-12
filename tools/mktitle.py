"""Title screen: art/title/title.png (4:3) -> game/textures/title.art, which replaces Duke's BETASCREEN tile (2493);
and the game's name flying in over it, from art/title/logo.jpg ("SHIFT" over "HAPPENS" on black) -> logo_shift.art
and logo_happens.art, replacing DUKENUKEM (2497) and THREEDEE (2498); and the menu backdrop, art/title/menu.jpg ->
menu.art over MENUSCREEN (2456), in the game palette (the menus use it). Run: python tools/mktitle.py

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
MENU = os.path.join(ROOT, 'art', 'title', 'menu.jpg')
MENUTILE = 2456  # MENUSCREEN
WIDE = os.path.join(ROOT, 'art', 'title', 'logo_wide.png')  # "SHIFT HAPPENS" on one line, on white
WIDETILE = 2499  # INGAMEDUKETHREEDEE: the name at the top of the menu you get in play (203x32)
WIDE_GAP = 3     # the gap between the two words, times the painting's
STUDIO = os.path.join(ROOT, 'art', 'title', 'studio.jpg')  # the studio screen before the title
STUDIOTILE = 2492  # DREALMS: 320x200, shown in its own palette (DREALMSPAL)
LOADING = os.path.join(ROOT, 'art', 'loading', 'loading.png')  # an aerial shot of the world (4:3)
LOADINGTILE = 3281  # LOADSCREEN: 320x200, game palette
LOADING_BLUR = 14   # Gaussian radius in the shot's pixels (1280 wide): the blocks melt away
LOADING_LOGO = (180, 162)  # the name's width and centre row on the 320x200 screen (the game writes "Loading" and
                           # the level name at rows 90-125, and a status line at 180)
MENUPREVIEW = os.path.join(ROOT, 'art', 'title', 'menu_320x200.png')
LOGO = os.path.join(ROOT, 'art', 'title', 'logo.jpg')
LOGO_WHITE = os.path.join(ROOT, 'art', 'title', 'logo_white.jpg')  # the same render on white: for the cut-out only
# Each word: its band of rows in logo.jpg, the tile it replaces, where its letters should land on the 320x200 screen
# (yellow face width, centre y; from the layout mockup) and where Duke's title code draws that tile (centre y; both
# finish at zoom 15/16, screens.cpp). The tile is padded so the letters sit at their centre, not the tile's.
WORDS = [('shift', (258, 1011), 2497, 151, 92.5, 104), ('happens', (1046, 1531), 2498, 142, 130, 129)]
ZOOM = 60 / 64
PIXEL_ASPECT = 1.2  # 320x200 shown at 4:3: pixels are 1.2 times taller than wide


def title_palette(block=2):
    """A palette from LOOKUP.DAT: after the lookup tables come water, slime, title (2), 3D Realms (3), ending."""
    sys.path.insert(0, HERE)
    import mksprite
    d = mksprite.grp_file(mksprite.GRP, 'LOOKUP.DAT')
    n = d[0]
    base = 1 + n * 257 + block * 768
    return np.frombuffer(d[base:base + 768], np.uint8).reshape(256, 3).astype(np.int32) << 2


def game_palette():
    """Duke's game palette, minus the fullbright colours (they'd stay lit when shaded)."""
    sys.path.insert(0, HERE)
    import mksprite
    d = mksprite.grp_file(mksprite.GRP, 'PALETTE.DAT')
    pal = np.frombuffer(d[:768], np.uint8).reshape(256, 3).astype(np.int32) << 2
    ns = struct.unpack('<h', d[768:770])[0]
    sh = np.frombuffer(d[770:770 + ns * 256], np.uint8).reshape(ns, 256)
    fb = np.all(sh == sh[0], axis=0)
    return pal, [i for i in range(255) if fb[i]]


def write_art(path, tile, idx):
    """One-tile ART file: version, tile count, first, last; x size, y size, anim word; pixels column by column."""
    h, w = idx.shape
    with open(path, 'wb') as f:
        f.write(struct.pack('<4i', 1, 1, tile, tile))
        f.write(struct.pack('<hhI', w, h, 0))
        f.write(idx.T.tobytes())


def to_palette(rgb, pal, avoid=()):
    # 255 is see-through in Build, and Duke's shade table sends 239 to another colour even at full brightness (it
    # was made for the game palette, where the two look alike): give both a copy of 254, so they're never chosen
    usable = pal.copy()
    avoid = {239, 255} | set(avoid)
    keep = next(i for i in range(254, -1, -1) if i not in avoid)
    for i in avoid: usable[i] = pal[keep]
    p = Image.new('P', (1, 1))
    p.putpalette(usable.astype(np.uint8).flatten().tolist())
    idx = np.asarray(rgb.quantize(palette=p, dither=Image.Dither.FLOYDSTEINBERG), np.uint8)
    return np.where(np.isin(idx, list(avoid)), keep, idx).astype(np.uint8)


def logo_mask(black, white):
    """Where the letters are. The two renders line up exactly; each fails somewhere on its own (dark letter sides
    vanish into the black, grey ones into the white's shadows), so: what's clearly letter on white (saturated, or
    dark), plus what's background in neither (the black one's cast-shadow areas are black, its glow is white on the
    white one). Then small enclosed holes (worn grey patches) are filled; the big ones are the counters of A, P..."""
    from scipy import ndimage
    W, B = np.asarray(white, np.float32), np.asarray(black, np.float32)
    lum, sat = W.mean(2), W.max(2) - W.min(2)
    m = (sat > 45) | (lum < 105) | (((lum < 200) | (sat > 25)) & (B.mean(2) > 14))
    m = ndimage.binary_opening(m, np.ones((3, 3)))
    lab, n = ndimage.label(m)
    m = np.isin(lab, 1 + np.nonzero(ndimage.sum(m, lab, range(1, n + 1)) > 2000)[0])
    m = ndimage.binary_closing(m, np.ones((5, 5)))
    holes = ndimage.binary_fill_holes(m) & ~m
    lab, n = ndimage.label(holes)
    return m | np.isin(lab, 1 + np.nonzero(ndimage.sum(holes, lab, range(1, n + 1)) < 4000)[0])


def logo_word(src, mask, band, face_w, want_y, drawn_y, pal):
    a = np.asarray(src, np.float32)[band[0]:band[1] + 1]
    alpha = mask[band[0]:band[1] + 1]
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


def wide_logo(width):
    """"SHIFT HAPPENS" on one line: (RGB image, alpha mask) `width` 320x200 pixels wide (on its tall pixels). Cut
    from the white render, coloured from the rock one, the gap between the words widened."""
    from scipy import ndimage
    a = np.asarray(Image.open(WIDE).convert('RGB'), np.float32)
    m = (a.mean(2) < 215) | (a.max(2) - a.min(2) > 40)
    # the shape from the white render, the colours from the rock one (the same render: white would fringe the edges)
    a = np.asarray(Image.open(MENU).convert('RGB').resize((a.shape[1], a.shape[0]), Image.LANCZOS), np.float32)
    m = ndimage.binary_opening(m, np.ones((2, 2)))
    lab, n = ndimage.label(m)
    m = np.isin(lab, 1 + np.nonzero(ndimage.sum(m, lab, range(1, n + 1)) > 200)[0])
    ys, xs = np.nonzero(m)
    a, m = a[ys.min():ys.max() + 1, xs.min():xs.max() + 1], m[ys.min():ys.max() + 1, xs.min():xs.max() + 1]
    empty = ~m.any(0)
    runs, x = [], 0  # the widest run of empty columns is the space between the words
    while x < len(empty):
        if empty[x]:
            x0 = x
            while x < len(empty) and empty[x]: x += 1
            runs.append((x - x0, x0, x))
        x += 1
    _, g0, g1 = max(runs)
    extra = (g1 - g0) * (WIDE_GAP - 1)
    a = np.concatenate([a[:, :g0], np.zeros((a.shape[0], extra, 3), np.float32), a[:, g0:]], 1)
    m = np.concatenate([m[:, :g0], np.zeros((m.shape[0], extra), bool), m[:, g0:]], 1)
    w = width
    h = round(a.shape[0] * width / a.shape[1] / PIXEL_ASPECT)
    rgb = Image.fromarray(a.astype(np.uint8)).resize((w, h), Image.LANCZOS)
    al = np.asarray(Image.fromarray((m * 255).astype(np.uint8)).resize((w, h), Image.LANCZOS)) >= 128
    return rgb, al


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
    mask = logo_mask(logo, Image.open(LOGO_WHITE).convert('RGB'))
    for name, band, tile, face_w, want_y, drawn_y in WORDS:
        word = logo_word(logo, mask, band, face_w, want_y, drawn_y, pal)
        path = os.path.join(ROOT, 'game', 'textures', 'logo_%s.art' % name)
        write_art(path, tile, word)
        print(path, '%dx%d' % (word.shape[1], word.shape[0]))
        big = Image.fromarray(word).resize((round(word.shape[1] * ZOOM), round(word.shape[0] * ZOOM)), Image.NEAREST)
        wz = np.asarray(big)
        y0, x0 = round(drawn_y - wz.shape[0] / 2), round(160 - wz.shape[1] / 2)
        region = screen[y0:y0 + wz.shape[0], x0:x0 + wz.shape[1]]
        region[...] = np.where(wz != 255, wz, region)
    Image.fromarray(pal[screen].astype(np.uint8)).resize((640, 480), Image.NEAREST).save(PREVIEW)

    # the menu backdrop
    gpal, fullbright = game_palette()

    # the name over the in-game menu, fitted to Duke's tile
    tw, th = 203, 32
    rgb, al = wide_logo(tw)
    w, h = rgb.size
    word = np.where(al, to_palette(rgb, gpal, fullbright), 255).astype(np.uint8)
    wide = np.full((th, tw), 255, np.uint8)
    y0, x0 = (th - h) // 2, (tw - w) // 2
    wide[y0:y0 + h, x0:x0 + w] = word
    path = os.path.join(ROOT, 'game', 'textures', 'logo_ingame.art')
    write_art(path, WIDETILE, wide)
    print(path, '%dx%d' % (w, h))
    prev = gpal[wide].astype(np.uint8)
    prev[wide == 255] = (40, 40, 40)
    Image.fromarray(prev).resize((tw * 3, round(th * 3 * PIXEL_ASPECT)), Image.NEAREST).save(
        os.path.join(ROOT, 'art', 'title', 'logo_ingame_preview.png'))
    menu = Image.open(MENU).convert('RGB').resize((320, 200), Image.LANCZOS)
    midx = to_palette(menu, gpal, fullbright)
    path = os.path.join(ROOT, 'game', 'textures', 'menu.art')
    write_art(path, MENUTILE, midx)
    print(path)
    Image.fromarray(gpal[midx].astype(np.uint8)).resize((640, 480), Image.NEAREST).save(MENUPREVIEW)

    # the loading screen: the world, blurred until it's just shapes and light, with the name low in the middle
    from PIL import ImageFilter
    shot = Image.open(LOADING).convert('RGB')
    shot = shot.filter(ImageFilter.GaussianBlur(LOADING_BLUR * shot.width / 1280)).resize((320, 200), Image.LANCZOS)
    lw, ly = LOADING_LOGO
    rgb, al = wide_logo(lw)
    x0, y0 = (320 - lw) // 2, ly - rgb.height // 2
    canvas = np.asarray(shot, np.float32).copy()
    region = canvas[y0:y0 + rgb.height, x0:x0 + lw]
    region[al] = np.asarray(rgb, np.float32)[al]
    lidx = to_palette(Image.fromarray(canvas.astype(np.uint8)), gpal, fullbright)
    path = os.path.join(ROOT, 'game', 'textures', 'loading.art')
    write_art(path, LOADINGTILE, lidx)
    print(path)
    Image.fromarray(gpal[lidx].astype(np.uint8)).resize((640, 480), Image.NEAREST).save(
        os.path.join(ROOT, 'art', 'loading', 'loading_320x200.png'))

    # the studio screen (in place of 3D Realms'), in its palette
    spal = title_palette(3)
    sidx = to_palette(Image.open(STUDIO).convert('RGB').resize((320, 200), Image.LANCZOS), spal)
    path = os.path.join(ROOT, 'game', 'textures', 'studio.art')
    write_art(path, STUDIOTILE, sidx)
    print(path)
    Image.fromarray(spal[sidx].astype(np.uint8)).resize((640, 480), Image.NEAREST).save(
        os.path.join(ROOT, 'art', 'title', 'studio_320x200.png'))


if __name__ == '__main__':
    main()
