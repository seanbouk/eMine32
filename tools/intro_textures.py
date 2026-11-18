"""Intro map textures: fal paintings (art/intro/fal) + local ones -> palette-matched pixel textures (art/intro/tex).

    python tools/intro_textures.py

Everything is reduced to the game's palette (game/PALETTE.DAT) at Duke-like texel sizes, so the Blender look-dev
shows what the engine will. Writes art/intro/tex/<name>.png and art/intro/tex/manifest.json:
  tile textures: {"mode": "tile", "bu": size of one repeat in Blender units (1 BU = 512 Build units)}
  fit textures:  {"mode": "fit"} (stretched over one face), sprites: {"mode": "sprite", "w", "h"} in BU
  "emit": an emission mask image name (fullbright parts), or true for wholly fullbright.
Tiling textures are 32 texels per BU, the same density as Duke's walls at the default repeat (16 Build units a texel).
Props and signs get 64 per BU.
"""
import json, os, struct, sys
import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont
from scipy import ndimage

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SRC = os.path.join(ROOT, 'art', 'intro', 'fal')
OUT = os.path.join(ROOT, 'art', 'intro', 'tex')
sys.path.insert(0, HERE)
import mksprite, mkfx

os.makedirs(OUT, exist_ok=True)
MAN = {}

# ---------------------------------------------------------------- palette
d = open(os.path.join(ROOT, 'game', 'PALETTE.DAT'), 'rb').read()
PAL = np.frombuffer(d[:768], np.uint8).reshape(256, 3).astype(np.int32) << 2
nshades = struct.unpack('<h', d[768:770])[0]
shades = np.frombuffer(d[770:770 + nshades * 256], np.uint8).reshape(nshades, 256)
FULLBRIGHT = np.all(shades == shades[0], axis=0) & (PAL.sum(1) > 0)
USABLE = np.array([i for i in range(255) if not FULLBRIGHT[i]])
ALL = np.arange(255)
LAB = {False: mksprite.to_lab(PAL[USABLE].astype(float)), True: mksprite.to_lab(PAL[ALL].astype(float))}

def quantise(im, glow=False):
    im = im.convert('RGBA')
    return mksprite.quantise(im, PAL, ALL if glow else USABLE, LAB[glow])

def save(im, name, glow=False, **meta):
    quantise(im, glow).save(os.path.join(OUT, name + '.png'))
    MAN[name] = meta

def src(name):
    return Image.open(os.path.join(SRC, name + '.png')).convert('RGB')

def crop(im, x0, y0, x1, y1):
    w, h = im.size
    return im.crop((int(x0 * w), int(y0 * h), int(x1 * w), int(y1 * h)))

# ---------------------------------------------------------------- tiling
def seamless(im, n=512):
    a = np.asarray(im.resize((n, n), Image.LANCZOS), np.float32)
    t = np.clip((1 - np.abs(np.linspace(-1, 1, n, dtype=np.float32))) * 1.6, 0, 1)
    m = (t[:, None] * t[None, :])[..., None]
    return Image.fromarray((a * m + np.roll(a, (n // 2, n // 2), (0, 1)) * (1 - m)).clip(0, 255).astype(np.uint8))

def tile(name, im, px, bu, make_seamless=True, glow=False):
    if make_seamless: im = seamless(im)
    save(im.resize((px, px), Image.LANCZOS), name, glow, mode='tile', bu=bu)

for name, px, bu in [('brick', 128, 4), ('asphalt', 128, 4), ('render', 128, 4), ('shutter', 128, 4),
                     ('cinder', 64, 2), ('lino', 64, 2), ('ceiling', 64, 2), ('carpet', 128, 4)]:
    tile(name, src(name), px, bu)

# the pale wall: the same brick, painted over in cream (the sodium wash does the rest)
b = np.asarray(seamless(src('brick')), np.float32)
lum = b.mean(2, keepdims=True)
pale = 0.35 * b + 0.65 * (np.array([214, 204, 186]) * (0.62 + 0.55 * lum / 255))
tile('brick_pale', Image.fromarray(pale.clip(0, 255).astype(np.uint8)), 128, 4, make_seamless=False)

def noise_tile(name, base, var, px=64, bu=2, seed=1, streak=False):
    rng = np.random.default_rng(seed)
    n = ndimage.gaussian_filter(rng.standard_normal((px, px)), 1.2, mode='wrap')
    if streak: n += ndimage.gaussian_filter(rng.standard_normal((px, px)), (0.6, 6), mode='wrap') * 2
    n = n / (np.abs(n).max() + 1e-6)
    a = np.array(base, np.float32)[None, None] + n[..., None] * np.array(var, np.float32)
    save(Image.fromarray(a.clip(0, 255).astype(np.uint8)), name, mode='tile', bu=bu)

noise_tile('arcade_wall', (34, 22, 44), (10, 8, 12), seed=2)
noise_tile('laminate', (22, 20, 22), (8, 8, 8), seed=3)
noise_tile('plywood', (120, 92, 60), (30, 22, 14), seed=4, streak=True)
noise_tile('steel', (88, 96, 92), (16, 16, 16), seed=5)
noise_tile('rubber', (24, 24, 26), (6, 6, 6), seed=6)
noise_tile('paint_black', (16, 16, 18), (6, 6, 6), seed=7)
noise_tile('truck_white', (178, 178, 172), (14, 14, 14), seed=8)

# ---------------------------------------------------------------- sheets split into items
def items(name, n, bg_tol=40, hi=20):
    """the n biggest figures on a flat-background sheet, left to right: [(rgba crop, bbox)]"""
    im = src(name)
    a = np.asarray(im).astype(np.int32)
    border = np.concatenate([a[0], a[-1], a[:, 0], a[:, -1]])
    bg = np.median(border, axis=0)
    sat, lum = a.max(2) - a.min(2), a.mean(2)
    isbg = (np.abs(a - bg).sum(2) < bg_tol) | ((sat < 16) & (lum > bg.mean() - 60) & (lum < bg.mean() + hi))
    # background is only what's connected to the edge, so a white toilet on a pale sheet stays solid
    lab0, _ = ndimage.label(isbg)
    edge = np.unique(np.concatenate([lab0[0], lab0[-1], lab0[:, 0], lab0[:, -1]]))
    isbg = np.isin(lab0, edge[edge > 0])
    keep = ndimage.binary_opening(~isbg, iterations=2)
    lab, k = ndimage.label(keep)
    sizes = ndimage.sum(keep, lab, range(1, k + 1))
    big = 1 + np.argsort(sizes)[::-1][:n]
    out = []
    for i in big:
        sl = ndimage.find_objects((lab == i).astype(int))[0]
        m = ndimage.binary_fill_holes(lab[sl] == i)
        rgba = np.dstack([a[sl], m * 255]).astype(np.uint8)
        out.append((Image.fromarray(rgba, 'RGBA'), sl[1].start))
    return [o[0] for o in sorted(out, key=lambda o: o[1])]

def flatten(rgba, fill=(14, 12, 14)):
    bg = Image.new('RGBA', rgba.size, fill + (255,))
    bg.alpha_composite(rgba)
    return bg.convert('RGB')

def fit(name, im, w_bu, h_bu, density=64, glow=False, emit=None):
    save(im.resize((max(8, round(w_bu * density)), max(8, round(h_bu * density))), Image.LANCZOS), name, glow,
         mode='fit', **({'emit': emit} if emit else {}))

# arcade cabinets: the front splits at the control panel; above it is the tall block's face, below it the ledge's
CAB_W, CAB_H, PANEL_H = 0.9, 1.9, 1.0
SPLIT = 1 - PANEL_H / CAB_H        # the ledge is 1.0 of the cabinet's 1.9
def cabinet(name, im, band):
    w, h = im.size
    top, bot = im.crop((0, 0, w, int(h * SPLIT))), im.crop((0, int(h * SPLIT), w, h))
    fit(name + '_upper', top, CAB_W, CAB_H - PANEL_H)
    fit(name + '_lower', bot, CAB_W, PANEL_H)
    fit(name + '_panel', im.crop((0, int(h * band[0]), w, int(h * band[1]))), CAB_W, 0.35)

cabs = src('cabinets')
for i, (x0, x1) in enumerate([(0.115, 0.315), (0.40, 0.60), (0.705, 0.885)]):
    cabinet(f'cab{i}', crop(cabs, x0, 0.08, x1, 0.98), (0.47, 0.58))

# Mine Shift: crop to the cabinet; the marquee, screen and big red button glow
ms = crop(src('mineshift'), 0.16, 0.04, 0.84, 0.97)
cabinet('mineshift', ms, (0.43, 0.50))
mask = Image.new('L', ms.size, 0)
dm = ImageDraw.Draw(mask)
W, H = ms.size
def rect(x0, y0, x1, y1):  # in the full painting's fractions
    fx = lambda x: (x - 0.16) / 0.68 * W
    fy = lambda y: (y - 0.04) / 0.93 * H
    dm.rectangle([fx(x0), fy(y0), fx(x1), fy(y1)], fill=255)
rect(0.20, 0.06, 0.80, 0.14)    # marquee
rect(0.29, 0.245, 0.71, 0.425)  # screen
rect(0.59, 0.455, 0.70, 0.495)  # big red button
emit = Image.composite(ms, Image.new('RGB', ms.size, 0), mask)
cabinet('mineshift_emit', emit, (0.43, 0.50))
for part in ('upper', 'lower', 'panel'):
    MAN[f'mineshift_{part}']['emit'] = f'mineshift_emit_{part}'

# pinball and claw machine
pin, claw = items('pinclaw', 2)
pin, claw = flatten(pin), flatten(claw)
pw, ph = pin.size
fit('pinball_back', pin.crop((0, 0, pw, int(ph * 0.47))), 0.7, 0.9)
fit('pinball_front', pin.crop((0, int(ph * 0.62), pw, ph)), 0.7, 0.95)
fit('pinball_top', pin.crop((0, int(ph * 0.47), pw, int(ph * 0.62))), 0.7, 1.4)
fit('claw_front', claw, 1.1, 2.05)

# truck: the box side and the cab
truck = src('truck')
fit('truck_side', crop(truck, 0.345, 0.07, 0.985, 0.80), 7.5, 2.3)
fit('truck_cab', crop(truck, 0.02, 0.28, 0.345, 0.92), 2.5, 2.4)

# dumpster: the front, plus a seamless patch of its green steel for the other sides
dump = flatten(items('dumpster', 1)[0])
fit('dumpster_front', dump, 3.2, 1.3)
dw, dh = dump.size
tile('dumpster_steel', dump.crop((int(dw * 0.62), int(dh * 0.25), int(dw * 0.78), int(dh * 0.55))), 64, 2)

# fire exit door
fit('fire_door', crop(src('door'), 0.10, 0.02, 0.96, 1.0), 1.5, 2.3)

# signs (wall sprites): exit glows
exit_, staff, freeplay, ooo, noentry = items('signs', 5)
for nm, im, wb, glow in [('sign_exit', exit_, 0.7, True), ('sign_staff', staff, 0.6, False), ('sign_freeplay', freeplay, 0.25, False),
                         ('sign_outoforder', ooo, 0.45, False), ('sign_noentry', noentry, 0.6, False)]:
    hb = wb * im.size[1] / im.size[0]
    save(im.resize((max(8, round(wb * 64)), max(8, round(hb * 64))), Image.LANCZOS), nm, glow, mode='sprite', w=wb, h=hb,
         **({'emit': True} if glow else {}))

# billboards
for nm, im, hb in zip(['spr_barrel', 'spr_toilet', 'spr_chair', 'spr_bags', 'spr_brick'], items('sprites', 5, bg_tol=18, hi=2), [0.95, 0.8, 1.0, 0.7, 0.15]):
    wb = hb * im.size[0] / im.size[1]
    save(im.resize((max(8, round(wb * 64)), max(8, round(hb * 64))), Image.LANCZOS), nm, mode='sprite', w=wb, h=hb)

# the toilet is white on a pale sheet, so it has its own green-screen painting (fal edit of the sheet's toilet)
t = np.asarray(src('toilet')).astype(np.int32)
green = (t[..., 1] > t[..., 0] + 40) & (t[..., 1] > t[..., 2] + 40)
t[..., 1] = np.minimum(t[..., 1], np.maximum(t[..., 0], t[..., 2]) + 8)          # despill
ti = Image.fromarray(np.dstack([t, (~green) * 255]).astype(np.uint8), 'RGBA')
ti = ti.crop(ti.getchannel('A').getbbox())
wb = 0.8 * ti.size[0] / ti.size[1]
save(ti.resize((round(wb * 64), round(0.8 * 64)), Image.LANCZOS), 'spr_toilet', mode='sprite', w=wb, h=0.8)

rgb, alpha, hot = mkfx.flame(40, 64, 0, 14)
rgb = np.asarray(rgb, np.float32)
if rgb.max() <= 1.0: rgb = rgb * 255
fl = Image.fromarray(np.dstack([rgb.clip(0, 255), alpha * 255]).astype(np.uint8), 'RGBA')
save(fl, 'spr_fire', True, mode='sprite', w=0.6, h=0.95, emit=True)

# ---------------------------------------------------------------- the frosted front: backlit glass, the sign's neon seen backwards
GW, GH = 22 * 32, int(3.6 * 32)    # 1..23 plan-y, 0..3.6 high; u runs north -> south as seen from inside
g = np.zeros((GH, GW, 3), np.float32)
yy = np.linspace(1, 0, GH)[:, None]
g[:] = np.array([150, 170, 196]) * (0.55 + 0.45 * yy[..., None])        # brighter at the top, where the street lamps are
for cx in (0.18, 0.5, 0.82):                                              # three street lamps outside
    xx = np.linspace(0, 1, GW)[None, :]
    g += (np.exp(-((xx - cx) / 0.12) ** 2) * 60 * yy)[..., None] * np.array([1.0, 1.0, 1.05])
glass = Image.fromarray(g.clip(0, 255).astype(np.uint8))
neon = Image.new('L', (GW, GH), 0)
try: font = ImageFont.truetype(os.path.join(ROOT, 'art', 'fonts', 'anton', 'Anton-Regular.ttf'), 48)
except OSError: font = ImageFont.truetype('impact.ttf', 30)
dn = ImageDraw.Draw(neon)
dn.text((GW * 0.59, GH * 0.36), 'GRAVEYARD SHIFT', font=font, fill=255, anchor='mm')   # flipped below, so it lands 0.41 along: about plan y 10
neon = neon.transpose(Image.FLIP_LEFT_RIGHT).filter(ImageFilter.GaussianBlur(1.0))
glow_ = neon.filter(ImageFilter.GaussianBlur(10))
pink = np.array([255, 80, 170], np.float32)
a = np.asarray(glass, np.float32)
k = np.clip(np.asarray(glow_, np.float32) / 255 * 0.5, 0, 0.6)[..., None]              # soft pink haze round the tubes
core = np.clip(np.asarray(neon, np.float32) / 255 * 1.4, 0, 1)[..., None]               # the tubes: hot pink-white
a = a * 0.85 * (1 - k) + pink * k
a = a * (1 - core) + np.array([255, 150, 210], np.float32) * core
a = a + np.random.default_rng(9).normal(0, 4, a.shape[:2])[..., None]     # frosting grain, and no banding once it's palette colours
save(Image.fromarray(a.clip(0, 255).astype(np.uint8)), 'frost_front', True, mode='fit', emit=True)

json.dump(MAN, open(os.path.join(OUT, 'manifest.json'), 'w'), indent=1)
print(len(MAN), 'textures ->', OUT)
