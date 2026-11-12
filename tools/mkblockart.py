"""Prepare block face art: art/blocks/fal/<name>.png (raw generated paintings) -> art/blocks/<name>.png (1024, tiling).

  python tools/mkblockart.py

- Natural surfaces are made seamless: cross-faded with a half-offset copy of themselves, so each edge meets the
  opposite one (the offset copy wins at the edges, the original in the middle).
- Built blocks (planks, cobble) keep their edges: a seam between two crafted blocks looks right.
- wood_top is one log end per face, kept whole.
- grass_side / snow_side are composed here: the dirt with a ragged lip of grass / snow along the top, so the side
  always matches the dirt and the top it sits between. The lip's edge repeats across the face, so neighbours line up.
tools/mkworld.py then downsamples art/blocks/*.png to the in-game face size.
"""
import math, os
import numpy as np
from PIL import Image

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'art', 'blocks')
RAW = os.path.join(ROOT, 'fal')
N = 1024

SEAMLESS = ['grass_top', 'dirt', 'stone', 'bedrock', 'sand', 'wood_side', 'leaves', 'gravel', 'lava', 'water',
            'obsidian', 'snow', 'coal_ore', 'iron_ore', 'gold_ore', 'redstone_ore', 'lapis_ore', 'diamond_ore']
AS_IS = ['planks', 'cobble', 'wood_top', 'melon_side', 'melon_top']
KEYED = ['tallgrass', 'flower_red', 'flower_yellow', 'glass', 'melon_vine']  # painted on flat magenta: that becomes see-through
# small plants: keyed art scaled down to this fraction of the face, standing at the bottom middle (art/props/fal has
# the source, shared with the pickup sprites)
SMALL = {'mushroom': 0.5}


def load(name):
    return np.asarray(Image.open(os.path.join(RAW, name + '.png')).convert('RGB').resize((N, N), Image.LANCZOS),
                      dtype=np.float32)


def seamless(a):
    t = 1 - np.abs(np.linspace(-1, 1, N, dtype=np.float32))  # 0 at the edges, 1 in the middle
    t = np.clip(t * 1.6, 0, 1)                               # keep the middle all original
    m = (t[:, None] * t[None, :])[..., None]
    rolled = np.roll(a, (N // 2, N // 2), axis=(0, 1))
    return a * m + rolled * (1 - m)


def lip(base, top, depth, wobble, seed, shadow=0.55):
    """`base` with a band of `top` along the top edge: `depth` (fraction of the face) +- `wobble`, ragged, with a dark
    line under it. The edge is a sum of whole-period waves, so it repeats exactly across the face."""
    rnd = np.random.default_rng(seed)
    x = np.arange(N) / N
    edge = np.full(N, depth)
    for k in (2, 3, 5, 9, 17, 31, 47, 64):  # big lumps, then tufts
        edge += wobble / math.sqrt(k) * np.sin(2 * math.pi * (k * x + rnd.random()))
    edge = (edge * N).astype(int)
    out = base.copy()
    ys = np.arange(N)[:, None]
    over = ys < edge[None, :]
    out[over] = top[over]
    under = (ys >= edge[None, :]) & (ys < edge[None, :] + N // 40)  # shade cast by the overhang
    f = shadow + (1 - shadow) * ((ys - edge[None, :]) / (N / 40))
    out[under] = (base * f[..., None])[under]
    return out


def grassify(g):
    lum = g.mean(2, keepdims=True)
    green = np.array([62, 104, 32], np.float32) * (0.75 + 0.5 * lum / 255)
    return g * 0.4 + green * 0.6


def key_magenta(a):
    """RGBA: alpha 0 where the pixel is the magenta backdrop, and the pink fringe taken out of the edges."""
    r, g, b = a[..., 0], a[..., 1], a[..., 2]
    mag = np.minimum(r, b) - g                       # how magenta: red and blue both well above green
    alpha = np.clip(1 - (mag - 60) / 80, 0, 1) * 255  # >140 above green: backdrop; <60: the subject
    spill = np.clip(mag, 0, None)
    out = np.stack([r - spill * 0.8, g, b - spill * 0.8, alpha], -1)
    return out


def leaf_holes(a, frac=0.32):
    """RGBA: the darkest `frac` of the foliage, smoothed into clumps (the deep gaps between leaves), becomes holes
    big enough to survive shrinking to 32 pixels."""
    from PIL import ImageFilter
    lum = Image.fromarray(np.clip(a.mean(2), 0, 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(14))
    lum = np.asarray(lum, np.float32)
    cut = np.percentile(lum, frac * 100)
    alpha = np.clip((lum - cut) / 6 + 0.5, 0, 1) * 255
    return np.concatenate([a, alpha[..., None]], -1)


ORES = {  # nugget colour
    'coal_ore': (22, 22, 24), 'iron_ore': (222, 138, 78), 'gold_ore': (255, 214, 40), 'redstone_ore': (235, 28, 22),
    'lapis_ore': (70, 110, 255), 'diamond_ore': (105, 245, 240)}


def bold_ore(stone, col, seed, clusters=6):
    """The (seamless) stone with clusters of chunky ore lumps drawn on: each lump a rounded blob in the ore's colour,
    lit like a dome from the upper left over the stone's own light and shade, with a dark outline. The painted
    ores' nuggets were too small to survive shrinking to 32 pixels; these come out 5-7 pixels across. Lumps wrap
    round the edges, so the face still tiles."""
    from PIL import ImageFilter
    rnd = np.random.default_rng(seed)
    yy, xx = np.mgrid[0:N, 0:N].astype(np.float32)
    nug = np.zeros((N, N), np.float32)
    dome = np.zeros((N, N), np.float32)
    # clusters spread evenly: one per cell of a jittered grid
    cells = [(i, j) for i in range(3) for j in range(3)]
    rnd.shuffle(cells)
    for ci, cj in cells[:clusters]:
        cx, cy = (ci + rnd.uniform(0.2, 0.8)) * N / 3, (cj + rnd.uniform(0.2, 0.8)) * N / 3
        for _ in range(rnd.integers(3, 6)):
            px, py = cx + rnd.normal(0, 70), cy + rnd.normal(0, 70)
            r = rnd.uniform(52, 80)
            dx = (xx - px + N / 2) % N - N / 2
            dy = (yy - py + N / 2) % N - N / 2
            d = np.sqrt((dx / r) ** 2 + (dy / (r * rnd.uniform(0.75, 1.15))) ** 2)
            inside = d < 1
            nug = np.maximum(nug, inside.astype(np.float32))
            # dome light: brighter toward the upper left of each lump
            h = np.clip(1 - d, 0, 1) ** 0.5 * (0.6 - 0.4 * (dx + dy) / (2 * r))
            dome = np.maximum(dome, h * inside)
    soft = lambda m, r: np.asarray(Image.fromarray((np.clip(m, 0, 1) * 255).astype(np.uint8)).filter(
        ImageFilter.GaussianBlur(r)), np.float32) / 255
    nug = soft(nug, 2)
    ring = np.clip(soft(np.asarray(Image.fromarray((nug * 255).astype(np.uint8)).filter(ImageFilter.MaxFilter(21)),
                                   np.float32) / 255, 2) - nug, 0, 1)
    lum = soft(stone.mean(2) / 255, 3)[..., None]
    shade = (0.45 + 0.5 * lum + 0.55 * dome[..., None])
    if col.mean() < 60:  # coal: black, with grey glints where the light catches it
        paint = col + np.array([150, 150, 160], np.float32) * np.clip(dome[..., None] - 0.45, 0, 1) * 2.5
    else:
        paint = col * shade
    out = stone * (1 - 0.85 * ring[..., None])
    return out * (1 - nug[..., None]) + paint * nug[..., None]


def save(a, name):
    Image.fromarray(np.clip(a, 0, 255).astype(np.uint8)).save(os.path.join(ROOT, name + '.png'))
    print('art/blocks/%s.png' % name)


def main():
    done = {}
    for name in SEAMLESS:
        if os.path.exists(os.path.join(RAW, name + '.png')):
            done[name] = seamless(load(name))
            if name == 'grass_top':  # its bare patches tile into camouflage: pull toward an even green, softer
                done[name] = grassify(done[name])
            save(leaf_holes(done[name]) if name == 'leaves' else done[name], name)
    # Ores: the painted ones' nuggets were too small to read once shrunk to 32 pixels, so they're drawn here, big
    # and bold, on the painted stone (the fal ore paintings stay in art/blocks/fal for reference).
    for k, (name, col) in enumerate(ORES.items()):
        if name in done and 'stone' in done:
            save(bold_ore(done['stone'], np.array(col, np.float32), 100 + k), name)

    for name in KEYED:
        if os.path.exists(os.path.join(RAW, name + '.png')):
            save(key_magenta(load(name)), name)

    for name, f in SMALL.items():
        src = os.path.join(ROOT, '..', 'props', 'fal', name + '.png')
        if os.path.exists(src):
            a = key_magenta(np.asarray(Image.open(src).convert('RGB').resize((N, N), Image.LANCZOS), np.float32))
            im = Image.fromarray(np.clip(a, 0, 255).astype(np.uint8), 'RGBA')
            im = im.crop(im.getchannel('A').point(lambda v: 255 if v >= 128 else 0).getbbox())
            s = N * f / max(im.size)
            im = im.resize((round(im.width * s), round(im.height * s)), Image.LANCZOS)
            out = Image.new('RGBA', (N, N), (0, 0, 0, 0))
            out.paste(im, ((N - im.width) // 2, N - im.height))
            save(np.asarray(out, np.float32), name)

    for name in AS_IS:
        if os.path.exists(os.path.join(RAW, name + '.png')):
            save(load(name), name)
    if 'dirt' in done and 'grass_top' in done:
        # the top's bare patches would make a brown lip: pull it toward its own green so it reads at 32 pixels
        save(lip(done['dirt'], done['grass_top'], 0.27, 0.06, 1), 'grass_side')
    if 'dirt' in done and 'snow' in done:
        save(lip(done['dirt'], done['snow'], 0.24, 0.05, 2), 'snow_side')


if __name__ == '__main__':
    main()
