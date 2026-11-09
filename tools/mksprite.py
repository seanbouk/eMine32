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


def make_group(cut, g, ref_h=None):
    """One tile image from one view. Scale is set so the tallest view (ref_h) is `height` pixels tall, so views
    from one sheet keep their relative sizes."""
    ref_h = ref_h or cut.height
    h = g['height']
    # a squash keeps the width it has at full height (a crouch is shorter, not thinner)
    sx = (g.get('fullheight', 78) if g.get('squash') else h) / ref_h
    sy = h / ref_h
    img = cut.resize((max(1, round(cut.width * sx)), max(1, round(cut.height * sy))), Image.LANCZOS)
    if g.get('flip'):
        img = img.transpose(Image.FLIP_TOP_BOTTOM)
    if g.get('rotate'):
        img = img.rotate(-g['rotate'], resample=Image.BICUBIC, expand=True)
        img = img.crop(img.getchannel('A').point(lambda v: 255 if v >= 128 else 0).getbbox())
    return img


def split_sheet(cut, n):
    """A sheet of figures side by side -> the n biggest figures, left to right, each cropped.
    Figures are islands (2D), so they may overlap each other's columns (a figure falling over); small gaps inside
    a figure are bridged, and each panel keeps only its own island's pixels."""
    a = np.asarray(cut)
    solid = a[..., 3] >= 128
    lab, k = ndimage.label(ndimage.binary_dilation(solid, np.ones((9, 9))))
    sizes = ndimage.sum(solid, lab, range(1, k + 1))
    ids = 1 + np.argsort(sizes)[::-1][:n]
    if len(ids) < n:
        raise SystemExit('sheet: found %d figures, expected %d' % (len(ids), n))
    out = []
    for i in sorted(ids, key=lambda i: np.nonzero((lab == i).any(0))[0][0]):
        mine = solid & (lab == i)
        ys, xs = np.nonzero(mine)
        px = a.copy()
        px[..., 3] = np.where(mine, 255, 0)
        out.append(Image.fromarray(px[ys.min():ys.max() + 1, xs.min():xs.max() + 1], 'RGBA'))
    return out


def load_sheets(ent):
    """name -> list of panels. `sheets` names several source images; a plain `source` is the sheet "main"."""
    sheets = ent.get('sheets') or {'main': {k: ent[k] for k in ('source', 'views', 'mirror') if k in ent}}
    out = {}
    for sname, sh in sheets.items():
        cut = cut_out(Image.open(os.path.join(ROOT, sh['source'])), ent.get('bg_tol', 40), ent.get('shadow_depth', 60), ent.get('max_lum'))
        n = sh.get('count', sh.get('views'))
        panels = split_sheet(cut, n) if n else [cut]
        for i in sh.get('mirror', []):
            panels[i] = panels[i].transpose(Image.FLIP_LEFT_RIGHT)
        out[sname] = panels
    return out


def parse_ref(ref, default_sheet):
    """'side:2' = panel 2 of sheet "side"; a trailing ~ mirrors it; a bare number is a panel of the default sheet."""
    ref = str(ref)
    flip = ref.endswith('~')
    ref = ref.rstrip('~')
    sheet, _, idx = ref.rpartition(':')
    return sheet or default_sheet, int(idx), flip


def group_refs(g, nviews, default_sheet):
    """The image ref for each tile of a group, in tile order."""
    t0, t1 = g['tiles']
    n = t1 - t0 + 1
    if 'frames' in g:  # Duke's frame*views+view layout, one row of refs per frame (repeated to fill the range)
        flat = [r for row in g['frames'] for r in row]
        return [flat[k % len(flat)] for k in range(n)]
    if 'images' in g:  # one ref per tile
        return [g['images'][k % len(g['images'])] for k in range(n)]
    if g.get('per_view') and nviews > 1:
        return ['%s:%d' % (default_sheet, k % nviews) for k in range(n)]
    return [g.get('image', '%s:%d' % (default_sheet, g.get('view', 0)))] * n


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
        # a sheet's panels: for a turnaround, views 0..4 = front, 3/4 front, side, 3/4 back, back (Duke's turned
        # views face the viewer's left; `mirror` lists panels drawn facing the other way)
        sheets = load_sheets(ent)
        default = next(iter(sheets))
        ref_h = {s: max(p.height for p in ps) for s, ps in sheets.items()}  # each sheet's tallest figure = `height`
        src = ', '.join(sh['source'] for sh in ent['sheets'].values()) if 'sheets' in ent else ent['source']
        out = ['// %s (from %s, replaces %s)' % (name, src, ent.get('replaces', '-'))]
        for f in os.listdir(OUT):  # drop this entry's old outputs
            if f.startswith(name + '_'):
                os.remove(os.path.join(OUT, f))
        for gi, g in enumerate(ent['groups']):
            t0, t1 = g['tiles']
            refs = group_refs(g, len(sheets[default]), default)
            files = {}
            for ref in dict.fromkeys(refs):
                s, i, flip = parse_ref(ref, default)
                panel = sheets[s][i].transpose(Image.FLIP_LEFT_RIGHT) if flip else sheets[s][i]
                img = quantise(make_group(panel, g, ref_h[s]), pal, usable, lab_pal)
                fn = '%s_%d_%s%d%s.png' % (name, gi, s, i, 'm' if flip else '')
                img.save(os.path.join(OUT, fn))
                files[ref] = fn
                print('%s: tiles %d-%d <- %s %dx%d' % (name, t0, t1, fn, img.width, img.height))
            for k, ref in enumerate(refs):
                out.append('tilefromtexture %d { file "sprites/%s" xoffset 0 yoffset 0 }' % (t0 + k, files[ref]))
        lines[name] = out

    with open(DEF, 'w') as f:
        f.write('// eMine32 sprite tiles (generated by tools/mksprite.py from art/sprites.json)\n')
        for name in lines:
            f.write('\n'.join(lines[name]) + '\n')


if __name__ == '__main__':
    main(set(sys.argv[1:]))
