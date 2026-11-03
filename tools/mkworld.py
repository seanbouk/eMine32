"""Generate eMine32's spike content into game/:
  - textures/blocks/*.png  16x16 block face textures (procedural placeholders; real art replaces these)
  - emine32.def            loads them as tiles VW_TILEBASE.. (order must match voxworld.cpp)
  - E1L1.MAP               a one-sector Build map that opts into the block world (sector.extra = 0x7E32)

Run: python tools/mkworld.py
"""
import os, random, struct
from PIL import Image

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'game')
TEX = 16
TILEBASE = 7000
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
]

def write_textures():
    d = os.path.join(ROOT, 'textures', 'blocks')
    os.makedirs(d, exist_ok=True)
    lines = ['// eMine32 block face tiles (generated by tools/mkworld.py). Order matches voxworld.cpp.']
    for i, (name, fn) in enumerate(TEXTURES):
        fn().save(os.path.join(d, name + '.png'))
        lines.append('tilefromtexture %d { file "textures/blocks/%s.png" }' % (TILEBASE + i, name))
    open(os.path.join(ROOT, 'emine32.def'), 'w').write('\n'.join(lines) + '\n')

# ---------------------------------------------------------------- map (Build v7)
def write_map(size_blocks=2032, height_blocks=160, seed=1234):
    half = size_blocks * 512 // 2
    floorz = 0
    ceilz = floorz - (height_blocks + 2) * 8192
    sky = 89  # Duke's parallax city sky, for now

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
