"""Turn hi-res sprite art into Build tiles: cut out, scale, match to the game palette, write PNGs + a .def.

  python tools/mksprite.py            # everything in art/sprites.json
  python tools/mksprite.py zombie     # just one entry

Reads  art/sprites.json (see its _comment), the source images it names, and the palette (for now from Duke's
       PALETTE.DAT in devdata/; the colours are what make the "Build look").
Writes game/sprites/<name>_<group>.png (every pixel already an exact palette colour, so the engine's own
       colour matching can't shift it) and game/sprites.def (tilefromtexture lines; included by emine32.def).

Source images: either RGBA with real transparency (used as is), or a figure on a plain flat background, which is
keyed out (the background colour is taken from the image border; soft grey shadows of it go too).
Colours are matched in CIELAB rather than RGB: Duke's palette has few mid greens, and plain RGB matching turns
desaturated greens grey, while LAB keeps them green (olive/khaki).
"""
import json, os, struct, sys
import numpy as np
from scipy import ndimage
from PIL import Image

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
GRP = os.path.join(ROOT, 'devdata', 'duke3d-sw', 'DUKE3D.GRP')
OUT = os.path.join(ROOT, 'game', 'sprites')
DEF = os.path.join(ROOT, 'game', 'sprites.def')


def grp_file(path, want):
    with open(path, 'rb') as f:
        assert f.read(12) == b'KenSilverman', path
        n = struct.unpack('<I', f.read(4))[0]
        ents = [(f.read(12).rstrip(b'\0').decode(), struct.unpack('<I', f.read(4))[0]) for _ in range(n)]
        off = 16 + 16 * n
        for name, sz in ents:
            if name.upper() == want:
                f.seek(off)
                return f.read(sz)
            off += sz
    raise FileNotFoundError(want)


def load_palette():
    """The 255 usable colours (255 is transparent), as the engine sees them (6-bit values << 2), and which of
    them are fullbright (identical in every shade table row: they'd glow in the dark, so sprites avoid them)."""
    d = grp_file(GRP, 'PALETTE.DAT')
    pal = np.frombuffer(d[:768], np.uint8).reshape(256, 3).astype(np.int32) << 2
    numshades = struct.unpack('<h', d[768:770])[0]
    shades = np.frombuffer(d[770:770 + numshades * 256], np.uint8).reshape(numshades, 256)
    fullbright = np.all(shades == shades[0], axis=0) & (pal.sum(1) > 0)
    usable = [i for i in range(255) if not fullbright[i]]
    return pal, np.array(usable)


def to_lab(rgb):
    c = rgb / 255.0
    c = np.where(c > 0.04045, ((c + 0.055) / 1.055) ** 2.4, c / 12.92)
    m = np.array([[0.4124, 0.3576, 0.1805], [0.2126, 0.7152, 0.0722], [0.0193, 0.1192, 0.9505]])
    xyz = c @ m.T / np.array([0.9505, 1.0, 1.089])
    f = np.where(xyz > 0.008856, np.cbrt(xyz), 7.787 * xyz + 16 / 116)
    return np.stack([116 * f[..., 1] - 16, 500 * (f[..., 0] - f[..., 1]), 200 * (f[..., 1] - f[..., 2])], -1)


def cut_out(im, bg_tol=40, shadow_depth=60, max_lum=None):
    """RGBA with the flat background (and its soft shadows) made transparent, cropped to the figure.
    max_lum: also drop pale, greyish pixels brighter than this (glow/haze around a dark figure).
    Specks (bits much smaller than the figure) are dropped too."""
    if im.mode == 'RGBA' and np.asarray(im)[..., 3].min() < 128:
        out = im
    else:
        a = np.asarray(im.convert('RGB')).astype(np.int32)
        border = np.concatenate([a[0], a[-1], a[:, 0], a[:, -1]])
        bg = np.median(border, axis=0)
        sat = a.max(2) - a.min(2)
        lum = a.mean(2)
        isbg = (np.abs(a - bg).sum(2) < bg_tol) | ((sat < 16) & (lum > bg.mean() - shadow_depth) & (lum < bg.mean() + 20))
        if max_lum is not None:
            isbg |= (lum > max_lum) & (sat < 70)
        keep = ~isbg
        # drop specks: connected bits under 1% of the biggest one
        lab, n = ndimage.label(keep)
        if n > 1:
            sizes = ndimage.sum(keep, lab, range(1, n + 1))
            keep = np.isin(lab, 1 + np.nonzero(sizes >= sizes.max() * 0.01)[0])
        out = Image.fromarray(np.dstack([a, keep * 255]).astype(np.uint8), 'RGBA')
    return out.crop(out.getchannel('A').point(lambda v: 255 if v >= 128 else 0).getbbox())


def quantise(im, pal, usable, lab_pal):
    """RGBA image -> RGBA image whose opaque pixels are exact palette colours; alpha is 0 or 255."""
    a = np.asarray(im).astype(np.float64)
    lab = to_lab(a[..., :3]).reshape(-1, 1, 3)
    best = np.empty(lab.shape[0], np.int64)
    for s in range(0, lab.shape[0], 4096):  # chunked: pixels x 255 colours
        best[s:s + 4096] = ((lab[s:s + 4096] - lab_pal[None]) ** 2).sum(2).argmin(1)
    rgb = pal[usable[best]].reshape(a.shape[0], a.shape[1], 3)
    alpha = np.where(a[..., 3] >= 128, 255, 0)
    return Image.fromarray(np.dstack([rgb, alpha]).astype(np.uint8), 'RGBA')


def make_group(cut, g):
    h = g['height']
    if g.get('squash'):
        # keep the width it has at full height (a crouch is shorter, not thinner)
        full = g.get('fullheight', 78)
        w = max(1, round(cut.width * full / cut.height))
        img = cut.resize((w, h), Image.LANCZOS)
    else:
        w = max(1, round(cut.width * h / cut.height))
        img = cut.resize((w, h), Image.LANCZOS)
    if g.get('flip'):
        img = img.transpose(Image.FLIP_TOP_BOTTOM)
    if g.get('rotate'):
        img = img.rotate(-g['rotate'], resample=Image.BICUBIC, expand=True)
        img = img.crop(img.getchannel('A').point(lambda v: 255 if v >= 128 else 0).getbbox())
    return img


def main(only):
    spec = json.load(open(os.path.join(ROOT, 'art', 'sprites.json')))
    pal, usable = load_palette()
    lab_pal = to_lab(pal[usable].astype(np.float64))
    os.makedirs(OUT, exist_ok=True)

    # keep other entries' lines when building just one
    lines = {}
    if only and os.path.exists(DEF):
        cur = None
        for ln in open(DEF).read().splitlines()[1:]:
            if ln.startswith('// '):
                cur = ln[3:].split(' ')[0]
            lines.setdefault(cur, []).append(ln)

    for name, ent in spec.items():
        if name.startswith('_') or (only and name not in only):
            continue
        cut = cut_out(Image.open(os.path.join(ROOT, ent['source'])), ent.get('bg_tol', 40), ent.get('shadow_depth', 60), ent.get('max_lum'))
        out = ['// %s (from %s, replaces %s)' % (name, ent['source'], ent.get('replaces', '-'))]
        for gi, g in enumerate(ent['groups']):
            img = quantise(make_group(cut, g), pal, usable, lab_pal)
            fn = '%s_%d.png' % (name, gi)
            img.save(os.path.join(OUT, fn))
            t0, t1 = g['tiles']
            for t in range(t0, t1 + 1):
                out.append('tilefromtexture %d { file "sprites/%s" xoffset 0 yoffset 0 }' % (t, fn))
            print('%s: tiles %d-%d <- %s %dx%d' % (name, t0, t1, fn, img.width, img.height))
        lines[name] = out

    with open(DEF, 'w') as f:
        f.write('// eMine32 sprite tiles (generated by tools/mksprite.py from art/sprites.json)\n')
        for name in lines:
            f.write('\n'.join(lines[name]) + '\n')


if __name__ == '__main__':
    main(set(sys.argv[1:]))
