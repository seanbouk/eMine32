"""Generate eMine32's spike content into game/:
  - textures/blocks/*.png  VW_TEXSIZE (32) block face textures: art/blocks/<name>.png (painted at ~1024, downsampled
                           here) where it exists, else the 16x16 procedural placeholder scaled up
  - emine32.def            loads them as tiles VW_TILEBASE.. (order must match voxworld.cpp)
  - E1L1.MAP               a one-sector Build map that opts into the block world (sector.extra = 0x7E32)

Run: python tools/mkworld.py
"""
import os, random, struct
from PIL import Image

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'game')
TEX = 16      # procedural placeholders are drawn at this size
OUT = 32      # block face size in the game (VW_TEXSIZE in voxworld.h)
ART = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'art', 'blocks')
TILEBASE = 7000
SKYTILE = 7200  # the night sky panorama
MAGIC = 0x7E32

# ---------------------------------------------------------------- textures
def noise_tex(seed, base, var, speck=None, speck_p=0.0):
    rnd = random.Random(seed)
    im = Image.new('RGB', (TEX, TEX))
    for y in range(TEX):
        for x in range(TEX):
            f = 1.0 + rnd.uniform(-var, var)
            c = [max(0, min(255, int(ch * f))) for ch in base]
            if speck and rnd.random() < speck_p:
                c = list(speck)
            im.putpixel((x, y), tuple(c))
    return im

def grass_side():
    im = noise_tex(2, (121, 85, 58), 0.18, (90, 64, 44), 0.15)
    top = noise_tex(1, (96, 150, 60), 0.2)
    rnd = random.Random(3)
    for x in range(TEX):
        depth = 3 + (rnd.random() < 0.5)
        for y in range(depth):
            im.putpixel((x, y), top.getpixel((x, y)))
    return im

def wood_side():
    im = noise_tex(6, (104, 82, 50), 0.1)
    for x in range(0, TEX, 4):
        for y in range(TEX):
            r, g, b = im.getpixel((x, y))
            im.putpixel((x, y), (int(r * .7), int(g * .7), int(b * .7)))
    return im

def wood_top():
    im = noise_tex(7, (160, 130, 80), 0.08)
    for y in range(TEX):
        for x in range(TEX):
            d = max(abs(x - 7.5), abs(y - 7.5))
            if int(d) % 3 == 0:
                r, g, b = im.getpixel((x, y))
                im.putpixel((x, y), (int(r * .75), int(g * .75), int(b * .75)))
    return im

def planks():
    im = noise_tex(9, (170, 132, 82), 0.1)
    for y in range(TEX):
        for x in range(TEX):
            if y % 4 == 3 or (x == (0 if (y // 4) % 2 else 8)):
                r, g, b = im.getpixel((x, y))
                im.putpixel((x, y), (int(r * .6), int(g * .6), int(b * .6)))
    return im

def cobble():
    im = noise_tex(10, (120, 120, 120), 0.12)
    rnd = random.Random(11)
    for _ in range(9):
        cx, cy, r = rnd.randrange(TEX), rnd.randrange(TEX), rnd.randrange(2, 4)
        for y in range(TEX):
            for x in range(TEX):
                dx, dy = min(abs(x - cx), TEX - abs(x - cx)), min(abs(y - cy), TEX - abs(y - cy))
                if r - 0.6 < (dx * dx + dy * dy) ** .5 < r + 0.4:
                    im.putpixel((x, y), (70, 70, 72))
    return im

def stone():
    return noise_tex(3, (128, 128, 128), 0.12, (100, 100, 104), 0.2)

def ore(seed, colour, clusters=5):
    """Stone with a few clumps of coloured nuggets."""
    im = stone()
    rnd = random.Random(seed)
    for _ in range(clusters):
        cx, cy = rnd.randrange(1, TEX - 2), rnd.randrange(1, TEX - 2)
        for _ in range(rnd.randrange(3, 6)):
            x, y = cx + rnd.randrange(-1, 2), cy + rnd.randrange(-1, 2)
            f = rnd.uniform(0.75, 1.15)
            im.putpixel((x % TEX, y % TEX), tuple(max(0, min(255, int(ch * f))) for ch in colour))
    return im

def gravel():
    im = noise_tex(12, (126, 118, 112), 0.1)
    rnd = random.Random(13)
    for _ in range(26):
        x, y = rnd.randrange(TEX), rnd.randrange(TEX)
        c = rnd.choice([(88, 84, 82), (160, 150, 142), (110, 100, 96)])
        for dx, dy in ((0, 0), (1, 0), (0, 1)):
            if rnd.random() < 0.8:
                im.putpixel(((x + dx) % TEX, (y + dy) % TEX), c)
    return im

def lava():
    """Bright molten orange with darker crust veins and hot yellow spots. Tiles seamlessly (the engine scrolls it)."""
    import math
    rnd = random.Random(18)
    im = Image.new('RGB', (TEX, TEX))
    ph = [rnd.uniform(0, 6.28) for _ in range(4)]
    for y in range(TEX):
        for x in range(TEX):
            a = 2 * math.pi * x / TEX
            b = 2 * math.pi * y / TEX
            v = (math.sin(a + ph[0]) + math.sin(b * 2 + ph[1]) + math.sin(a + b + ph[2]) + math.sin(a * 2 - b + ph[3])) / 4
            v += rnd.uniform(-0.12, 0.12)
            if v < -0.35:
                c = (120, 30, 10)        # crust
            elif v < 0.0:
                c = (200, 60, 10)
            elif v < 0.4:
                c = (245, 120, 20)
            else:
                c = (255, 200, 60)       # hot
            im.putpixel((x, y), c)
    return im

def water():
    """Blue with soft lighter ripples. Tiles seamlessly (the engine scrolls it); it is drawn see-through."""
    import math
    rnd = random.Random(19)
    im = Image.new('RGB', (TEX, TEX))
    ph = [rnd.uniform(0, 6.28) for _ in range(3)]
    for y in range(TEX):
        for x in range(TEX):
            a = 2 * math.pi * x / TEX
            b = 2 * math.pi * y / TEX
            v = (math.sin(a + b + ph[0]) + math.sin(2 * a - b + ph[1]) + math.sin(b * 2 + ph[2])) / 3
            v += rnd.uniform(-0.1, 0.1)
            if v > 0.55:
                c = (110, 160, 235)      # glint
            elif v > 0.1:
                c = (52, 104, 200)
            else:
                c = (38, 80, 176)
            im.putpixel((x, y), c)
    return im

def obsidian():
    im = noise_tex(20, (34, 22, 48), 0.25, (70, 50, 100), 0.12)
    return im

# Plants: drawn as billboards, so the background is transparent (alpha 0 -> Build's transparent colour).
def tallgrass():
    """A tuft: lots of blades, close together, the grass-top colours (darker low down)."""
    rnd = random.Random(21)
    im = Image.new('RGBA', (TEX, TEX), (0, 0, 0, 0))
    for i in range(16):
        x = rnd.randint(1, TEX - 2)
        h = rnd.randint(6, 15) if 3 <= x <= 12 else rnd.randint(4, 9)
        lean = rnd.choice((-1, 0, 0, 1))
        light = rnd.uniform(0.85, 1.25)
        for k in range(h):
            xx = x + (lean * k) // 5
            if not 0 <= xx < TEX:
                continue
            f = light * (0.7 + 0.3 * k / h)
            c = tuple(max(0, min(255, int(ch * f))) for ch in (96, 150, 60))
            im.putpixel((xx, TEX - 1 - k), c + (255,))
    return im

def flower(seed, petal, centre):
    rnd = random.Random(seed)
    im = Image.new('RGBA', (TEX, TEX), (0, 0, 0, 0))
    stem = (70, 124, 42, 255)
    # a few short grass blades at the foot
    for x in (3, 5, 10, 12):
        for k in range(rnd.randint(2, 4)):
            im.putpixel((x, TEX - 1 - k), (88, 142, 54, 255))
    for k in range(9):  # stem
        im.putpixel((7, TEX - 1 - k), stem)
    im.putpixel((8, TEX - 4), stem); im.putpixel((9, TEX - 5), stem)  # leaf
    im.putpixel((6, TEX - 6), stem)
    cx, cy = 7, TEX - 11  # flower head
    for dx, dy in ((0, -2), (-2, 0), (2, 0), (0, 2), (-1, -1), (1, -1), (-1, 1), (1, 1), (0, -1), (-1, 0), (1, 0), (0, 1)):
        im.putpixel((cx + dx, cy + dy), petal + (255,))
    im.putpixel((cx, cy), centre + (255,))
    return im

def glass():
    """A pale frame with a clear middle and a couple of glints (alpha 0 = see-through)."""
    im = Image.new('RGBA', (TEX, TEX), (0, 0, 0, 0))
    for i in range(TEX):
        for (x, y) in ((i, 0), (i, TEX - 1), (0, i), (TEX - 1, i)):
            im.putpixel((x, y), (190, 214, 226, 255))
    for (x, y) in ((3, 5), (4, 4), (5, 3), (9, 12), (10, 11), (11, 10), (12, 9), (11, 11)):
        im.putpixel((x, y), (232, 244, 250, 255))
    return im

def snow_side():
    """Dirt with a ragged band of snow along the top."""
    rnd = random.Random(31)
    im = noise_tex(2, (121, 85, 58), 0.18, (90, 64, 44), 0.15)
    for x in range(TEX):
        depth = 3 + rnd.randint(0, 2)
        for y in range(depth):
            v = rnd.uniform(0.92, 1.0)
            im.putpixel((x, y), tuple(int(c * v) for c in (236, 240, 246)))
    return im

# order == voxworld.cpp T_* enum
TEXTURES = [
    ('grass_top',  lambda: noise_tex(1, (96, 150, 60), 0.2, (70, 120, 40), 0.2)),
    ('grass_side', grass_side),
    ('dirt',       lambda: noise_tex(2, (121, 85, 58), 0.18, (90, 64, 44), 0.15)),
    ('stone',      stone),
    ('bedrock',    lambda: noise_tex(4, (38, 38, 40), 0.35, (104, 104, 108), 0.22)),
    ('sand',       lambda: noise_tex(5, (219, 207, 160), 0.07)),
    ('wood_side',  wood_side),
    ('wood_top',   wood_top),
    ('leaves',     lambda: noise_tex(8, (60, 120, 40), 0.3, (40, 80, 25), 0.3)),
    ('planks',     planks),
    ('cobble',     cobble),
    ('gravel',     gravel),
    ('coal_ore',   lambda: ore(14, (34, 34, 36))),
    ('iron_ore',   lambda: ore(15, (196, 150, 118))),
    ('gold_ore',   lambda: ore(16, (240, 200, 60))),
    ('redstone_ore', lambda: ore(17, (220, 30, 24), clusters=7)),
    ('lava',       lava),
    ('water',      water),
    ('obsidian',   obsidian),
    ('tallgrass',  tallgrass),
    ('flower_red', lambda: flower(22, (210, 40, 36), (60, 30, 20))),
    ('flower_yellow', lambda: flower(23, (250, 214, 48), (200, 130, 20))),
    ('glass',      glass),
    ('snow',       lambda: noise_tex(32, (238, 242, 248), 0.05, (214, 222, 236), 0.15)),
    ('snow_side',  snow_side),
    ('lapis_ore',  lambda: ore(33, (36, 70, 190))),
    ('diamond_ore', lambda: ore(34, (100, 230, 225), clusters=4)),
    ('melon_side', lambda: noise_tex(35, (60, 120, 40), 0.25)),
    ('melon_top',  lambda: noise_tex(36, (70, 130, 45), 0.25)),
    ('melon_vine', tallgrass),
    ('mushroom',   tallgrass),
]

def cracks():
    """Damage-stage crack masks shared by every block: dark lines on transparent, each stage adding to the last.
    Opaque texels darken the block texel under them (black = darkest), so these are placeholders for real art."""
    rnd = random.Random(40)
    strokes = []  # (x, y, colour) in drawing order; stage n shows the first n/3 of them
    # a few cracks wandering out from near the middle, then branches off them
    starts = [(7, 8), (8, 7), (5, 10), (10, 5), (3, 4), (12, 12)]
    for sx, sy in starts:
        x, y = sx, sy
        dx, dy = rnd.choice(((1, 0), (-1, 0), (0, 1), (0, -1), (1, 1), (-1, -1), (1, -1), (-1, 1)))
        for k in range(rnd.randint(4, 7)):
            if not (0 <= x < TEX and 0 <= y < TEX):
                break
            strokes.append((x, y, (20, 20, 20) if k < 3 else (60, 60, 60)))
            if rnd.random() < 0.35:  # chipped edge beside the crack
                cx, cy = x + rnd.choice((-1, 1)), y
                if 0 <= cx < TEX:
                    strokes.append((cx, cy, (120, 120, 120)))
            if rnd.random() < 0.4:  # wander
                dx, dy = rnd.choice(((dx, dy), (dy, dx), (-dy, -dx) if dx != dy else (dx, 0)))
            x, y = x + dx, y + dy
    out = []
    for st in (1, 2, 3):
        im = Image.new('RGBA', (TEX, TEX), (0, 0, 0, 0))
        for x, y, c in strokes[:len(strokes) * st // 3]:
            im.putpixel((x, y), c + (255,))
        out.append(im)
    return out

# Duke's palette, for snapping faces to it here (the engine would pick the nearest colour itself, fullbright ones
# included: those never darken with distance, so a speck of one in a texture glows in the dark)
_PAL = None
def palette():
    global _PAL
    if _PAL is None:
        import struct, sys
        import numpy as np
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import mksprite
        d = mksprite.grp_file(mksprite.GRP, 'PALETTE.DAT')
        pal = np.frombuffer(d[:768], np.uint8).reshape(256, 3).astype(np.int64) << 2
        ns = struct.unpack('<h', d[768:770])[0]
        sh = np.frombuffer(d[770:770 + ns * 256], np.uint8).reshape(ns, 256)
        fb = np.all(sh == sh[0], axis=0) & (pal.sum(1) > 0)
        fbrgb = {tuple(pal[i]) for i in range(256) if fb[i]}
        # usable: not fullbright, not 255 (transparent), and not the same RGB as a fullbright entry (the engine
        # matching that RGB could land on either)
        ok = [i for i in range(255) if not fb[i] and tuple(pal[i]) not in fbrgb]
        _PAL = pal[ok]
    return _PAL

def snap(im):
    """RGB(A) -> the same with every opaque pixel moved to the nearest usable palette colour (Build's own weighting)."""
    import numpy as np
    pal = palette()
    a = np.asarray(im.convert('RGBA'), np.int64)
    rgb = a[..., :3].reshape(-1, 1, 3)
    i = ((((rgb - pal[None]) ** 2) * np.array([30, 59, 11])).sum(2)).argmin(1)
    out = a.copy()
    out[..., :3] = pal[i].reshape(a.shape[0], a.shape[1], 3)
    return Image.fromarray(out.astype(np.uint8), 'RGBA' if im.mode == 'RGBA' else None).convert(im.mode)

PLANTS = ('tallgrass', 'flower_red', 'flower_yellow', 'melon_vine', 'mushroom')

def face(name, fn):
    """The OUT x OUT face: painted art downsampled (box filter: an honest average, then a touch of sharpening so it
    doesn't go to mush), or the procedural placeholder scaled up with hard pixels; then snapped to the palette."""
    from PIL import ImageFilter
    src = os.path.join(ART, name + '.png')
    if not os.path.exists(src):
        return snap(fn().resize((OUT, OUT), Image.NEAREST))
    im = Image.open(src)
    if 'A' in im.getbands():
        # average in premultiplied alpha so see-through pixels don't bleed their colour into the edges
        im = im.convert('RGBa').resize((OUT, OUT), Image.BOX).convert('RGBA')
        rgb = im.convert('RGB').filter(ImageFilter.UnsharpMask(radius=1, percent=60, threshold=2))
        # Build has one transparent colour: alpha is all or nothing. Plants keep thin stems: a low cut-off.
        cut = 60 if name in PLANTS else 128
        rgb.putalpha(im.getchannel('A').point(lambda v: 255 if v >= cut else 0))
        im = rgb
    else:
        im = im.convert('RGB').resize((OUT, OUT), Image.BOX)
        im = im.filter(ImageFilter.UnsharpMask(radius=1, percent=60, threshold=2))
    return snap(im)

def write_sky(w=2048, h=512, seed=11):
    """A night sky in the spirit of Roblox's: dark gradient, a field of stars, one big round moon (about 11 degrees
    across), wispy clouds whose undersides catch a low twilight glow along part of the horizon. Periodic in x so
    the seam doesn't show. Dithered into Duke's palette so the gradients don't band. Horizon at row 3/4 h."""
    import numpy as np
    rnd = np.random.default_rng(seed)
    hz = h * 3 // 4
    y = np.arange(h, dtype=np.float32)[:, None]
    x = np.arange(w, dtype=np.float32)[None, :]
    up = np.clip((hz - y) / hz, 0, 1)                                  # 0 at the horizon, 1 at the top
    ang = 2 * np.pi * x / w

    # sky: deep navy overhead, a dusky blue-violet toward the horizon
    top, low = np.array([8, 11, 30.]), np.array([38, 40, 78.])
    t = up ** 0.55
    img = low * (1 - t[..., None]) + top * t[..., None]
    img = np.broadcast_to(img, (h, w, 3)).copy()

    # twilight: a warm glow low on one side of the horizon (and a faint one opposite)
    glowdir = 0.25 * 2 * np.pi
    side = (0.5 + 0.5 * np.cos(ang - glowdir)) ** 3 + 0.25 * (0.5 + 0.5 * np.cos(ang - glowdir - np.pi)) ** 4
    low_band = np.exp(-((hz - y) / (h * 0.16)) ** 2) * (y <= hz)
    glow = (side * low_band)[..., None] * np.array([110, 58, 46.])
    img += glow

    # stars: more overhead, fading toward the horizon haze
    n = 1400
    sx = rnd.integers(0, w, n); sy = (rnd.random(n) ** 1.6 * hz * 0.95).astype(int)
    for i in range(n):
        b = rnd.random() ** 3 * 200 + 50
        fade = min(1.0, (hz - sy[i]) / (h * 0.25))
        c = np.array([b, b, b * 1.05 + 10]) * fade
        img[sy[i], sx[i]] = np.maximum(img[sy[i], sx[i]], c)
        if b > 210:  # the bright ones get a cross
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                yy, xx = sy[i] + dy, (sx[i] + dx) % w
                if 0 <= yy < h: img[yy, xx] = np.maximum(img[yy, xx], c * 0.55)

    # clouds: banks of soft, wide, flat puffs above the horizon (wrapped in x)
    dens = np.zeros((h, w), np.float32)
    for _ in range(26):
        cx, cy = rnd.uniform(0, w), rnd.uniform(hz * 0.5, hz * 0.9)
        for _ in range(rnd.integers(5, 11)):
            px, py = cx + rnd.normal(0, 70), cy + rnd.normal(0, 9)
            sx, sy = rnd.uniform(30, 85), rnd.uniform(6, 14)
            ddx = (x - px + w / 2) % w - w / 2
            dens += 0.55 * np.exp(-(ddx / sx) ** 2 - ((y - py) / sy) ** 2)
    dens = np.clip(dens, 0, 1)
    cloud = np.array([50, 52, 80.])
    lit = np.clip(dens - np.roll(dens, -8, axis=0), 0, 1)             # undersides: cloud here, clear just below
    rim = np.clip(dens - np.roll(dens, 6, axis=0), 0, 1)              # tops catch a little moonlight
    under = (0.15 + side)[..., None] * np.array([170, 100, 76.])
    img = img * (1 - dens[..., None] * 0.9) + cloud * dens[..., None] * 0.9 + lit[..., None] * under * 1.4         + rim[..., None] * np.array([40, 44, 60.])

    # the moon: round, pale, a few darker maria, a soft halo
    mx, my, mr = int(w * 0.62), int(hz * 0.62), 30  # about 25 degrees up
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    dx = (xx - mx + w / 2) % w - w / 2; dy = yy - my
    d = np.hypot(dx, dy)
    halo = np.exp(-np.clip(d - mr, 0, None) / 26.0)[..., None] * np.array([40, 44, 60.]) * (d >= mr)[..., None]
    img += halo
    disc = d < mr
    shade = 1 - 0.18 * (dx + dy) / (2 * mr)                            # lit a touch more from the lower left
    maria = np.zeros_like(d)
    for (ox, oy, r) in ((-9, -6, 10), (8, 4, 8), (-2, 12, 6), (12, -10, 5), (-14, 8, 4)):
        maria += np.exp(-((dx - ox) ** 2 + (dy - oy) ** 2) / (2 * r * r))
    moon = np.array([226, 224, 210.]) * (shade - 0.22 * np.clip(maria, 0, 1))[..., None]
    edge = np.clip(mr - d, 0, 1)[..., None]                            # antialiased rim
    img = img * (1 - edge * disc[..., None]) + moon * (edge * disc[..., None])

    # below the horizon: dark haze (seen only through gaps at the world's edge)
    below = (y > hz)[..., None]
    img = np.where(below, img * 0.0 + np.array([14, 15, 26.]) + np.array([30, 18, 16.]) * side[..., None] * np.exp(-(y - hz) / 12)[..., None], img)

    # ordered dither into the palette
    bayer = np.array([[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]], np.float32) / 16 - 0.5
    img = img + np.tile(bayer, (h // 4, w // 4))[..., None] * 10
    im = Image.fromarray(np.clip(img, 0, 255).astype(np.uint8))
    snap(im).save(os.path.join(ROOT, 'textures', 'sky.png'))


def write_textures():
    d = os.path.join(ROOT, 'textures', 'blocks')
    os.makedirs(d, exist_ok=True)
    lines = ['// eMine32 block face tiles (generated by tools/mkworld.py). Order matches voxworld.cpp.']
    for i, (name, fn) in enumerate(TEXTURES):
        face(name, fn).save(os.path.join(d, name + '.png'))
        lines.append('tilefromtexture %d { file "textures/blocks/%s.png" }' % (TILEBASE + i, name))
    lines.append('// damage-stage crack masks (VW_CRACKBASE = VW_TILEBASE + number of face tiles)')
    for st, im in enumerate(cracks()):
        im.resize((OUT, OUT), Image.NEAREST).save(os.path.join(d, 'crack_%d.png' % (st + 1)))
        lines.append('tilefromtexture %d { file "textures/blocks/crack_%d.png" }' % (TILEBASE + len(TEXTURES) + st, st + 1))
    write_sky()
    lines.append('// the night sky: one panorama, wrapped once around the horizon (horizon at 3/4 of its height)')
    lines.append('tilefromtexture %d { file "textures/sky.png" }' % SKYTILE)
    lines.append('// the title screen, replacing BETASCREEN in its own size and palette (tools/mktitle.py)')
    lines.append('artfile { file "textures/studio.art" }  // over DREALMS, the studio screen before the title')
    lines.append('artfile { file "textures/title.art" }')
    lines.append('artfile { file "textures/logo_shift.art" }    // over DUKENUKEM')
    lines.append('artfile { file "textures/logo_happens.art" }  // over THREEDEE')
    lines.append('artfile { file "textures/menu.art" }          // over MENUSCREEN, the menu backdrop')
    lines.append('artfile { file "textures/logo_ingame.art" }   // over INGAMEDUKETHREEDEE, the name on the in-game menu')
    lines.append('artfile { file "textures/cursor.art" }        // over SPINNINGNUKEICON: a spinning grass block (tools/mkcursor.py)')
    lines.append('artfile { file "textures/loading.art" }       // over LOADSCREEN')
    lines.append('// fonts, rendered from free fonts over the original font tiles (tools/mkfont.py)')
    for f in ('big', 'bigappos', 'small', 'mini', 'digits'):
        lines.append('artfile { file "textures/font_%s.art" }' % f)
    lines.append('// sprites (monsters etc.): generated by tools/mksprite.py from art/sprites.json')
    lines.append('include "sprites.def"')
    open(os.path.join(ROOT, 'emine32.def'), 'w').write('\n'.join(lines) + '\n')

# ---------------------------------------------------------------- map (Build v7)
def write_map(size_blocks=2032, height_blocks=160, seed=1234):
    half = size_blocks * 512 // 2
    floorz = 0
    ceilz = floorz - (height_blocks + 2) * 8192
    sky = SKYTILE  # our night sky (write_sky); the block renderer wraps it once around the horizon

    walls = [(-half, -half), (half, -half), (half, half), (-half, half)]
    sectors = [struct.pack('<hhiihhhhbBBBhhbBBBBBhhh',
        0, 4, ceilz, floorz,
        1, 0,            # ceilingstat: parallax; floorstat
        sky, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0,
        0, 0,            # visibility, filler
        0, seed, MAGIC)]
    wallb = [struct.pack('<iihhhhhhbBBBBBhhh', x, y, (i + 1) % 4, -1, -1, 0, 0, 0, 0, 0, 8, 8, 0, 0, 0, 0, 0)
             for i, (x, y) in enumerate(walls)]

    def spr(x, y, pic, ang=0, xr=40, yr=40, cstat=0):
        return struct.pack('<iiihhbBBBBBbbhhhhhhhhhh', x, y, floorz, cstat, pic, 0, 0, 32, 0, xr, yr, 0, 0,
                           0, 0, ang, -1, 0, 0, 0, 0, 0, 0)

    B = 512
    sprites = [
        spr(0, 0, 1405, ang=0),                 # APLAYER start, facing +x
        spr(10 * B, 3 * B, 1680, ang=1024),     # LIZTROOP
        spr(14 * B, -4 * B, 1680, ang=1024),
        spr(20 * B, 6 * B, 2000, ang=1024),     # PIGCOP
        spr(4 * B, 2 * B, 51),                  # medkit-ish pickup
        spr(5 * B, -2 * B, 40),                 # ammo
        # Test weapons by the spawn so every mining weapon is easy to try.
        spr(2 * B, -2 * B, 28),                 # SHOTGUNSPRITE
        spr(2 * B, -3 * B, 49),                 # SHOTGUNAMMO
        spr(3 * B, -2 * B, 22),                 # CHAINGUNSPRITE
        spr(3 * B, -3 * B, 41),                 # BATTERYAMMO
        spr(4 * B, -2 * B, 23),                 # RPGSPRITE
        spr(4 * B, -3 * B, 44),                 # RPGAMMO
    ]

    out = struct.pack('<iiiihhH', 7, 0, 0, floorz - 40 * 8192, 0, 0, len(sectors))
    out += b''.join(sectors)
    out += struct.pack('<H', len(wallb)) + b''.join(wallb)
    out += struct.pack('<H', len(sprites)) + b''.join(sprites)
    open(os.path.join(ROOT, 'E1L1.MAP'), 'wb').write(out)

if __name__ == '__main__':
    write_textures()
    write_map()
    print('wrote', os.path.normpath(ROOT))
