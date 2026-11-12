"""Our fonts, rendered from free (SIL Open Font Licence) fonts into the tiles Duke's fonts used.
Run: python tools/mkfont.py   -> game/textures/font_*.art (loaded by game/emine32.def) + art/fonts/preview.png

  menu font (BIGALPHANUM, the big red one) and its grey twin, HUD digits (DIGITALNUM): Anton
  small font (STARTALPHANUM: messages, the smaller menu text): Press Start 2P
  mini font (MINIFONT): Silkscreen
The fonts and their licences are in art/fonts/; the licences ship with the game.

Each glyph keeps its tile's height (Duke draws text from the top-left corner, so letters share a baseline by
sitting at the bottom of same-height tiles), and is coloured only with the palette entries the original font
used: the game recolours text by remapping those entries (menu highlights, coloured messages), so they must stay.
Shading is ours: lit from the top, darker toward the bottom, with a one-pixel shadow to the right and below.
"""
import os, struct, sys
import numpy as np
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, '..')
FONTS = os.path.join(ROOT, 'art', 'fonts')
OUT = os.path.join(ROOT, 'game', 'textures')
ANTON = os.path.join(FONTS, 'anton', 'Anton-Regular.ttf')
PRESS = os.path.join(FONTS, 'pressstart2p', 'PressStart2P-Regular.ttf')
SILK = os.path.join(FONTS, 'silkscreen', 'Silkscreen-Regular.ttf')

# tile numbers (duke3d/src/dnames.h)
BIGALPHANUM, BIGPERIOD, BIGAPPOS, STARTALPHANUM, MINIFONT, DIGITALNUM = 2940, 3002, 3022, 2822, 3072, 2472
ASCII = [chr(c) for c in range(ord('!'), ord('~') + 1)]  # STARTALPHANUM / MINIFONT: one tile each, from '!'


def grp_tiles():
    sys.path.insert(0, HERE)
    import mksprite
    cache = {}

    def tile(n):
        f = 'TILES%03d.ART' % (n // 256)
        if f not in cache:
            d = mksprite.grp_file(mksprite.GRP, f)
            _, _, t0, t1 = struct.unpack('<4i', d[:16])
            k = t1 - t0 + 1
            xs = struct.unpack('<%dh' % k, d[16:16 + 2 * k])
            ys = struct.unpack('<%dh' % k, d[16 + 2 * k:16 + 4 * k])
            pa = struct.unpack('<%dI' % k, d[16 + 4 * k:16 + 8 * k])
            off, tiles = 16 + 8 * k, {}
            for i in range(k):
                sz = xs[i] * ys[i]
                px = np.frombuffer(d[off:off + sz], np.uint8).reshape(xs[i], ys[i]).T if sz else None
                tiles[t0 + i] = (px, pa[i])
                off += sz
            cache[f] = tiles
        return cache[f][n]
    pal = np.frombuffer(mksprite.grp_file(mksprite.GRP, 'PALETTE.DAT')[:768], np.uint8).reshape(256, 3).astype(np.int32) << 2
    return tile, pal


def ramp(tile, pal, tiles):
    """The palette entries the original glyphs used, darkest first, with their brightness 0..1 within the set."""
    counts = np.zeros(256)
    for t in tiles:
        px = tile(t)[0]
        if px is not None:
            counts += np.bincount(px.ravel(), minlength=256)
    counts[255] = 0
    # the colours it's mostly made of (a font's few odd edge pixels can be another hue: red letters with grey rims)
    used = set(np.nonzero(counts >= counts.sum() * 0.015)[0].tolist())
    idx = np.array(sorted(used, key=lambda i: pal[i] @ [30, 59, 11]))
    lum = pal[idx] @ np.array([30, 59, 11])
    return idx, (lum - lum.min()) / max(1, lum.max() - lum.min())


def colour(level, alpha, rmp):
    """Level image (0 dark .. 1 bright) -> palette indices from the ramp; 255 where alpha is off."""
    idx, lum = rmp
    pick = np.abs(level[..., None] - lum[None, None, :]).argmin(2)
    return np.where(alpha, idx[pick], 255).astype(np.uint8)


def shade(mask, top=0.95, bottom=0.45):
    """Face lit from the top, a 1-pixel shadow right and below (level 0). Returns level, alpha (one pixel bigger)."""
    h, w = mask.shape
    face = np.zeros((h + 1, w + 1), bool)
    face[:h, :w] = mask
    shadow = np.zeros_like(face)
    shadow[1:, 1:] = face[:-1, :-1]
    shadow &= ~face
    y = np.arange(h + 1)[:, None] / max(1, h - 1)
    level = np.where(face, top + (bottom - top) * y, 0.0)
    return level, face | shadow


def outline_glyph(path, ch, cap):
    """Anton glyph with capitals `cap` pixels tall: a boolean mask, the full cap height plus anything below the
    baseline (comma) or above it; returned with the row the cap line sits on."""
    big = 200
    f = ImageFont.truetype(path, big)
    asc, _ = f.getmetrics()
    def raster(c):
        im = Image.new('L', (big * 2, big * 2))
        ImageDraw.Draw(im).text((big // 2, big // 4), c, font=f, fill=255)
        return np.asarray(im, np.float32) / 255
    href = raster('H')
    ys = np.nonzero(href.max(1) > 0.5)[0]
    cap_top, base = ys.min(), ys.max() + 1
    s = cap / (base - cap_top)
    a = raster(ch)
    cols = np.nonzero(a.max(0) > 0.5)[0]
    rows = np.nonzero(a.max(1) > 0.5)[0]
    if not len(cols):
        return None
    y0, y1 = min(rows.min(), cap_top), max(rows.max() + 1, base)
    a = a[y0:y1, cols.min():cols.max() + 1]
    w, h = max(1, round(a.shape[1] * s)), max(1, round(a.shape[0] * s))
    small = np.asarray(Image.fromarray((a * 255).astype(np.uint8)).resize((w, h), Image.LANCZOS), np.float32) / 255
    return small > 0.45, round((cap_top - y0) * s)


def pixel_glyph(path, size, ch):
    """A pixel font at its native size, unsmoothed; returned with the row of its cap line."""
    f = ImageFont.truetype(path, size)
    def raster(c):
        im = Image.new('L', (size * 4, size * 4))
        d = ImageDraw.Draw(im)
        d.fontmode = '1'
        d.text((size, size), c, font=f, fill=255)
        return np.asarray(im) > 0
    ys = np.nonzero(raster('H').any(1))[0]
    cap_top = ys.min()
    a = raster(ch)
    if not a.any():
        return None
    rows, cols = np.nonzero(a.any(1))[0], np.nonzero(a.any(0))[0]
    y0 = min(rows.min(), cap_top)
    return a[y0:rows.max() + 1, cols.min():cols.max() + 1], cap_top - y0


def place(mask_top, height):
    """Glyph mask + its cap-line row -> a tile `height` tall (at least) with the cap line at the top."""
    mask, top = mask_top
    out = np.zeros((max(height, mask.shape[0] - top), mask.shape[1]), bool)
    rows = mask[top:]
    out[:rows.shape[0]] = rows
    return out


def write_art(path, first, tiles):
    """ART file for tiles first.. (consecutive): (pixels or None, picanm word) each."""
    with open(path, 'wb') as f:
        f.write(struct.pack('<4i', 1, len(tiles), first, first + len(tiles) - 1))
        for px, _ in tiles: f.write(struct.pack('<h', 0 if px is None else px.shape[1]))
        for px, _ in tiles: f.write(struct.pack('<h', 0 if px is None else px.shape[0]))
        for _, pa in tiles: f.write(struct.pack('<I', pa))
        for px, _ in tiles:
            if px is not None: f.write(np.ascontiguousarray(px.T).tobytes())
    print('%s: tiles %d-%d' % (os.path.relpath(path, ROOT), first, first + len(tiles) - 1))


def offsets(px, pa_orig):
    """No animation, offsets 0: text is drawn from the top-left corner, where offsets don't apply."""
    return 0


def main():
    tile, pal = grp_tiles()
    sheet = []  # for the preview

    # --- menu font: digits and '-' below BIGALPHANUM, red A-Z, grey A-Z and digits, red punctuation
    red_tiles = list(range(BIGALPHANUM - 11, BIGALPHANUM + 26)) + list(range(BIGPERIOD, BIGPERIOD + 8))
    grey_tiles = list(range(BIGALPHANUM + 26, BIGALPHANUM + 62))
    red, grey = ramp(tile, pal, red_tiles), ramp(tile, pal, grey_tiles)
    big = {}
    for c in '-0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ':
        big[(c, 'red')] = (c, 15, red)
    for c in 'ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789':
        big[(c, 'grey')] = (c, 16, grey)
    layout = {BIGALPHANUM - 11: ('-', 'red')}
    for i, c in enumerate('0123456789'): layout[BIGALPHANUM - 10 + i] = (c, 'red')
    for i in range(26): layout[BIGALPHANUM + i] = (chr(65 + i), 'red')
    for i in range(26): layout[BIGALPHANUM + 26 + i] = (chr(65 + i), 'grey')
    for i, c in enumerate('0123456789'): layout[BIGALPHANUM + 52 + i] = (c, 'grey')
    for i, c in enumerate('.,!?;:/%'): layout[BIGPERIOD + i] = (c, 'red')
    tiles = []
    for t in range(BIGALPHANUM - 11, BIGPERIOD + 8):
        c, kind = layout[t]
        h, rmp = (15, red) if kind == 'red' else (16, grey)
        g = outline_glyph(ANTON, c, h - 1)
        m = place(g, h - 1)
        level, alpha = shade(m)
        px = colour(level, alpha, rmp)
        tiles.append((px, offsets(px, tile(t)[1])))
    write_art(os.path.join(OUT, 'font_big.art'), BIGALPHANUM - 11, tiles)
    sheet.append([p for p, _ in tiles])
    g = outline_glyph(ANTON, "'", 14)
    level, alpha = shade(place(g, 14))
    apos = colour(level, alpha, red)
    write_art(os.path.join(OUT, 'font_bigappos.art'), BIGAPPOS, [(apos, offsets(apos, tile(BIGAPPOS)[1]))])

    # --- small font: '!'..'~', capitals for lowercase (as Duke's)
    small_tiles = list(range(STARTALPHANUM, STARTALPHANUM + len(ASCII)))
    rmp = ramp(tile, pal, small_tiles)
    tiles = []
    for t, c in zip(small_tiles, ASCII):
        g = pixel_glyph(PRESS, 8, c.upper() if 'a' <= c <= 'z' else c)
        if g is None:
            tiles.append((None, 0)); continue
        level, alpha = shade(place(g, 7), 1.0, 0.55)
        px = colour(level, alpha, rmp)
        tiles.append((px, offsets(px, tile(t)[1])))
    write_art(os.path.join(OUT, 'font_small.art'), STARTALPHANUM, tiles)
    sheet.append([p for p, _ in tiles if p is not None][:64])

    # --- mini font: '!'..'~' (lowercase filled in too: the game uses it when present)
    mini_tiles = list(range(MINIFONT, MINIFONT + len(ASCII)))
    rmp = ramp(tile, pal, mini_tiles)
    tiles = []
    for t, c in zip(mini_tiles, ASCII):
        g = pixel_glyph(SILK, 8, c)
        if g is None:
            tiles.append((None, 0)); continue
        m = place(g, 5)
        px = colour(np.where(m, 0.85, 0.0), m, rmp)  # flat: too small to shade
        tiles.append((px, offsets(px, tile(t)[1])))
    write_art(os.path.join(OUT, 'font_mini.art'), MINIFONT, tiles)
    sheet.append([p for p, _ in tiles if p is not None][:64])

    # --- HUD digits
    rmp = ramp(tile, pal, range(DIGITALNUM, DIGITALNUM + 10))
    tiles = []
    for i in range(10):
        level, alpha = shade(place(outline_glyph(ANTON, str(i), 9), 9), 1.0, 0.5)
        px = colour(level, alpha, rmp)
        tiles.append((px, offsets(px, tile(DIGITALNUM + i)[1])))
    write_art(os.path.join(OUT, 'font_digits.art'), DIGITALNUM, tiles)
    sheet.append([p for p, _ in tiles])

    # preview: each font in a row, 3x
    W, rows = 900, []
    for glyphs in sheet:
        x, h, row = 0, max(p.shape[0] for p in glyphs), []
        for p in glyphs:
            row.append((x, p)); x += p.shape[1] + 1
        rows.append((h, row))
    img = np.full((sum(h + 4 for h, _ in rows) + 4, W, 3), (40, 40, 60), np.uint8)
    y = 4
    for h, row in rows:
        for x, p in row:
            if x + p.shape[1] >= W: break
            rgb = pal[p].astype(np.uint8)
            region = img[y:y + p.shape[0], x + 4:x + 4 + p.shape[1]]
            region[p != 255] = rgb[p != 255]
        y += h + 4
    Image.fromarray(img).resize((W * 3, img.shape[0] * 3), Image.NEAREST).save(os.path.join(FONTS, 'preview.png'))


if __name__ == '__main__':
    main()
