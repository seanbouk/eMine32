"""The intro map, Graveyard Shift: a Build map built from the same plan as the Blender look-dev (tools/intro_blockout.py).

    python tools/mkintro.py [--plan]

Writes:
  game/INTRO.MAP              the map (Build v7): truck, yard, fire exit, staff corridor, arcade floor, Mine Shift
  game/textures/intro.art     its tiles, from the look-dev textures in art/intro/tex (already in the game's palette)
  game/intro.con              tile names and the intro's actors (Mine Shift, the toilet, the mopeds), included by EMINE.CON
  art/intro/renders/build_plan.png   (--plan) the sectors drawn from above, shaded as the engine will shade them

Plan coordinates are the blockout's: x east, y south (y down, like Build), in look-dev units of 512 Build units.
Heights are in the same units, up from the ground (Build z = -h * 8192).

How the map is made: regions (rooms with a floor and ceiling, or solid) are laid over each other in order with shapely,
later ones winning; light pools then cut the floors into rings so each ring can take its own shade. Every piece left
becomes a sector, shared edges become red walls, and every wall, floor and sprite is shaded from a list of lights.
"""
import json, math, os, struct, sys
import numpy as np
from PIL import Image, ImageDraw
import shapely
from shapely.geometry import Polygon, Point, box as sbox, LineString
from shapely.ops import unary_union

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
TEXDIR = os.path.join(ROOT, 'art', 'intro', 'tex')
GAME = os.path.join(ROOT, 'game')
MAN = json.load(open(os.path.join(TEXDIR, 'manifest.json')))

B = 512            # Build units per plan unit
ZU = 8192          # Build z units per plan unit of height
TILEBASE = 7400

# ---------------------------------------------------------------- palette
_pd = open(os.path.join(GAME, 'PALETTE.DAT'), 'rb').read()
PAL = np.frombuffer(_pd[:768], np.uint8).reshape(256, 3).astype(np.int32) * 4
FULLBRIGHT = set(range(240, 255))
NORMAL = [i for i in range(255) if i not in FULLBRIGHT]
_exact = {}
for i in range(255):
    _exact.setdefault((tuple(PAL[i]), i in FULLBRIGHT), i)

def to_index(rgba, glow=False):
    """RGBA image (already in the game's palette) -> palette indices; transparent -> 255"""
    a = np.asarray(rgba.convert('RGBA'))
    h, w = a.shape[:2]
    out = np.full((h, w), 255, np.uint8)
    cache = {}
    allowed = list(range(255)) if glow else NORMAL
    cand = PAL[allowed].astype(float)
    for y in range(h):
        for x in range(w):
            r, g, b, al = a[y, x]
            if al < 128: continue
            k = (r, g, b)
            if k not in cache:
                i = _exact.get((k, True)) if glow else None
                if i is None: i = _exact.get((k, False))
                if i is None:
                    i = allowed[int(((cand - np.array(k, float)) ** 2).sum(1).argmin())]
                cache[k] = i
            out[y, x] = cache[k]
    return out

def quant(im, glow=False):
    """RGB(A) image -> nearest palette indices (for the art made here)"""
    a = np.asarray(im.convert('RGBA')).astype(float)
    allowed = list(range(255)) if glow else NORMAL
    cand = PAL[allowed].astype(float)
    flat = a[..., :3].reshape(-1, 3)
    idx = np.empty(len(flat), np.int64)
    for s in range(0, len(flat), 4096):
        idx[s:s + 4096] = np.asarray(allowed)[((flat[s:s + 4096, None] - cand[None]) ** 2).sum(2).argmin(1)]
    out = idx.reshape(a.shape[:2]).astype(np.uint8)
    out[a[..., 3] < 128] = 255
    return out

# ---------------------------------------------------------------- tiles
TILES = {}        # key -> (tile number, index array, picanm)
def _add(key, px, picanm=0):
    TILES[key] = (TILEBASE + len(TILES), px, picanm)
    return TILES[key][0]

def src(name):
    return Image.open(os.path.join(TEXDIR, name + '.png')).convert('RGBA')

def glows(name):
    return bool(MAN.get(name, {}).get('emit'))

def pow2(n):
    return 1 << max(3, round(math.log2(n)))

def tile(name):
    """a world-aligned repeating texture (walls, floors, ceilings): power-of-two sizes"""
    key = name + '@tile'
    if key not in TILES:
        im = src(name)
        w, h = im.size
        if (pow2(w), pow2(h)) != (w, h):
            im = im.resize((pow2(w), pow2(h)), Image.NEAREST)
        _add(key, to_index(im, glows(name)))
    return TILES[key][0]

def fitwall(name, im=None):
    """a texture stretched once over a wall: width a multiple of 8 (xrepeat = width/8), height a power of two"""
    key = name + '@fit'
    if key not in TILES:
        im = im or src(name)
        w, h = im.size
        im = im.resize((max(8, round(w / 8) * 8), pow2(h)), Image.NEAREST)
        _add(key, to_index(im, glows(name)))
    return TILES[key][0]

def spritetile(name, im=None, glow=None):
    key = name + '@spr'
    if key not in TILES:
        _add(key, to_index(im or src(name), glows(name) if glow is None else glow))
    return TILES[key][0]

def made(name, px):
    """a tile drawn here (index array)"""
    key = name + '@made'
    if key not in TILES:
        _add(key, px)
    return TILES[key][0]

def tsize(pic):
    for t, px, _ in TILES.values():
        if t == pic: return px.shape[1], px.shape[0]
    raise KeyError(pic)

# texels: every repeating texture is 32 texels per plan unit (16 Build units each, Duke's default), except where a
# texture's own 'bu' says otherwise (the two-tone paint is drawn for a 3-unit-high wall)
def texel_units(name):
    e = MAN.get(name)
    if e and e.get('mode') == 'tile':
        w = src(name).size[0]
        return e['bu'] * B / w * (src(name).size[0] / pow2(w))
    return 16.0

# ---------------------------------------------------------------- art made here
def sky_tile():
    """night sky over a town: deep blue-black, a sodium glow low down, a few stars; repeats 8 times round"""
    w, h = 256, 256
    y = np.linspace(0, 1, h)[:, None]
    x = np.linspace(0, 2 * np.pi, w, endpoint=False)[None, :]
    glow = np.clip((y - 0.45) / 0.55, 0, 1) ** 1.8
    haze = 0.5 + 0.25 * np.sin(x * 2 + 1.3) + 0.15 * np.sin(x * 5 + y * 7)
    top = np.array([6, 8, 18], float); low = np.array([60, 38, 30], float)
    rgb = top[None, None] * (1 - glow[..., None]) + low[None, None] * glow[..., None] * (0.8 + 0.4 * haze[..., None])
    rng = np.random.default_rng(7)
    for _ in range(40):
        sx, sy = rng.integers(0, w), rng.integers(0, int(h * 0.5))
        rgb[sy, sx] = [150, 150, 170] if rng.random() < 0.7 else [200, 190, 160]
    return quant(Image.fromarray(rgb.clip(0, 255).astype(np.uint8)))

def flat_tile(rgb, size=16, noise=0.0, seed=1):
    rng = np.random.default_rng(seed)
    a = np.array(rgb, float)[None, None] * (1 + noise * rng.standard_normal((size, size, 1)))
    return quant(Image.fromarray(a.clip(0, 255).astype(np.uint8)))

def wallpack_tile():
    """the sodium wall-pack: a dark box with a glowing amber lens"""
    im = Image.new('RGBA', (24, 14), (40, 40, 44, 255))
    d = ImageDraw.Draw(im)
    d.rectangle((2, 7, 21, 12), fill=(255, 190, 90, 255))
    d.rectangle((4, 9, 19, 11), fill=(255, 230, 160, 255))
    d.line((0, 0, 23, 0), fill=(70, 70, 76, 255))
    return quant(im)

def truck_side_tile(wheels_left):
    """the box's side from the ground to the roof: livery over the box (0.8 to 3.1 high), a white roof edge above,
    the chassis and the rear wheels below (the wheels sit near the back of the truck)"""
    H_TOT, W = 3.18, 480
    ppu = 256 / H_TOT
    im = Image.new('RGBA', (W, 256), (0, 0, 0, 255))
    liv = src('truck_side').resize((W, round((3.1 - 0.8) * ppu)), Image.NEAREST)
    white = src('truck_white').resize((64, 64))
    for xx in range(0, W, 64):
        im.paste(white, (xx, 0))
    top = round((H_TOT - 3.1) * ppu)
    im.paste(liv, (0, top))
    y_deck = top + liv.size[1]
    d = ImageDraw.Draw(im)
    d.rectangle((0, y_deck, W, 255), fill=(8, 8, 10, 255))                          # shadow under the box
    d.rectangle((0, y_deck, W, y_deck + round(0.15 * ppu)), fill=(34, 34, 38, 255))  # the deck's edge
    d.rectangle((0, y_deck + round(0.35 * ppu), W, y_deck + round(0.5 * ppu)), fill=(26, 26, 28, 255))  # chassis rail
    # rear wheels: near the back (12.7..13.7 of a box 12..19.5), seen from outside
    for c in (13.2,):
        f = (c - 12) / 7.5
        cx = round((f if wheels_left else 1 - f) * W)
        r = round(0.45 * ppu)
        cy = 255 - r
        for dx in (-r - 3, r + 3):
            d.ellipse((cx + dx - r, cy - r, cx + dx + r, cy + r), fill=(20, 20, 22, 255))
            d.ellipse((cx + dx - r * 0.5, cy - r * 0.5, cx + dx + r * 0.5, cy + r * 0.5), fill=(90, 90, 96, 255))
            d.ellipse((cx + dx - r * 0.2, cy - r * 0.2, cx + dx + r * 0.2, cy + r * 0.2), fill=(50, 50, 54, 255))
    return im

# ---------------------------------------------------------------- regions
class R:
    """a region of the plan. kind 'room' (a sector: floor/ceiling heights and textures) or 'solid' (nothing there;
    its tex/faces paint the walls that face it)."""
    def __init__(self, geom, kind='room', **p):
        self.geom, self.kind, self.p = geom, kind, p

def rect(x0, y0, x1, y1):
    return sbox(min(x0, x1) * B, min(y0, y1) * B, max(x0, x1) * B, max(y0, y1) * B)

def rot_plan(x, y, deg, pivot):
    """a blockout rotation (degrees about Blender's Z, whose y points the other way) applied in plan coordinates"""
    a = math.radians(-deg)
    px, py = pivot
    dx, dy = x - px, y - py
    return px + dx * math.cos(a) - dy * math.sin(a), py + dx * math.sin(a) + dy * math.cos(a)

def rrect(x0, y0, x1, y1, deg, pivot):
    pts = [rot_plan(x, y, deg, pivot) for x, y in [(x0, y0), (x1, y0), (x1, y1), (x0, y1)]]
    return Polygon([(x * B, y * B) for x, y in pts])

REGIONS, CUTS, PAINT = [], [], []
def room(geom, floor, ceil, ftex, ctex, zone, sky=False, side='laminate', faces=None, frame=None, cut=False, **kw):
    REGIONS.append(R(geom, 'room', floor=floor, ceil=ceil, ftex=ftex, ctex=ctex, zone=zone, sky=sky, side=side,
                     faces=faces or {}, frame=frame, cut=cut, **kw))
def solid(geom, tex, faces=None, frame=None, zone=None):
    REGIONS.append(R(geom, 'solid', tex=tex, faces=faces or {}, frame=frame, zone=zone))
def paint(x0, y0, x1, y1, facing, tex):
    """walls whose middle lies in this plan rect and that face this way ('n' = toward -y; None = any) get tex"""
    PAINT.append((min(x0, x1), min(y0, y1), max(x0, x1), max(y0, y1), facing, tex))

SPRITES = []
def spr(kind, x, y, z, tex, **kw):
    SPRITES.append(dict(kind=kind, x=x, y=y, z=z, tex=tex, **kw))

LIGHTS = []
def light(x, y, z, r, s, pal=0, zones=('yard',), pool=None, seg=None, tag=None, walls_only=False):
    """shade falls by s at the light, fading to nothing at radius r. pool: ring radii (fractions of r) cut in floors.
    walls_only: a wash that lights walls (as a lamp aimed at a wall does) and not the floor"""
    LIGHTS.append(dict(p=(x, y, z), r=r, s=s, pal=pal, zones=set(zones), pool=pool, seg=seg, tag=tag, walls=walls_only))

# pal numbers: our tints (tools/mkpalette.py)
P_SODIUM, P_MINE, P_NEON, P_STREET, P_FIRE = 10, 11, 12, 13, 14

AMBIENT = dict(yard=22, street=4, truck=24, corridor=21, rooms=27, wc=25, arcade=26, frost=3, glow=0)

# ================================================================ the plan
YC = 3.18         # the yard's sky: as high as the truck's roof (anything taller sits behind a thin strip, see below)
YARD_H, BLD_H = 6.0, 7.0
TO, TI = 0.35 / 2, 0.18 / 2         # half wall thicknesses, outside and inside
CORR_C, ARC_C = 3.0, 4.0

# --- the yard. Its sky is low (the truck's roof), so the walls round it stand in a thin strip with a higher sky:
#     a red wall between two sky ceilings draws nothing above, so the far walls rise to their full height.
X0, Y0, X1, Y1 = 1 + TO, 1 + TO, 19 - TO, 23 - TO
room(rect(X0, Y0, X1, Y1), 0, YC, 'asphalt', 'sky', 'yard', sky=True, cut=True)
S = 0.04
room(rect(X0, Y0, X1, Y0 + S), 0, YARD_H, 'asphalt', 'sky', 'yard', sky=True)
room(rect(X0, Y0, X0 + S, Y1), 0, YARD_H, 'asphalt', 'sky', 'yard', sky=True)
room(rect(X0, Y1 - S, X1, Y1), 0, YARD_H, 'asphalt', 'sky', 'yard', sky=True)
room(rect(X1 - S, Y0, X1, Y1), 0, BLD_H, 'asphalt', 'sky', 'yard', sky=True)
paint(X0, Y0 - 0.1, 10.5, Y0 + 0.1, 's', 'brick_pale')       # the pale wall the truck faces
paint(X1 - 0.1, Y0, X1 + 0.1, Y1, 'w', 'render')              # the back of the arcade

# the gate: a brick lintel, then the shutter, stopped short of the ground; the street's light leaks under it
room(rect(3.5, Y1, 7.5, 23.05), 0, 4.0, 'asphalt', 'render', 'yard', side='brick')
room(rect(3.5, 23.05, 7.5, 23.2), 0, 0.22, 'asphalt', 'shutter', 'street', side='shutter')
room(rect(2.5, 23.2, 8.5, 25.5), 0, 2.0, 'asphalt', 'render', 'street', side='brick')
light(5.5, 24.0, 0.6, 3.2, 9, P_STREET, zones=('yard', 'street'), pool=(0.5, 1.0))

# bins, skip, pallets: raised floors
DUMP = {k: 'dumpster_front' for k in 'nsew'}
for x0, y0, x1, y1, h in [(10.8, 1.2, 14.0, 2.7, 1.3), (14.6, 1.2, 16.4, 2.6, 1.2), (1.2, 14.5, 2.7, 17.7, 1.3)]:
    room(rect(max(x0, X0 + S), max(y0, Y0 + S), x1, y1), h, YC, 'rubber', 'sky', 'yard', sky=True, side='dumpster_steel',
         faces=DUMP, frame=(x0, y0, x1, y1, 0, None))
room(rect(15.5, 18.5, 18.1, 19.9), 1.2, YC, 'dumpster_steel', 'sky', 'yard', sky=True, side='dumpster_steel')
for px, py, h in [(10, 14.2, 0.9), (11.3, 14.2, 0.6), (10, 15.5, 0.3)]:
    room(rect(px, py, px + 1.2, py + 1.2), h, YC, 'plywood', 'sky', 'yard', sky=True, side='plywood')
spr('face', 16.9, 1.9, 0, 'spr_bags', block=True)
spr('face', 2.0, 18.5, 0, 'spr_bags', block=True)
spr('floor', 0, 0, 0, 'paint_bays')
spr('floor', 0, 0, 0, 'paint_keepclear')
for name, x, y, heading in [('red', 2.4, 9.3, 0), ('teal', 3.9, 9.9, 20), ('cream', 2.9, 2.8, 160)]:
    spr('moped', x, y, 0, f'spr_moped_{name}', heading=heading)

# the barrel fire
spr('face', 12.6, 7.8, 0, 'spr_barrel', block=True)
spr('face', 12.6, 7.8, 0.72, 'spr_fire', h=0.95, actor='FIRE', glow=True)
light(12.6, 7.8, 1.4, 5.0, 17, P_FIRE, zones=('yard',), pool=(0.3, 0.6, 1.0))

# the sodium wall-pack over the pale wall
spr('wall', 5.55, Y0 + 0.02, 4.3, 'wallpack', facing='s', w=0.7, h=0.4)
light(5.55, 2.0, 4.7, 9.5, 22, P_SODIUM, zones=('yard', 'truck'), pool=(0.3, 0.55, 0.8))
light(5.55, 1.6, 2.6, 6.5, 22, P_SODIUM, zones=('yard',), walls_only=True)     # its wash down the pale wall

# --- the truck: parked crooked, 11 degrees about (5.3, 16), its back open to the pale wall
TR = (11, (5.3, 16))
FLOOR, TOP = 0.8, 3.1
fr = (4, 12, 6.6, 19.5) + TR
side_e = truck_side_tile(wheels_left=False); side_w = truck_side_tile(wheels_left=True)   # the back is on the e face's right
solid(rrect(4, 12, 6.6, 19.5, *TR), 'truck_white', faces={'e': ('truck_side_e', side_e), 'w': ('truck_side_w', side_w)},
      frame=fr)
room(rrect(4.08, 12, 6.52, 19.42, *TR), FLOOR, TOP, 'plywood', 'truck_white', 'truck', side='truck_white', walltex='shutter')
light(5.3, 12.6, 2.0, 3.2, 6, P_SODIUM, zones=('truck',))     # the yard's glow in at the open back
room(rrect(4.2, 17.6, 5.1, 18.5, *TR), FLOOR + 1.9, TOP, 'dustsheet', 'truck_white', 'truck', side='dustsheet')
room(rrect(5.6, 18.0, 6.45, 19.38, *TR), FLOOR + 1.1, TOP, 'plywood', 'truck_white', 'truck', side='plywood')
room(rrect(4.15, 19.5, 6.45, 22.1, *TR), 2.8, YC, 'truck_white', 'sky', 'yard', sky=True, side='truck_white',
     faces={'e': 'truck_cab', 'w': 'truck_cab_l', 's': 'truck_cab_front'}, frame=(4.15, 19.5, 6.45, 22.1) + TR)
START = rot_plan(5.3, 16.4, *TR)
START_DIR = rot_plan(5.3, 8, *TR)
# the doors, swung right open (135 degrees), as two one-sided wall sprites back to back
for hx, sgn in [(4.0, -1), (6.6, 1)]:
    hp = rot_plan(hx, 12, *TR)
    # closed, a door points across the opening; swung 135 degrees out it points back and away from the truck
    ang = math.radians((-135 if sgn < 0 else -45) - TR[0])
    spr('door', hp[0], hp[1], FLOOR, 'truck_white', ang=ang, length=1.3, h=TOP - FLOOR, back='shutter')

# --- the building
# fire exit: in the west wall, 5..6.5, propped open (hinged on its north jamb, swung 70 degrees out into the yard)
room(rect(19 - TO, 5, 19 + TO, 6.5), 0, 2.3, 'concrete', 'steel', 'corridor', side='render', walltex='steel')
spr('door', 19 - TO, 5.0, 0, 'fire_door_inside', ang=math.radians(90 + 70), length=1.5, h=2.3, back='fire_door')
spr('face', 18.45, 6.6, 0, 'spr_brick')
spr('wall', X1 - 0.015, 5.75, 2.4, 'sign_exit', facing='w', glow=True)
spr('wall', X1 - 0.015, 10.0, 2.3, 'sign_trade', facing='w')
light(18.5, 5.75, 2.6, 2.6, 8, P_MINE, zones=('yard',))                # the EXIT sign's green
light(18.6, 4.6, 2.9, 4.2, 15, 0, zones=('yard',), pool=(0.45, 0.9))  # the caged bulb over the door

# staff corridor and the rooms off it
room(rect(19 + TO, 4 + TI, 33 - TI, 7.5 - TI), 0, CORR_C, 'concrete', 'pipes', 'corridor', cut=True)
paint(19 + TO - 0.1, 1, 19 + TO + 0.1, 7.5, 'e', 'cinder_pale')
paint(19, 4 + TI - 0.1, 33, 4 + TI + 0.1, 's', 'cinder_pale')
paint(19, 7.5 - TI - 0.1, 33, 7.5 - TI + 0.1, 'n', 'wall_twotone')
paint(33 - TI - 0.1, 4, 33 - TI + 0.1, 7.5, 'w', 'wall_twotone')
for x0, x1, zone in [(19 + TO, 24 - TI, 'rooms'), (24 + TI, 27 - TI, 'wc'), (27 + TI, 33 - TI, 'rooms')]:
    room(rect(x0, 1 + TO, x1, 4 - TI), 0, CORR_C, 'concrete', 'pipes', zone, cut=True)
for x0, x1, zone in [(21.5, 22.5, 'rooms'), (25, 25.8, 'wc'), (29, 30, 'rooms')]:
    room(rect(x0, 4 - TI, x1, 4 + TI), 0, 2.2, 'concrete', 'cinder', zone, side='cinder_pale', walltex='steel')
paint(19, 1, 33, 4, None, 'cinder')
# the drinks machine, a chunk of wall beside it, the extinguisher beyond
solid(rect(25.3, 6.95, 25.75, 7.5), 'wall_twotone')
room(rect(24.2, 6.62, 25.25, 7.5 - TI), 2.0, CORR_C, 'paint_black', 'pipes', 'corridor', side='paint_black',
     faces={'n': 'vending_front'}, frame=(24.2, 6.62, 25.25, 7.5 - TI, 0, None))
spr('face', 26.4, 7.22, 0.35, 'spr_extinguisher')
spr('wall', 26.4, 7.5 - TI - 0.015, 1.05, 'sign_extinguisher', facing='n')
spr('wall', 28.55, 7.5 - TI - 0.015, 1.3, 'sign_staff', facing='n')
# fluorescent tubes: dead, flickering, dead, on
TUBES = [(20.6, 'dead'), (23.5, 'flicker'), (26.6, 'dead'), (29.7, 'on')]
for x0, state in TUBES:
    room(rect(x0, 5.65, x0 + 1.4, 5.85), 0, CORR_C - 0.06, 'concrete', 'tube_' + ('dead' if state == 'dead' else 'on'),
         'corridor', side='steel', tube=state)
light(24.2, 5.75, 2.9, 3.6, 9, 0, zones=('corridor',), pool=(0.55,), tag='flicker')
light(30.4, 5.75, 2.9, 5.0, 12, 0, zones=('corridor', 'rooms'), pool=(0.25, 0.45, 0.65, 0.85))
light(24.72, 6.3, 1.2, 1.8, 7, 0, zones=('corridor',))                  # the drinks machine's glow
light(19.6, 5.75, 1.6, 2.4, 6, P_SODIUM, zones=('corridor',))           # the yard through the door
# rooms: the office, the staff WC (the toilet gag), the store
room(rect(19.3, 1.3, 21.3, 2.3), 0.75, CORR_C, 'plywood', 'pipes', 'rooms', side='plywood')
spr('face', 20.4, 2.8, 0, 'spr_chair', block=True)
spr('face', 25.5, 1.55, 0, 'spr_toilet', actor='TOILET', block=True)
light(25.5, 2.4, 2.8, 2.6, 9, 0, zones=('wc',))
room(rect(27.3, 1 + TO, 32.7, 2.0), 2.2, CORR_C, 'plywood', 'pipes', 'rooms', side='plywood')
room(rect(27.3, 2.4, 28.5, 3.6), 1.0, CORR_C, 'dustsheet', 'pipes', 'rooms', side='dustsheet')
room(rect(29, 2.4, 30.2, 3.6), 1.6, CORR_C, 'dustsheet', 'pipes', 'rooms', side='dustsheet')

# the arcade floor (and the prize counter's corner of it, which is open to it)
ARC = unary_union([rect(19 + TO, 7.5 + TI, 41 - 0.03, 23 - TO), rect(33 + TI, 1 + TO, 41 - 0.03, 7.5 + TI)])
room(ARC, 0, ARC_C, 'carpet', 'paint_black', 'arcade', cut=True)
paint(19, 7.5, 41, 23.5, None, 'arcade_wall')
paint(33, 1, 41, 7.6, None, 'arcade_wall')
room(rect(29.2, 7.5 - TI, 30.8, 7.5 + TI), 0, 2.2, 'concrete', 'steel', 'corridor', side='wall_twotone', walltex='steel')
for hx, sgn in [(29.2, 1), (30.8, -1)]:      # the staff doors, ajar into the arcade
    ang = (0 if sgn > 0 else math.pi) + sgn * math.radians(55)
    spr('door', hx, 7.5, 0, 'steel', ang=ang, length=0.8, h=2.2, back='steel')
# the frosted front: one painted wall, backlit (no fog on it, full bright); a band of wall above it
room(rect(40.9, 1 + TO, 41 - 0.03, 23 - TO), 0, 3.6, 'carpet', 'paint_black', 'frost', side='arcade_wall', glow=True)
solid(rect(41 - 0.03, 1 + TO, 41.3, 23 - TO), 'render', faces={'w': 'frost_front'},
      frame=(41 - 0.03, 1 + TO, 41.3, 23 - TO, 0, None))
light(40.5, 12, 1.8, 7.5, 13, P_STREET, zones=('arcade',), seg=(40.5, 2, 40.5, 22), pool=(0.3, 0.6))
light(40.4, 10.2, 2.8, 4.0, 7, P_NEON, zones=('arcade',))
# prize counter, shelves, change machine, air hockey, a claw machine by the door
room(rect(34, 5.4, 39.6, 6.2), 1.05, ARC_C, 'laminate', 'paint_black', 'arcade', side='laminate')
room(rect(34, 1 + TO, 39.6, 1.85), 2.6, ARC_C, 'plywood', 'paint_black', 'arcade', side='plywood')
room(rect(39.4, 8.4, 40.5, 9.6), 1.8, ARC_C, 'steel', 'paint_black', 'arcade', side='steel')
room(rect(38.6, 17.6, 40.6, 21.2), 0.8, ARC_C, 'laminate', 'paint_black', 'arcade', side='laminate')
room(rect(39.2, 15.0, 40.4, 16.2), 2.05, ARC_C, 'paint_black', 'paint_black', 'arcade', side='paint_black',
     faces={'w': 'claw_front'}, frame=(39.2, 15.0, 40.4, 16.2, 0, None))

# arcade machines: Build-style, a tall body (marquee and screen on its face) and a lower ledge in front (coin door on
# its face, the control panel painted on top as a floor sprite)
AXIS = {'e': (1, 0), 'w': (-1, 0), 'n': (0, -1), 's': (0, 1)}
def footprint(cx, cy, face, a0, a1, width):
    dx, dy = AXIS[face]
    if dx: xs, ys = sorted((cx + dx * a0, cx + dx * a1)), (cy - width / 2, cy + width / 2)
    else:  xs, ys = (cx - width / 2, cx + width / 2), sorted((cy + dy * a0, cy + dy * a1))
    return xs[0], ys[0], xs[1], ys[1]
def cabinet(cx, cy, face, tex, width=0.95, depth=0.9, ledge=0.3, panel_h=1.0, height=1.9, glow=False):
    b = footprint(cx, cy, face, -depth / 2, depth / 2 - ledge, width)
    room(rect(*b), height, ARC_C, 'laminate', 'paint_black', 'arcade', side='laminate',
         faces={face: tex + '_upper'}, frame=b + (0, None), glow=glow)
    l = footprint(cx, cy, face, depth / 2 - ledge, depth / 2, width)
    room(rect(*l), panel_h, ARC_C, 'laminate', 'paint_black', 'arcade', side='laminate',
         faces={face: tex + '_lower'}, frame=l + (0, None), glow=glow)
    px, py = (l[0] + l[2]) / 2, (l[1] + l[3]) / 2
    spr('floor', px, py, panel_h, tex + '_panel', w=width, h=ledge, top=face, glow=glow)
def sheeted(cx, cy, face, width=0.95, depth=0.9):
    x0, y0, x1, y1 = footprint(cx, cy, face, -depth / 2, depth / 2, width)
    room(rect(x0 - 0.04, y0 - 0.04, x1 + 0.04, y1 + 0.04), 1.95, ARC_C, 'dustsheet', 'paint_black', 'arcade',
         side='dustsheet')
k = 0
for rx, face in [(19.2, 'e'), (23.5, 'w'), (24.4, 'e'), (26.9, 'e'), (32.2, 'w'), (35.3, 'w'), (36.2, 'e')]:
    y = 9.8
    while y < 18.5:
        cx, cy = rx + 0.45, y + 0.475
        if (k * 7 + 3) % 5 == 0:
            sheeted(cx, cy, face)
        else:
            cabinet(cx, cy, face, f'cab{k % 3}')
            if k % 9 == 4:
                spr('wall', cx + AXIS[face][0] * 0.47, cy, 1.25, 'sign_outoforder', facing=face, h=0.4)
        k += 1; y += 1.15
def pinball(x):
    room(rect(x, 21.45, x + 0.7, 23 - TO), 0.95, ARC_C, 'laminate', 'paint_black', 'arcade', side='laminate',
         faces={'n': 'pinball_front'}, frame=(x, 21.45, x + 0.7, 23 - TO, 0, None))
    spr('floor', x + 0.35, (21.45 + 22.65) / 2, 0.95, 'pinball_top', w=0.7, h=1.2, top='n')
    room(rect(x, 22.65, x + 0.7, 23 - TO), 1.85, ARC_C, 'laminate', 'paint_black', 'arcade', side='laminate',
         faces={'n': 'pinball_back'}, frame=(x, 22.65, x + 0.7, 23 - TO, 0, None))
def claw(x):
    room(rect(x, 21.75, x + 1.1, 23 - TO), 2.05, ARC_C, 'paint_black', 'paint_black', 'arcade', side='paint_black',
         faces={'n': 'claw_front'}, frame=(x, 21.75, x + 1.1, 23 - TO, 0, None))
def backwall(seq, x):
    for t in seq:
        if t == 'P': pinball(x); x += 0.85
        else: claw(x); x += 1.25
backwall('PPCPPCPP', 19.4)
backwall('PPCPPCP', 31.3)

# Mine Shift: the one live cabinet. Its trigger (an invisible actor on its face) takes the big red button or a kick.
cabinet(30.0, 23 - TO - 0.425, 'n', 'mineshift', width=1.0, depth=0.85, glow=True)
spr('trigger', 30.0, 23 - TO - 0.85 - 0.02, 1.0, 'trigger', facing='n', actor='MINESHIFT')
light(30.0, 21.6, 1.4, 8.0, 23, P_MINE, zones=('arcade',), pool=(0.2, 0.38, 0.58, 0.8, 1.0))

# ================================================================ overlay
GRID = 1.0
def areal(g):
    """just the polygons of an overlay result (it can leave lines and points where edges touch)"""
    from shapely.geometry import MultiPolygon
    ps = [p for p in polys(g) if p.area > 0]
    return MultiPolygon(ps) if len(ps) != 1 else ps[0]

def overlay():
    pieces = []          # [geom, region]
    for reg in REGIONS:
        g = reg.geom
        nxt = []
        for pg, pr in pieces:
            if not pg.intersects(g):
                nxt.append([pg, pr]); continue
            d = areal(shapely.difference(pg, g, grid_size=GRID))
            if not d.is_empty: nxt.append([d, pr])
        nxt.append([shapely.set_precision(g, GRID), reg])
        pieces = nxt
    return pieces

def protect(c, pieces):
    """grow a light pool's outline round anything with a painted face it would otherwise cut across"""
    guards = [pg for pg, pr in pieces if pr.p.get('faces')]
    for _ in range(4):
        changed = False
        for gg in guards:
            if c.boundary.intersects(gg.buffer(40, join_style=2)) and not c.contains(gg.buffer(40, join_style=2)):
                c = unary_union([c, gg.buffer(80, join_style=2)])
                changed = True
        if not changed: break
    return c

def cut_pools(pieces):
    for L in LIGHTS:
        if not L['pool']: continue
        x, y, _ = L['p']
        for f in sorted(L['pool']):
            r = L['r'] * f * B
            if L['seg']:
                c = LineString([(L['seg'][0] * B, L['seg'][1] * B), (L['seg'][2] * B, L['seg'][3] * B)]).buffer(r, 2, join_style=2)
            else:
                c = Point(x * B, y * B).buffer(r, 2)        # an octagon
            c = protect(c, pieces)
            nxt = []
            for pg, pr in pieces:
                if pr.kind == 'room' and pr.p.get('cut') and pr.p['zone'] in L['zones'] and pg.intersects(c):
                    a = areal(shapely.intersection(pg, c, grid_size=GRID))
                    b = areal(shapely.difference(pg, c, grid_size=GRID))
                    for part in (a, b):
                        if not part.is_empty: nxt.append([part, pr])
                else:
                    nxt.append([pg, pr])
            pieces = nxt
    return pieces

def polys(g):
    if g.is_empty: return []
    if g.geom_type == 'Polygon': return [g]
    out = []
    for gg in getattr(g, 'geoms', []):
        out += polys(gg)
    return out

# ================================================================ lighting
def lightat(x, y, z, zone, normal=None):
    """(shade 0..31, pal) at a plan point and height, for a surface facing 'normal' (3-vector) or none"""
    base = AMBIENT.get(zone, 24)
    if zone in ('frost', 'glow'): return 0, 0
    total, best, pal = 0.0, 0.0, 0
    for L in LIGHTS:
        if zone not in L['zones']: continue
        lx, ly, lz = L['p']
        if L['seg']:
            ax, ay, bx, by = L['seg']
            t = max(0, min(1, ((x - ax) * (bx - ax) + (y - ay) * (by - ay)) / ((bx - ax) ** 2 + (by - ay) ** 2)))
            lx, ly = ax + t * (bx - ax), ay + t * (by - ay)
        dx, dy, dz = lx - x, ly - y, lz - z
        d = math.sqrt(dx * dx + dy * dy + dz * dz)
        if d >= L['r']: continue
        if L['walls'] and (normal is None or abs(normal[2]) > 0.5): continue
        f = (1 - d / L['r']) ** 1.3
        if normal is not None and d > 1e-6:
            f *= 0.3 + 0.7 * max(0.0, (dx * normal[0] + dy * normal[1] + dz * normal[2]) / d)
        c = L['s'] * f
        total += c
        if c > best: best, pal = c, L['pal']
    shade = int(round(max(0, min(31, base - total))))
    return shade, (pal if best >= 5 else 0)

def area_shade(poly, z, zone, up):
    """average over the area (a grid of points)"""
    x0, y0, x1, y1 = poly.bounds
    step = max(0.2 * B, math.sqrt(poly.area / 300))
    pts = [(x, y) for x in np.arange(x0 + step / 2, x1, step) for y in np.arange(y0 + step / 2, y1, step)]
    pts = [p for p in pts if poly.contains(Point(p))] or [poly.representative_point().coords[0]]
    sh, pals = [], {}
    for x, y in pts:
        s, p = lightat(x / B, y / B, z, zone, (0, 0, 1) if up else (0, 0, -1))
        sh.append(s); pals[p] = pals.get(p, 0) + 1
    return int(round(sum(sh) / len(sh))), max(pals, key=pals.get)

# ================================================================ sectors, walls
def ring_pts(coords):
    pts = [(int(round(x)), int(round(y))) for x, y in coords[:-1]]
    out = []
    for p in pts:                                   # drop repeats
        if not out or out[-1] != p: out.append(p)
    if len(out) > 1 and out[0] == out[-1]: out.pop()
    changed = True                                  # drop collinear points
    while changed and len(out) > 3:
        changed = False
        for i in range(len(out)):
            a, b, c = out[i - 1], out[i], out[(i + 1) % len(out)]
            if (b[0] - a[0]) * (c[1] - b[1]) - (b[1] - a[1]) * (c[0] - b[0]) == 0:
                out.pop(i); changed = True; break
    return out

def signed(pts):
    return sum(pts[i][0] * pts[(i + 1) % len(pts)][1] - pts[(i + 1) % len(pts)][0] * pts[i][1] for i in range(len(pts)))

def build():
    pieces = overlay()
    pieces = cut_pools(pieces)
    rooms, solids = [], []
    for pg, pr in pieces:
        for p in polys(pg):
            if p.area < 64: continue
            (rooms if pr.kind == 'room' else solids).append((p, pr))
    # loops, oriented for Build (outer clockwise on the y-down map = positive shoelace; holes negative)
    secs = []
    for p, pr in rooms:
        loops = []
        for k, ring in enumerate([p.exterior] + list(p.interiors)):
            pts = ring_pts(list(ring.coords))
            if len(pts) < 3: continue
            if (signed(pts) > 0) != (k == 0): pts.reverse()
            loops.append(pts)
        if loops: secs.append(dict(poly=p, reg=pr, loops=loops))
    # node: split every edge at the vertices of other loops lying on it, so neighbours share their walls exactly
    allv = np.array(sorted({v for s in secs for l in s['loops'] for v in l} |
                           {(int(round(x)), int(round(y))) for p, _ in solids for ring in [p.exterior] + list(p.interiors)
                            for x, y in ring.coords}), float)
    for s in secs:
        for li, l in enumerate(s['loops']):
            out = []
            for i, a in enumerate(l):
                b = l[(i + 1) % len(l)]
                out.append(a)
                ax, ay = a; bx, by = b
                L2 = (bx - ax) ** 2 + (by - ay) ** 2
                t = ((allv[:, 0] - ax) * (bx - ax) + (allv[:, 1] - ay) * (by - ay)) / L2
                dist = np.abs((allv[:, 0] - ax) * (by - ay) - (allv[:, 1] - ay) * (bx - ax)) / math.sqrt(L2)
                m = (t > 1e-6) & (t < 1 - 1e-6) & (dist <= 1.01)
                for j in np.argsort(t[m]):
                    v = tuple(int(c) for c in allv[m][j])
                    if v != out[-1] and v != b: out.append(v)
            s['loops'][li] = out
    # plain one-sided walls are cut into short lengths, so the light can vary along them (painted faces stay whole)
    edges = {(a, l[(i + 1) % len(l)]) for s in secs for l in s['loops'] for i, a in enumerate(l)}
    from shapely.strtree import STRtree
    tree = STRtree([p for p, _ in solids]) if solids else None
    def painted_solid(x, y):
        if tree is None: return False
        pt = Point(x, y)
        return any(solids[i][1].p.get('faces') and solids[i][0].covers(pt) for i in tree.query(pt))
    for s in secs:
        for li, l in enumerate(s['loops']):
            out = []
            for i, a in enumerate(l):
                b = l[(i + 1) % len(l)]
                out.append(a)
                if (b, a) in edges: continue
                L = math.hypot(b[0] - a[0], b[1] - a[1])
                n = int(L // (1.0 * B))
                if n < 1: continue
                tx, ty = (b[0] - a[0]) / L, (b[1] - a[1]) / L
                if painted_solid((a[0] + b[0]) / 2 + ty * 24, (a[1] + b[1]) / 2 - tx * 24): continue
                for k in range(1, n + 1):
                    out.append((int(round(a[0] + (b[0] - a[0]) * k / (n + 1))), int(round(a[1] + (b[1] - a[1]) * k / (n + 1)))))
            s['loops'][li] = out
    return secs, solids

# ---------------------------------------------------------------- wall texturing
def face_key(reg, nx, ny):
    """which face of a region a wall is on, from the wall's outward normal (pointing away from the region)"""
    fr = reg.p.get('frame')
    deg = fr[4] if fr and len(fr) > 4 and fr[4] else 0
    a = math.radians(deg)   # undo the blockout rotation (plan rotation is -deg)
    lx, ly = nx * math.cos(a) - ny * math.sin(a), nx * math.sin(a) + ny * math.cos(a)
    if abs(lx) >= abs(ly): return 'e' if lx > 0 else 'w'
    return 's' if ly > 0 else 'n'

def face_u(reg, key, x, y):
    """0..1 across a face, left to right as seen from outside"""
    x0, y0, x1, y1 = reg.p['frame'][:4]
    deg = reg.p['frame'][4] if len(reg.p['frame']) > 4 and reg.p['frame'][4] else 0
    if deg:
        x, y = rot_plan(x, y, -deg, reg.p['frame'][5])
    if key == 'e': return (y1 - y) / (y1 - y0)
    if key == 'w': return (y - y0) / (y1 - y0)
    if key == 'n': return (x1 - x) / (x1 - x0)
    return (x - x0) / (x1 - x0)

def on_face(reg, key, x, y, tol=0.06):
    """is this point on that face of the region's frame (and not on an inner edge, like the truck's insides)?"""
    x0, y0, x1, y1 = reg.p['frame'][:4]
    deg = reg.p['frame'][4] if len(reg.p['frame']) > 4 and reg.p['frame'][4] else 0
    if deg:
        x, y = rot_plan(x, y, -deg, reg.p['frame'][5])
    return abs({'e': x - x1, 'w': x - x0, 'n': y - y0, 's': y - y1}[key]) < tol

def painted(mx, my, facing):
    tex = None
    for x0, y0, x1, y1, f, t in PAINT:
        if x0 <= mx <= x1 and y0 <= my <= y1 and (f is None or f == facing) and t:
            tex = t
    return tex

def facing_of(nx, ny):
    if abs(nx) >= abs(ny): return 'e' if nx > 0 else 'w'
    return 's' if ny > 0 else 'n'

# ================================================================ write
def write():
    secs, solids = build()
    sky = made('sky', sky_tile())
    TEXPIC = {}
    def tex_pic(name):
        if name == 'sky': return sky
        if name == 'tube_on': return made('tube_on', flat_tile((235, 240, 245)))
        if name == 'tube_dead': return made('tube_dead', flat_tile((92, 94, 98), noise=0.05))
        return tile(name)

    # which piece is where (for sprites and for the far side of one-sided walls)
    from shapely.strtree import STRtree
    room_tree = STRtree([s['poly'] for s in secs])
    solid_tree = STRtree([p for p, _ in solids]) if solids else None
    def room_at(x, y):
        pt = Point(x, y)
        for i in room_tree.query(pt):
            if secs[i]['poly'].covers(pt): return i
        return None
    def solid_at(x, y):
        if solid_tree is None: return None
        pt = Point(x, y)
        for i in solid_tree.query(pt):
            if solids[i][0].covers(pt): return solids[i][1]
        return None

    # walls
    walls, sectors = [], []
    edge = {}
    for si, s in enumerate(secs):
        s['wallptr'] = len(walls)
        for l in s['loops']:
            first = len(walls)
            for i, a in enumerate(l):
                b = l[(i + 1) % len(l)]
                walls.append(dict(a=a, b=b, sec=si, point2=first + (i + 1) % len(l)))
                edge[(a, b)] = len(walls) - 1
        s['wallnum'] = len(walls) - s['wallptr']
    unmatched = 0
    for wi, w in enumerate(walls):
        j = edge.get((w['b'], w['a']))
        w['next'] = j if j is not None else -1
        w['nsec'] = walls[j]['sec'] if j is not None else -1

    # floors and ceilings: heights, textures, shades
    for s in secs:
        p = s['reg'].p
        zone = p['zone']
        fs, fp = area_shade(s['poly'], p['floor'], zone, True)
        cs, cp = area_shade(s['poly'], p['ceil'], zone, False)
        s.update(fz=-int(round(p['floor'] * ZU)), cz=-int(round(p['ceil'] * ZU)), fs=fs, fp=fp, cs=cs, cp=cp)
        if p['sky']: s['cs'], s['cp'] = 4, 0          # one sky everywhere
        if p.get('tube') == 'on': s['cs'] = 0
        if p.get('tube') == 'flicker': s['cs'] = 20
        s['vis'] = 240 if p.get('glow') or p['zone'] == 'frost' else 0

    # wall pictures
    split_fit = []
    for wi, w in enumerate(walls):
        s = secs[w['sec']]; p = s['reg'].p
        (ax, ay), (bx, by) = w['a'], w['b']
        L = math.hypot(bx - ax, by - ay)
        tx, ty = (bx - ax) / L, (by - ay) / L
        nx, ny = -ty, tx                        # Build walls keep their sector on the right of a -> b: the face looks there
        mx, my = (ax + bx) / 2, (ay + by) / 2
        facing = facing_of(nx, ny)
        w['cstat'] = 0
        other = None; low = up = False
        if w['next'] >= 0:
            o = secs[w['nsec']]; op = o['reg'].p
            low = op['floor'] > p['floor'] + 1e-6
            up = op['ceil'] < p['ceil'] - 1e-6 and not (op['sky'] and p['sky'])
            other = o['reg']
            zspan = (p['floor'], op['floor']) if low else (op['ceil'], p['ceil'])
            ref = 'step' if low else 'up'
        else:
            other = solid_at(mx - nx * 24, my - ny * 24)
            zspan = (p['floor'], p['ceil'])
            ref = 'one'
        name, fit = None, None
        pt = painted(mx / B, my / B, facing)
        if other is not None and other.p.get('faces') and (ref != 'up'):
            key = face_key(other, nx, ny)
            f = other.p['faces'].get(key)
            if f and on_face(other, key, mx / B, my / B):
                fit = (other, key)
                name = f
        if name is None:
            if ref == 'one' and p.get('walltex'): name = p['walltex']
            elif ref in ('one', 'up') and pt: name = pt
            elif other is None: name = 'brick'
            elif other.kind == 'solid': name = other.p['tex']
            else: name = other.p['side'] if (low or up) else 'brick'
        w['name'] = name
        if fit:
            reg, key = fit
            if isinstance(name, tuple):           # a texture made here: (name, image)
                pic = fitwall(name[0], name[1]); tname = name[0]
            else:
                pic = fitwall(name); tname = name
            tw, th = tsize(pic)
            u0, u1 = face_u(reg, key, ax / B, ay / B), face_u(reg, key, bx / B, by / B)
            if abs((u1 - u0) - 1) > 0.02: split_fit.append((tname, round(u0, 2), round(u1, 2)))
            w['xrepeat'] = max(1, min(255, round(tw * abs(u1 - u0) / 8)))
            w['xpanning'] = int(round(tw * min(u0, u1))) % 256
            h = (zspan[1] - zspan[0])
            w['yrepeat'] = max(1, min(255, round(th * 2048 / (h * ZU)))) if h > 0 else 8
            w['ypanning'] = 0
            if ref == 'one': w['cstat'] |= 4            # bottom of the picture on the floor
            w['pic'] = pic
            w['glow'] = glows(tname)
        else:
            if isinstance(name, tuple): name = name[0]
            pic = tex_pic(name)
            tw, th = tsize(pic)
            tu = texel_units(name)
            w['xrepeat'] = max(1, min(255, round(L / (8 * tu))))
            u = ax * tx + ay * ty
            w['xpanning'] = int(u / tu) % tw % 256
            w['yrepeat'] = max(1, min(255, round(128 / tu)))
            w['ypanning'] = 0
            if ref == 'one': w['cstat'] |= 4
            w['pic'] = pic
            w['glow'] = False
        # shade from the lights, at the middle of what shows
        zone = p['zone']
        zmid = max(0.3, min(1.6, (zspan[0] + zspan[1]) / 2))
        sh, pal = lightat(mx / B + nx / B * 8, my / B + ny / B * 8, zmid, zone, (nx, ny, 0))
        if w['glow']: sh, pal = 0, 0
        w['shade'], w['pal'] = sh, pal
    if split_fit:
        print('note: painted faces split over several walls:', split_fit[:12], '...' if len(split_fit) > 12 else '')

    # sprites
    sprites = []
    def add_sprite(x, y, z, pic, cstat, xr, yr, ang=0, shade=0, pal=0, clip=32, lotag=0, hitag=0, sect=None):
        sect = room_at(x, y) if sect is None else sect
        if sect is None:
            print('warning: sprite outside every sector at', round(x / B, 2), round(y / B, 2)); return
        if cstat >= 32768: cstat -= 65536
        sprites.append(struct.pack('<iiihhbBBBBBbbhhhhhhhhhh', int(x), int(y), int(z), cstat, pic, shade, pal, clip, 0,
                                   max(1, min(255, int(round(xr)))), max(1, min(255, int(round(yr)))), 0, 0, sect, 0,
                                   int(ang) & 2047, -1, 0, 0, 0, lotag, hitag, 0))

    def bang(rad):
        return int(round(rad / (2 * math.pi) * 2048)) & 2047
    FACE_ANG = {'e': 0, 's': 512, 'w': 1024, 'n': 1536}
    actors = {}
    for sp in SPRITES:
        k = sp['kind']
        x, y = sp['x'] * B, sp['y'] * B
        z = -sp['z'] * ZU
        zone = 'yard'
        sec = room_at(x, y) if k != 'floor' else None
        if sec is not None: zone = secs[sec]['reg'].p['zone']
        if k == 'floor':
            x0, y0, x1, y1 = MAN['_floor'][sp['tex']] if sp['tex'] in MAN['_floor'] else (None,) * 4
            if x0 is not None:
                x, y = (x0 + x1) / 2 * B, (y0 + y1) / 2 * B
                wbu, hbu, top = x1 - x0, y1 - y0, 'n'
            else:
                wbu, hbu, top = sp['w'], sp['h'], sp['top']
            im = None
            if sp['tex'] == 'paint_bays':
                im = src('paint_bays'); a = np.asarray(im).copy()
                hole = rrect(3.9, 11.9, 6.7, 22.2, *TR)
                H_, W_ = a.shape[:2]
                for v in range(H_):
                    for u in range(W_):
                        px_ = (x0 + (u + 0.5) / W_ * (x1 - x0)) * B; py_ = (y0 + (v + 0.5) / H_ * (y1 - y0)) * B
                        if hole.contains(Point(px_, py_)): a[v, u, 3] = 0
                im = Image.fromarray(a)
            pic = spritetile(sp['tex'], im=im, glow=sp.get('glow'))
            tw, th = tsize(pic)
            # the picture's top points away from the front it's read from
            topdir = {'n': 512, 's': 1536, 'e': 1024, 'w': 0}[top] if sp.get('top') else 1536
            if x0 is not None: topdir = 1536
            sec = room_at(x, y)
            zone = secs[sec]['reg'].p['zone'] if sec is not None else 'yard'
            sh, pal = lightat(x / B, y / B, sp['z'], 'glow' if sp.get('glow') else zone, (0, 0, 1))
            z = -(sp['z'] * ZU) - 64
            add_sprite(x, y, z, pic, 32 | 2048, wbu * 2048 / tw, hbu * 2048 / th, ang=topdir - 1024, shade=sh, pal=pal,
                       sect=sec)
            continue
        if k == 'moped':
            pics = [spritetile(f"{sp['tex']}_{i}") for i in range(5)]
            e = MAN[f"{sp['tex']}_0"]
            tw, th = tsize(pics[0])
            r = e['h'] * 2048 / th
            sh, pal = lightat(sp['x'], sp['y'], 0.5, zone)
            actors.setdefault('MOPED_' + sp['tex'].split('_')[-1].upper(), pics[0])
            add_sprite(x, y, z, pics[0], 1 | 256 | 2048, r, r, ang=sp['heading'] / 360 * 2048, shade=sh, pal=pal, clip=48)
            continue
        if k == 'door':
            # a door leaf: two one-sided wall sprites back to back, centred along its length from the hinge
            ang = sp['ang']
            dx, dy = math.cos(ang), math.sin(ang)
            cx, cy = x + dx * sp['length'] * B / 2, y + dy * sp['length'] * B / 2
            for tex, side in [(sp['tex'], 1), (sp['back'], -1)]:
                pic = fitwall(tex) if MAN.get(tex, {}).get('mode') == 'fit' else tile(tex)
                tw, th = tsize(pic)
                # the face looks along the leaf's normal; the sprite's plane runs along its angle + 90
                nrm = ang - math.pi / 2 * side
                ox, oy = math.cos(nrm) * 4, math.sin(nrm) * 4
                sh, pal = lightat((cx + ox * 6) / B, (cy + oy * 6) / B, 1.2, zone, (math.cos(nrm), math.sin(nrm), 0))
                add_sprite(cx + ox, cy + oy, z, pic, 16 | 64 | 1 | 256, sp['length'] * 2048 / tw, sp['h'] * 2048 / th,
                           ang=bang(nrm), shade=sh, pal=pal)
            continue
        if k == 'trigger':
            pic = made('trigger', np.full((16, 16), 255, np.uint8))
            ang = (FACE_ANG[sp['facing']] + 1024) & 2047        # it looks into the cabinet, the way you do using it
            add_sprite(x, y, z, pic, 16 | 256 | 32768, 64 * 1.0 * 2048 / 16 / 64, 0.9 * 2048 / 16, ang=ang)
            actors[sp['actor']] = pic
            continue
        # face and wall sprites
        if sp['tex'] == 'wallpack':
            pic = made('wallpack', wallpack_tile())
            tw, th = tsize(pic)
            xr, yr = sp['w'] * 2048 / tw, sp['h'] * 2048 / th
        else:
            pic = spritetile(sp['tex'], glow=sp.get('glow'))
            e = MAN[sp['tex']]
            tw, th = tsize(pic)
            h = sp.get('h') or e['h']
            xr = yr = h * 2048 / th
        normal = None
        cstat = 2048
        ang = 0
        if k == 'wall':
            cstat = 16 | 64
            ang = FACE_ANG[sp['facing']]
            normal = (math.cos(ang / 2048 * 2 * math.pi), math.sin(ang / 2048 * 2 * math.pi), 0)
        if sp.get('block'):
            cstat |= 1 | 256
        sh, pal = lightat(sp['x'], sp['y'], sp['z'] + 0.5, zone, normal)
        if sp.get('glow') or glows(sp['tex']): sh, pal = 0, 0
        add_sprite(x, y, z, pic, cstat, xr, yr, ang=ang, shade=sh, pal=pal, clip=36)
        if sp.get('actor'): actors[sp['actor']] = pic

    # the flicker: Duke's random-lights effect (SE 4) in the pool under the flickering tube, and in the tube itself
    flick = [L for L in LIGHTS if L['tag'] == 'flicker'][0]
    fx, fy = flick['p'][0] * B, flick['p'][1] * B
    tube_sec = room_at(fx, 5.75 * B)
    pool_sec = None
    for si, s in enumerate(secs):
        if s['reg'].p['zone'] == 'corridor' and s['reg'].p.get('cut') and s['poly'].covers(Point(fx, fy + 0.5 * B)):
            pool_sec = si
    for si in (tube_sec, pool_sec):
        if si is None: continue
        sx, sy = secs[si]['poly'].representative_point().coords[0]
        add_sprite(sx, sy, secs[si]['fz'], 1, 0, 32, 32, shade=0, lotag=4, hitag=0, sect=si)
        secs[si]['fs'] = max(secs[si]['fs'], 20)
    if tube_sec is not None: secs[tube_sec]['cs'] = 22

    # the player, in the truck, facing out of the back
    sx, sy = START[0] * B, START[1] * B
    pang = bang(math.atan2(START_DIR[1] - START[1], START_DIR[0] - START[0]))
    start_sec = room_at(sx, sy)
    pz = -int(FLOOR * ZU)
    sprites.insert(0, struct.pack('<iiihhbBBBBBbbhhhhhhhhhh', int(sx), int(sy), pz, 1 | 256, 1405, 0, 0, 32, 0,
                                  42, 36, 0, 0, start_sec, 0, pang, -1, 0, 0, 0, 0, 0, 0))

    # sectors out
    secb = []
    for s in secs:
        p = s['reg'].p
        cpic = tex_pic(p['ctex']); fpic = tex_pic(p['ftex'])
        cstat = 1 if p['sky'] else 0
        secb.append(struct.pack('<hhiihhhhbBBBhhbBBBBBhhh', s['wallptr'], s['wallnum'], s['cz'], s['fz'],
                                cstat, 0, cpic, 0, s['cs'], s['cp'], 0, 0, fpic, 0, s['fs'], s['fp'], 0, 0,
                                s['vis'], 0, 0, 0, 0))
    wallb = []
    for w in walls:
        x, y = w['a']
        wallb.append(struct.pack('<iihhhhhhbBBBBBhhh', x, y, w['point2'], w['next'], w['nsec'], w['cstat'], w['pic'], 0,
                                 w['shade'], w['pal'], w['xrepeat'], w['yrepeat'], w['xpanning'], w['ypanning'], 0, 0, 0))
    out = struct.pack('<iiiihhH', 7, int(sx), int(sy), pz - 9728, pang, start_sec, len(secb))
    out += b''.join(secb)
    out += struct.pack('<H', len(wallb)) + b''.join(wallb)
    out += struct.pack('<H', len(sprites)) + b''.join(sprites)
    open(os.path.join(GAME, 'INTRO.MAP'), 'wb').write(out)
    print(f'game/INTRO.MAP: {len(secb)} sectors, {len(wallb)} walls, {len(sprites)} sprites')
    nred = sum(1 for w in walls if w['next'] >= 0)
    print(f'  {nred} red walls')
    return secs, walls, actors

def write_art():
    tiles = sorted(TILES.values(), key=lambda t: t[0])
    path = os.path.join(GAME, 'textures', 'intro.art')
    with open(path, 'wb') as f:
        f.write(struct.pack('<4i', 1, len(tiles), tiles[0][0], tiles[-1][0]))
        for _, px, _ in tiles: f.write(struct.pack('<h', px.shape[1]))
        for _, px, _ in tiles: f.write(struct.pack('<h', px.shape[0]))
        for _, _, pa in tiles: f.write(struct.pack('<I', pa))
        for _, px, _ in tiles: f.write(np.ascontiguousarray(px.T).tobytes())
    print(f'game/textures/intro.art: {len(tiles)} tiles, {tiles[0][0]}..{tiles[-1][0]}')

def write_con(actors):
    lines = ['// Generated by tools/mkintro.py: tile numbers for the intro map\'s actors. Their code is in EMINE.CON.']
    for name, pic in sorted(actors.items()):
        lines.append(f'define {name}_TILE {pic}')
    open(os.path.join(GAME, 'intro.con'), 'w').write('\n'.join(lines) + '\n')
    print('game/intro.con:', ', '.join(sorted(actors)))

def plan(secs, walls):
    """a top-down picture of the sectors, filled with their floor shade and tint, for checking"""
    S = 40
    im = Image.new('RGB', (int(48 * S), int(31 * S)), (0, 0, 0))
    d = ImageDraw.Draw(im)
    tint = {0: (1, 1, 1), P_SODIUM: (1, .7, .35), P_MINE: (.4, 1, .8), P_NEON: (1, .4, .8), P_STREET: (.6, .75, 1),
            P_FIRE: (1, .5, .25)}
    for s in secs:
        v = (31 - s['fs']) / 31
        c = tuple(int(255 * v * t) for t in tint.get(s['fp'], (1, 1, 1)))
        for k, l in enumerate(s['loops']):
            d.polygon([(x / B * S, y / B * S) for x, y in l], fill=c if k == 0 else None)
    for w in walls:
        (ax, ay), (bx, by) = w['a'], w['b']
        col = (200, 40, 40) if w['next'] >= 0 else (240, 240, 240)
        if w['next'] >= 0 and secs[w['sec']]['reg'].p['floor'] == secs[w['nsec']]['reg'].p['floor']:
            col = (90, 30, 30)
        d.line((ax / B * S, ay / B * S, bx / B * S, by / B * S), fill=col, width=1)
    out = os.path.join(ROOT, 'art', 'intro', 'renders', 'build_plan.png')
    im.save(out)
    print(os.path.relpath(out, ROOT))

if __name__ == '__main__':
    secs, walls, actors = write()
    write_art()
    write_con(actors)
    if '--plan' in sys.argv: plan(secs, walls)
