"""Our own PALETTE.DAT and LOOKUP.DAT: game/PALETTE.DAT, game/LOOKUP.DAT

Run: python tools/mkpalette.py            (writes both from art/palette/colours.json)
     python tools/mkpalette.py extract    (once: pulls the colour lists out of the shareware GRP into colours.json)
     python tools/mkpalette.py compare    (how close our generated tables come to the GRP's, for sanity)

We keep Duke's colours on purpose (part of the look, a nod to those who know): the 256-colour game palette and the
five whole-screen palettes (underwater, night vision, title, studio screen, ending). Those are lists of colours, kept in
art/palette/colours.json. Everything else is computed here from them, by our own rules, so the files we ship are ours:

  - shade table (32 shades): each colour faded towards black, matched to the nearest palette colour; the palette's
    fullbright colours (240-254, which glow in the dark) never fade.
  - translucency table: every pair of colours mixed, matched to the nearest colour.
  - palette swaps (LOOKUP.DAT): the whole-sprite tints the engine uses (1 blue: frozen, 2 red, 6 green: night vision,
    7 yellow, 8 slime green, 4 black: silhouettes), and the intro's coloured lights (10-14). Each colour keeps its brightness and takes the tint's hue. Duke's
    other swaps recoloured his enemies; they're cut, and an unknown swap draws in normal colours.

TABLES.DAT isn't needed: the engine computes its sine and arctangent tables and has its own fonts.
"""
import json, os, struct, sys
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, '..')
COLOURS = os.path.join(ROOT, 'art', 'palette', 'colours.json')
GRP = os.path.join(ROOT, 'devdata', 'duke3d-sw', 'DUKE3D.GRP')
OUT = os.path.join(ROOT, 'game')

NUMSHADES = 32
FULLBRIGHT = range(240, 255)       # never fade
BASEPALS = ['underwater', 'nightvision', 'title', 'studio', 'ending']  # LOOKUP.DAT's on-disk order
# palette swaps: pal number -> (the tint's colour at full brightness, brightness gain), or None for black
TINTS = {1: ((110, 140, 200), 1.5), 2: ((216, 64, 24), 2.5), 4: None, 6: ((150, 255, 110), 2.0),
         7: ((220, 180, 30), 1.25), 8: ((150, 200, 80), 2.0),
         # the intro's lights (tools/mkintro.py): sodium lamps, Mine Shift's glow, pink neon, cold street light, fire
         10: ((255, 165, 70), 1.6), 11: ((80, 255, 190), 1.7), 12: ((255, 90, 200), 1.7), 13: ((150, 180, 255), 1.4),
         14: ((255, 120, 45), 1.8)}


def grp_files():
    f = open(GRP, 'rb')
    assert f.read(12) == b'KenSilverman'
    n = struct.unpack('<i', f.read(4))[0]
    ents = [(f.read(12).rstrip(b'\0').decode(), struct.unpack('<i', f.read(4))[0]) for _ in range(n)]
    off, out = 16 + 16 * n, {}
    for name, size in ents:
        f.seek(off)
        out[name] = f.read(size)
        off += size
    return out


def extract():
    g = grp_files()
    pal = list(g['PALETTE.DAT'][:768])
    lk = g['LOOKUP.DAT']
    k = lk[0]
    base = lk[1 + k * 257:]
    d = {'game': pal}
    for i, name in enumerate(BASEPALS):
        d[name] = list(base[i * 768:(i + 1) * 768])
    os.makedirs(os.path.dirname(COLOURS), exist_ok=True)
    with open(COLOURS, 'w') as f:
        f.write('{\n' + ',\n'.join('  "%s": %s' % (n, json.dumps(v)) for n, v in d.items()) + '\n}\n')
    print('wrote', COLOURS)


def load():
    d = json.load(open(COLOURS))
    return {n: np.array(v, np.uint8).reshape(256, 3) for n, v in d.items()}


def nearest(rgb, pal, allowed):
    """Index of the nearest allowed palette colour for each rgb (…x3, 0-255), weighted for the eye."""
    w = np.array([0.30, 0.59, 0.11]) ** 0.5
    cand = pal[allowed].astype(float) * w
    x = rgb.reshape(-1, 3).astype(float) * w
    out = np.empty(len(x), np.int64)
    for s in range(0, len(x), 4096):
        d = ((x[s:s + 4096, None, :] - cand[None]) ** 2).sum(2)
        out[s:s + 4096] = np.asarray(allowed)[d.argmin(1)]
    return out.reshape(rgb.shape[:-1])


def tables(pal6):
    pal = pal6.astype(float) * 4  # 6-bit to 8-bit
    normal = [i for i in range(255) if i not in FULLBRIGHT]
    every = list(range(255))
    # shades: towards black, a touch faster than linear so the far shades are properly dark (the last is black)
    shade = np.zeros((NUMSHADES, 256), np.uint8)
    for s in range(NUMSHADES):
        f = (1 - s / (NUMSHADES - 1)) ** 1.3
        shade[s] = nearest(pal * f, pal, normal)
        shade[s, list(FULLBRIGHT)] = list(FULLBRIGHT)
        shade[s, 255] = 255
    # translucency: a third of the one drawn over two thirds of what's behind
    mix = pal[:, None, :] / 3 + pal[None, :, :] * 2 / 3
    trans = nearest(mix, pal, every).astype(np.uint8)
    # swaps
    lum = (pal @ np.array([0.30, 0.59, 0.11])) / 255
    swaps = {}
    for pn, tint in TINTS.items():
        if tint is None:
            m = np.full(256, nearest(np.zeros((1, 3)), pal, normal)[0], np.uint8)
        else:
            colour, gain = tint
            want = np.clip(np.array(colour, float)[None] * np.clip(lum[:, None] * gain, 0, 1.3), 0, 255)
            m = nearest(want, pal, normal).astype(np.uint8)
        m[255] = 255
        swaps[pn] = m
    return shade, trans, swaps


def write():
    c = load()
    shade, trans, swaps = tables(c['game'])
    with open(os.path.join(OUT, 'PALETTE.DAT'), 'wb') as f:
        f.write(c['game'].tobytes())
        f.write(struct.pack('<h', NUMSHADES))
        f.write(shade.tobytes())
        f.write(trans.tobytes())
    with open(os.path.join(OUT, 'LOOKUP.DAT'), 'wb') as f:
        f.write(bytes([len(swaps)]))
        for pn, m in swaps.items():
            f.write(bytes([pn]) + m.tobytes())
        for name in BASEPALS:
            f.write(c[name].tobytes())
    print('wrote game/PALETTE.DAT, game/LOOKUP.DAT')
    return shade, trans, swaps


def compare():
    shade, trans, swaps = write()
    g = grp_files()
    p = g['PALETTE.DAT']
    dshade = np.frombuffer(p[770:770 + NUMSHADES * 256], np.uint8).reshape(NUMSHADES, 256)
    dtrans = np.frombuffer(p[770 + NUMSHADES * 256:770 + NUMSHADES * 256 + 65536], np.uint8).reshape(256, 256)
    pal = load()['game'].astype(float) * 4

    def err(a, b):
        return np.sqrt(((pal[a] - pal[b]) ** 2).sum(-1)).mean()
    print('shade: same index %.0f%%, mean colour difference %.1f' % ((shade == dshade).mean() * 100, err(shade, dshade)))
    print('translucency: same index %.0f%%, mean colour difference %.1f' % ((trans == dtrans).mean() * 100, err(trans, dtrans)))
    lk = g['LOOKUP.DAT']
    for t in range(lk[0]):
        pn = lk[1 + t * 257]
        if pn in swaps:
            dm = np.frombuffer(lk[2 + t * 257:2 + t * 257 + 256], np.uint8)
            print('swap %d: mean colour difference %.1f' % (pn, err(swaps[pn][:255], dm[:255])))


if __name__ == '__main__':
    {'extract': extract, 'compare': compare}.get(sys.argv[1] if len(sys.argv) > 1 else '', write)()
