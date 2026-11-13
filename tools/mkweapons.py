"""Our own first-person weapons, kick and hands, in the same places as Duke's: art/weapons -> game/textures/weapons_*.art

Run: python tools/mkweapons.py guides   (layout guides from Duke's tiles: flat colour regions only, no detail)
     python tools/mkweapons.py gen [tile...]   (fal nano-banana-2/edit, $0.08 an image; only makes what's missing)
     python tools/mkweapons.py build    (fit, key, palette-match, silhouette check, write the ART files)

Each tile is painted by fal from a guide: Duke's tile reduced to a few flat colours (where the hand, the gun and the
flash go), so ours has the same size, place and silhouette but none of the original's drawing. Other frames of a
weapon also get its first frame's painting as a reference, so the weapon looks the same throughout. Every result's
URL is saved to art/weapons/raw/<tile>.url before it's downloaded.

The tiny bits (crosshair, shell casings, muzzle flashes, the chaingun's hot barrels) are drawn here, locally.
Each tile keeps Duke's size and offsets, so the game draws ours exactly where it drew his. `build` prints how well
each silhouette matches Duke's (intersection over union); below 0.75 it's flagged for another go.
"""
import json, os, struct, sys
import numpy as np
from PIL import Image, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, '..')
sys.path.insert(0, HERE)
import mksprite, mkfont

ART = os.path.join(ROOT, 'art', 'weapons')
GUIDE, RAW = os.path.join(ART, 'guide'), os.path.join(ART, 'raw')
OUT = os.path.join(ROOT, 'game', 'textures')
MAGENTA = np.array([255, 0, 255])

HAND = 'a tanned muscular man\'s bare hand'
BEHIND = ('seen from directly behind and a little above, the way the shooter sees it: the flat back of the squared-off slide faces the viewer and the barrel points straight ahead into the distance, so the gun is foreshortened to a chunky block')
STYLE = ('Original artwork for a 1990s first-person shooter: a pre-rendered 3D look, chunky readable shapes, '
         'strong light from the upper left, crisp edges. The first image is a flat-colour layout guide: paint exactly '
         'the same silhouette, at the same size and position, filling each flat region with detailed, shaded '
         'material ({what}). Anything running off the edge of the guide runs off the edge of the picture. Flat solid '
         'magenta (#FF00FF) background everywhere outside the silhouette: no shadow, no floor, no text, no border. '
         'Paint every region fully, skin included: never leave the flat colours of the guide showing. Nothing bigger or '
         'smaller than in the guide.')
SAME = (' The second image is the finished painting of this same {kind}: keep its exact design, materials, colours '
        'and lighting, only change the pose to match the guide.')

# tile: (what to paint, weapon kind, key tile for that weapon or None if it is the key, muzzle flash in it)
TILES = {
    2324: (HAND + 's, fingers interlocked and pushed outward, cracking the knuckles, the two forearms slanting down and out to the bottom left and bottom right corners as in the guide, with open magenta between them', 'pair of hands', None, False),
    2325: (HAND + 's, fingers interlocked, cracking the knuckles, forearms running off the bottom', 'pair of hands', 2324, False),
    2326: (HAND + 's, fingers interlocked, cracking the knuckles, forearms running off the bottom', 'pair of hands', 2324, False),
    2327: (HAND + 's, fingers interlocked, cracking the knuckles, forearms running off the bottom', 'pair of hands', 2324, False),
    2521: ('a kicking leg seen from the kicker\'s own eyes: faded blue denim jeans and a scuffed black leather work boot with a thick tread sole, the boot sole toward the viewer', 'leg', None, False),
    2522: ('a leg seen from the kicker\'s own eyes, looking down along it: faded blue denim jeans and a scuffed black leather work boot with laces', 'leg', 2521, False),
    2524: (HAND + ' from behind, gripping a chunky black semi-automatic pistol ' + BEHIND, 'pistol', None, False),
    2525: (HAND + ' gripping a chunky black semi-automatic pistol ' + BEHIND + '; it is firing and kicking up, a bright yellow-orange muzzle flash bursting from the far end', 'pistol', 2524, True),
    2526: (HAND + ' gripping a chunky black semi-automatic pistol ' + BEHIND + '; just after firing, a fading orange muzzle flash at the far end', 'pistol', 2524, True),
    2528: (HAND + ' holding a chunky black semi-automatic pistol tipped sideways to reload, the empty magazine well showing', 'pistol', 2524, False),
    2529: (HAND + ' holding a chunky black semi-automatic pistol tipped steeply over on its side to reload, the gun small at the top left as in the guide, wrist and forearm running off the bottom right', 'pistol', 2524, False),
    2530: ('a single black pistol magazine, slightly angled', 'pistol magazine', 2524, False),
    2531: (HAND + ' holding a black pistol magazine between fingers and thumb', 'pistol magazine', 2524, False),
    2532: (HAND + ', open, fingers spread, palm toward the viewer, as if slapping a magazine home', 'hand', 2524, False),
    2536: (HAND + ' gripping the side of a heavy black rotary machine gun receiver that runs off the right edge, chunky ribbed metal casing', 'machine gun', None, False),
    2537: ('the muzzle end of a heavy rotary machine gun: a cluster of steel barrel ends held by a dark metal clamp ring, seen from behind and above', 'machine gun', 2536, False),
    2544: (HAND + ' gripping the grip of a big shoulder-fired rocket launcher: a thick olive-drab and gunmetal tube with riveted bands and a tan stripe, receding forward', 'rocket launcher', None, False),
    2545: ('the open front end of a rocket launcher tube with a bright yellow-orange blast of fire bursting out of it', 'rocket launcher', 2544, True),
    2546: ('the open front end of a rocket launcher tube with a big round fireball of yellow-orange flame and black smoke bursting out of it', 'rocket launcher', 2544, True),
    2570: (HAND + ' clenched around a small black detonator with a red push button on top, arm reaching in from the bottom left', 'detonator', None, False),
    2571: (HAND + ' clenched around a small black detonator, thumb pressing its red button, arm from the bottom left', 'detonator', 2570, False),
    2572: (HAND + ' clenched around a small black detonator, thumb lifting off its red button, arm from the bottom left', 'detonator', 2570, False),
    2573: (HAND + ' from below holding up a squat steel pipe bomb: a grey metal canister with yellow and black hazard stripes on its end caps and a small rubber-banded fuse box on the side', 'pipe bomb', None, False),
    2574: (HAND + ' throwing a steel pipe bomb, seen close up on its side: a grey metal canister with yellow and black hazard stripes on its end caps and a small label', 'pipe bomb', 2573, False),
    2575: (HAND + ' clenched in a fist just after throwing, knuckles toward the viewer', 'hand', 2573, False),
    2613: (HAND + ' gripping an all-black pump-action shotgun at its pistol grip, the barrel and pump pointing forward and up', 'shotgun', None, False),
    2616: (HAND + 's racking an all-black pump-action shotgun: one hand pulling the pump back, the gun angled up', 'shotgun', 2613, False),
    2617: (HAND + 's racking an all-black pump-action shotgun held steeply upright, barrel pointing up and slightly right exactly as in the guide, one fist on the pump at the left', 'shotgun', 2613, False),
    2618: (HAND + ' loading a red shotgun shell into the loading port of an all-black pump-action shotgun held upright', 'shotgun', 2613, False),
    2619: (HAND + ' pushing a red shotgun shell into the loading port of an all-black pump-action shotgun held upright', 'shotgun', 2613, False),
    # ammo pickups: what the ores drop, lying in the world (and the ammo icons on the HUD)
    40: ('an ammo pickup for the game: a single tall black pistol magazine standing upright, a brass cartridge showing at its top', 'pistol magazine', 2524, False),
    49: ('an ammo pickup for the game: an open plain cardboard box of red shotgun shells with brass heads, a couple of loose shells beside it, no writing', 'shotgun shells', None, False),
    41: ('an ammo pickup for the game: an olive-green steel ammunition can with its lid hinged open, a belt of brass machine-gun cartridges draped out over its side', 'ammunition can', None, False),
    47: ('an ammo pickup for the game: a low olive-green steel crate with steel pipe bombs (grey canisters with yellow and black hazard-striped end caps) lying on top of it; no hands, no people', 'pipe bomb', None, False),
    44: ('an ammo pickup for the game: a low olive-green steel crate with rockets (brass bodies, blue-grey pointed warheads) standing in it and leaning out of it', 'rocket crate', None, False),
    37: ('an ammo pickup for the game: a squat insulated canister of freezing coolant, white and pale icy blue with frost on it, a glowing blue window band round its middle and a steel cap and base', 'canister', None, False),
    # things in the world: the pipe bomb thrown and lying there, the rocket in flight (7 views), and HUD icons
    26: ('a squat steel pipe bomb lying on its side on the ground: a grey metal canister with yellow and black hazard-striped end caps and a small fuse box strapped to it; no hands, no people', 'pipe bomb', None, False),
    2608: ('a rocket in flight seen from the side, in the same pose as the guide (nose and tail where its shape puts them): brass body, blue-grey pointed warhead, small dark steel fins at the tail; the rockets in the second image show the design', 'rocket', 44, False),
    2605: ('a rocket in flight seen nose-on, coming straight at the viewer: the pointed blue-grey warhead in the middle, four small dark fins around it in an X', 'rocket', 2608, False),
    2606: ('a rocket in flight seen from the front at an angle, foreshortened, as in the guide: brass body, blue-grey pointed warhead, small dark steel fins at the tail', 'rocket', 2608, False),
    2607: ('a rocket in flight seen from the side and a little from the front, as in the guide: brass body, blue-grey pointed warhead, small dark steel fins at the tail', 'rocket', 2608, False),
    2609: ('a rocket in flight seen from the side and a little from behind, as in the guide, a bright exhaust flame at its tail: brass body, blue-grey pointed warhead, small dark steel fins at the tail', 'rocket', 2608, True),
    2610: ('a rocket in flight seen from behind at an angle, foreshortened, a bright yellow-orange exhaust flame at its tail', 'rocket', 2608, True),
    2611: ('a rocket in flight seen straight from behind: four small dark fins in an X around a bright yellow-orange exhaust flame', 'rocket', 2608, True),
    # the devastator (lapis): the shareware has no art for it, so our own layout guide; the game draws it in the right
    # hand and mirrored in the left
    2510: (HAND + ' gripping ' + "a chunky futuristic twin-tube mini-rocket launcher (gunmetal body with lapis-blue armour panels, two short rocket tubes side by side on top), held low at the bottom right of the view and pointing forward and a little left, seen from behind the shooter", 'rocket launcher', None, False),
    2511: (HAND + ' gripping ' + "a chunky futuristic twin-tube mini-rocket launcher (gunmetal body with lapis-blue armour panels, two short rocket tubes side by side on top), held low at the bottom right of the view and pointing forward and a little left, seen from behind the shooter" + '; it is firing, bright yellow-orange flashes at the mouths of both tubes', 'rocket launcher', 2510, True),
    42: ('an ammo pickup for the game: a small lapis-blue steel box with its lid open, packed with a row of small brass mini rockets with blue-grey tips', 'ammo box', None, False),
    51: ('a game HUD icon: a chunky glossy red heart with a bright highlight, filling the guide', 'heart icon', None, False),
    54: ('a game HUD icon: a riveted iron chest plate (body armour), front view, filling the guide', 'armour icon', None, False),
}
# world sprites and icons: not padded (a world sprite's bottom edge sits on the floor; icons keep their size)
PICKUPS = (37, 40, 41, 42, 44, 47, 49, 26, 51, 54, 2605, 2606, 2607, 2608, 2609, 2610, 2611)
LOCAL = [2523, 2533, 2534, 2535, 2538, 2539, 2540, 2541, 2542, 2543, 2614, 2615]
GROUPS = [(2324, 2327), (2521, 2526), (2528, 2546), (2570, 2575), (2613, 2619), (37, 38), (40, 41), (44, 44), (47, 47), (49, 49),
          (26, 26), (51, 51), (54, 54), (2605, 2611), (42, 42), (2510, 2511)]
COPIES = {38: 37}
FLIP = {2608}  # painted facing the other way from the rocket's other views: mirrored to match  # the HUD shows freezer ammo as FREEZEAMMO+1 (empty in the shareware): the same canister  # one ART file each (consecutive tiles)
ASPECTS = [(21, 9), (16, 9), (3, 2), (4, 3), (1, 1), (3, 4), (2, 3), (9, 16)]
LONG = 1024  # guide canvas, long side


_tile = None


def duke(n):
    """Duke's tile n: (RGBA array, palette indices, picanm)."""
    global _tile, _pal
    if _tile is None:
        _tile, _pal = mkfont.grp_tiles()
    px, pa = _tile(n)
    if px is None and n == 37:  # the shareware has no freezer ammo: our own outline, a squat canister on the floor
        px = np.full((44, 34), 255, np.uint8)
        yy, xx = np.mgrid[0:44, 0:34]
        body = (np.abs(xx - 16.5) <= 14) & (yy >= 6) & (yy <= 43)
        cap = (np.abs(xx - 16.5) <= 9) & (yy >= 1) & (yy < 6)
        px[body | cap] = 15   # a grey; only the outline matters
        pa = 0
    if n == 38 and px is None:
        return duke(37)
    if n == 42 and px is None:  # devastator ammo: a box on the floor, like the others
        px = np.full((32, 44), 255, np.uint8)
        px[6:32, 2:42] = 15
        pa = 0
    if n in (2510, 2511) and px is None:
        # Devastator: our own layout. A gun body slanting up and left from the bottom right, a hand and forearm on it.
        # Offsets put it low in the right of the screen (the game draws it at x 268, y 238 of 320x200, and a mirrored
        # copy at x 30 for the left hand).
        w, h = 110, 96
        yy, xx = np.mgrid[0:h, 0:w].astype(float)
        a = np.radians(-28)
        u = (xx - 62) * np.cos(a) - (yy - 52) * np.sin(a)
        v = (xx - 62) * np.sin(a) + (yy - 52) * np.cos(a)
        gun = (np.abs(u) < 46) & (np.abs(v) < 17)
        tubes = (np.abs(u + 30) < 22) & (np.abs(v + 21) < 9)
        hand = ((xx - 84) ** 2 / 20 ** 2 + (yy - 74) ** 2 / 16 ** 2) < 1
        arm = (xx > 82) & (yy > 70) & (yy - 70 > (xx - 82) * -0.2)
        grey, blue, skin = (int(np.argmin(((_pal - c) ** 2).sum(1)[:255])) for c in ((95, 95, 100), (60, 90, 170), (200, 150, 110)))
        px = np.full((h, w), 255, np.uint8)
        px[gun] = grey; px[tubes] = blue; px[hand | arm] = skin
        pa = ((-20 & 255) << 8) | ((28 & 255) << 16)  # low in the corner, the cut-off forearm off the bottom and side
    if n in (51, 54):  # HUD icons: our own outlines (a heart, a chest plate) at Duke's icon sizes
        w, h = (23, 16) if n == 51 else (27, 26)
        yy, xx = np.mgrid[0:h, 0:w].astype(float)
        if n == 51:
            u, v = (xx - w / 2 + 0.5) / (w / 2), (h * 0.38 - yy) / (h / 2)
            shape = (u * u + v * v - 1) ** 3 - u * u * v ** 3 <= 0
        else:
            u, v = np.abs(xx - w / 2 + 0.5) / (w / 2), yy / h
            shape = ((v > 0.08) & (u < 0.95) & ~((v < 0.3) & (u < 0.32)) & ~((v < 0.45) & (u > 0.8)) & (v < 0.98))
        px = np.where(shape, 15, 255).astype(np.uint8)
        pa = (pa & ~0xFFFF00) if pa else 0
    rgba = np.zeros(px.shape + (4,), np.uint8)
    rgba[..., :3] = _pal[px]
    rgba[..., 3] = (px != 255) * 255
    return rgba, px, pa


def canvas(w, h):
    """The guide canvas for a w x h tile: (aspect string, canvas size, scale, top-left of the tile in it)."""
    a = min(ASPECTS, key=lambda a: abs(np.log((a[0] / a[1]) / (w / h))))
    cw, ch = (LONG, round(LONG * a[1] / a[0])) if a[0] >= a[1] else (round(LONG * a[0] / a[1]), LONG)
    s = min(0.8 * cw / w, 0.8 * ch / h)
    return '%d:%d' % a, (cw, ch), s, ((cw - w * s) / 2, (ch - h * s) / 2)


def flat(rgba, k=4):
    """Reduce a tile to k flat colours (k-means in LAB), smoothed: where things go, none of how they're drawn."""
    on = rgba[..., 3] > 0
    lab = mksprite.to_lab(rgba[..., :3][on].astype(float))
    rng = np.random.default_rng(1)
    cen = lab[rng.choice(len(lab), k, replace=False)]
    for _ in range(20):
        lbl = ((lab[:, None] - cen[None]) ** 2).sum(2).argmin(1)
        cen = np.array([lab[lbl == i].mean(0) if (lbl == i).any() else cen[i] for i in range(k)])
    rgb = rgba[..., :3][on].astype(float)
    means = np.array([rgb[lbl == i].mean(0) if (lbl == i).any() else [0, 0, 0] for i in range(k)])
    lab_img = np.full(on.shape, -1)
    lab_img[on] = lbl
    # majority filter so regions are blobs, not texture
    from scipy import ndimage
    votes = np.stack([ndimage.uniform_filter((lab_img == i).astype(float), 7) for i in range(k)])
    lab_img = np.where(on, votes.argmax(0), -1)
    out = np.zeros_like(rgba)
    out[on, :3] = means[lab_img[on]].astype(np.uint8)
    out[..., 3] = rgba[..., 3]
    return out


def carry(f, m):
    """Pad by m on every side. Where the shape runs solidly off the bottom edge (an arm leaving the bottom of the
    screen), carry it on into the padding; the other edges are mid-screen, and a stray pixel touching one isn't."""
    from scipy import ndimage
    for side in range(4):  # bottom, left, top, right in turn (rotate so it's always the bottom)
        if side:
            f = np.rot90(np.pad(np.rot90(f, side), ((0, m), (0, 0), (0, 0))), -side)
            continue
        r = np.rot90(f, side)
        edge = r[-1, :, 3] > 0
        runs, k = ndimage.label(edge)
        keep = np.zeros_like(edge)
        for i in range(1, k + 1):
            if (runs == i).sum() >= max(6, 0.12 * len(edge)):
                keep |= runs == i
        last = r[-1:].copy()
        last[0, ~keep] = 0
        f = np.rot90(np.concatenate([r, np.repeat(last, m, 0)], 0), -side)
    return f


def guides():
    os.makedirs(GUIDE, exist_ok=True)
    meta = {}
    for n in TILES:
        rgba, _, _ = duke(n)
        h, w = rgba.shape[:2]
        aspect, (cw, ch), s, (ox, oy) = canvas(w, h)
        f = flat(rgba)
        # carry anything that touches the tile's edge on outward (an arm that runs off the screen keeps going)
        m = int(max(ox, oy) / s) + 2
        f = carry(f, m)
        big = Image.fromarray(f).resize((round((w + 2 * m) * s), round((h + 2 * m) * s)), Image.BICUBIC)
        big = big.filter(ImageFilter.GaussianBlur(s * 0.6))
        bg = Image.new('RGBA', (cw, ch), (255, 0, 255, 255))
        bg.alpha_composite(big, (round(ox - m * s), round(oy - m * s)))
        bg.convert('RGB').save(os.path.join(GUIDE, '%d.png' % n))
        meta[n] = dict(aspect=aspect, size=[cw, ch], scale=s, origin=[ox, oy], w=w, h=h)
    json.dump(meta, open(os.path.join(GUIDE, 'guides.json'), 'w'), indent=1)
    print('wrote %d guides to %s' % (len(meta), os.path.relpath(GUIDE, ROOT)))


def fal_edit(n, prompt, aspect, refs):
    import fal_client, httpx, pathlib
    if not os.environ.get('FAL_KEY'):
        os.environ['FAL_KEY'] = (pathlib.Path.home() / '.fal_key').read_text(encoding='utf-8').strip()
    png, url_file = os.path.join(RAW, '%d.png' % n), os.path.join(RAW, '%d.url' % n)
    if os.path.exists(png):
        return 'have'
    if os.path.exists(url_file):
        url = open(url_file).read().strip()
    else:
        urls = [fal_client.upload_file(p) for p in refs]
        r = fal_client.subscribe('fal-ai/nano-banana-2/edit', arguments={
            'prompt': prompt, 'image_urls': urls, 'num_images': 1, 'resolution': '1K',
            'aspect_ratio': aspect, 'output_format': 'png'})
        url = r['images'][0]['url']
        open(url_file, 'w').write(url + '\n')
    open(png, 'wb').write(httpx.get(url, timeout=120).raise_for_status().content)
    return 'made'


def gen(only):
    import concurrent.futures as cf
    os.makedirs(RAW, exist_ok=True)
    meta = {int(k): v for k, v in json.load(open(os.path.join(GUIDE, 'guides.json'))).items()}
    want = [n for n in TILES if (not only or n in only) and not os.path.exists(os.path.join(RAW, '%d.png' % n))]
    print('%d images to make (about $%.2f)' % (len(want), len(want) * 0.08))

    def one(n):
        what, kind, key, _ = TILES[n]
        prompt = STYLE.format(what=what)
        refs = [os.path.join(GUIDE, '%d.png' % n)]
        if key:
            prompt += SAME.format(kind=kind)
            refs.append(os.path.join(RAW, '%d.png' % key))
        return fal_edit(n, prompt, meta[n]['aspect'], refs)

    for phase in (lambda n: TILES[n][2] is None, lambda n: TILES[n][2] is not None):  # keys first
        todo = [n for n in want if phase(n) and (TILES[n][2] is None or os.path.exists(os.path.join(RAW, '%d.png' % TILES[n][2])))]
        with cf.ThreadPoolExecutor(6) as ex:
            futs = {ex.submit(one, n): n for n in todo}
            for f in cf.as_completed(futs):
                try:
                    print(futs[f], f.result())
                except Exception as e:
                    print('FAILED', futs[f], str(e)[:300])


# --- build ---------------------------------------------------------------------------------------------------------

def palette():
    """(palette, the entries to paint with: all but fullbright, every entry)."""
    pal, usable = mksprite.load_palette()
    return pal, usable, np.arange(255)


def dither(rgb, on, pal, idx, damp=0.5):
    """Palette indices (of idx) for the pixels in `on`, with half-strength Floyd-Steinberg in LAB: smooth skin and metal
    ramps without the pink patches plain nearest-colour matching leaves, and without a heavy dither pattern."""
    lab = mksprite.to_lab(rgb.astype(float)).copy()
    plab = mksprite.to_lab(pal[idx].astype(float))
    h, w = on.shape
    out = np.full((h, w), 255, np.uint8)
    for y in range(h):
        for x in range(w):
            if not on[y, x]:
                continue
            v = lab[y, x]
            i = ((plab - v) ** 2).sum(1).argmin()
            out[y, x] = idx[i]
            e = (v - plab[i]) * damp
            for dx, dy, f in ((1, 0, 7 / 16), (-1, 1, 3 / 16), (0, 1, 5 / 16), (1, 1, 1 / 16)):
                X, Y = x + dx, y + dy
                if 0 <= X < w and Y < h and on[Y, X]:
                    lab[Y, X] += e * f
    return out


def match(rgb, pal, idx):
    """Nearest palette entry (of idx) in LAB for each colour."""
    lab = mksprite.to_lab(rgb.reshape(-1, 3).astype(float))
    plab = mksprite.to_lab(pal[idx].astype(float))
    out = np.empty(len(lab), np.int64)
    for i in range(0, len(lab), 4096):
        out[i:i + 4096] = idx[((lab[i:i + 4096, None] - plab[None]) ** 2).sum(2).argmin(1)]
    return out.reshape(rgb.shape[:-1])


def fit(n, meta):
    """The fal painting at tile scale: (rgb float, alpha 0..1, padding (left, top, right, bottom)). A weapon tile grows
    past Duke's rectangle wherever the painting does (so nothing is cut off at the old edge); top and bottom are padded
    alike, which keeps the offset sums right for tiles the game draws flipped. Pickups keep Duke's rectangle."""
    m = meta[n]
    im = Image.open(os.path.join(RAW, '%d.png' % n)).convert('RGB').resize(tuple(m['size']), Image.LANCZOS)
    a = np.asarray(im).astype(float)
    # alpha from distance to magenta, soft over a band, with the magenta taken back out of the edge colours
    d = np.sqrt(((a - MAGENTA) ** 2).sum(2))
    alpha = np.clip((d - 90) / 90, 0, 1)
    rgb = np.where(alpha[..., None] > 0, (a - (1 - alpha[..., None]) * MAGENTA) / np.maximum(alpha[..., None], 1e-3), 0)
    rgb = np.clip(rgb, 0, 255)
    s, (ox, oy), w, h = m['scale'], m['origin'], m['w'], m['h']
    L = T = R = B = 0
    if n not in PICKUPS:
        ys, xs = np.nonzero(alpha > 0.5)
        L = max(0, int(np.ceil((ox - xs.min()) / s)))
        R = max(0, int(np.ceil((xs.max() + 1 - ox) / s)) - w)
        T = max(0, int(np.ceil((oy - ys.min()) / s)))
        B = max(0, int(np.ceil((ys.max() + 1 - oy) / s)) - h)
        T = B = max(T, B)
    # room round the canvas, so a box past its edge reads as empty
    M = int((max(L, R, T, B) + 2) * s)
    rgb = np.pad(rgb, ((M, M), (M, M), (0, 0)))
    alpha = np.pad(alpha, M)
    W, H = w + L + R, h + T + B
    box = (M + ox - L * s, M + oy - T * s, M + ox + (w + R) * s, M + oy + (h + B) * s)
    chans = [Image.fromarray((rgb[..., c] * alpha).astype(np.float32), 'F') for c in range(3)] + [Image.fromarray((alpha * 255).astype(np.float32), 'F')]
    small = [np.asarray(c.resize((W, H), Image.BOX, box=box)) for c in chans]
    al = small[3] / 255
    col = np.stack(small[:3], -1) / np.maximum(al[..., None], 1e-3)
    return np.clip(col, 0, 255), al, (L, T, R, B)


def flame(alpha, rng, hot=1.0):
    """A muzzle flash filling a silhouette: white-yellow core to red rim, ragged."""
    from scipy import ndimage
    dist = ndimage.distance_transform_edt(alpha > 0)
    t = dist / max(dist.max(), 1)
    t = np.clip(t * hot + rng.normal(0, 0.12, t.shape), 0, 1)
    stops = np.array([[150, 20, 0], [235, 90, 10], [255, 180, 40], [255, 240, 150], [255, 255, 230]], float)
    x = t * (len(stops) - 1)
    i = np.minimum(x.astype(int), len(stops) - 2)
    f = (x - i)[..., None]
    return stops[i] * (1 - f) + stops[i + 1] * f


def local(n, built):
    """Locally drawn tiles: (rgb, alpha) at Duke's tile size, from his silhouette only."""
    rgba, _, _ = duke(n)
    h, w = rgba.shape[:2]
    al = (rgba[..., 3] > 0).astype(float)
    rng = np.random.default_rng(n)
    yy, xx = np.mgrid[0:h, 0:w]
    if n == 2523:  # crosshair: a small plus with a gap, light grey with a dark outline
        al = np.zeros((h, w))
        c = w // 2
        rgb = np.zeros((h, w, 3))
        for (y, x) in [(c, x) for x in range(w) if abs(x - c) >= 2] + [(y, c) for y in range(h) if abs(y - c) >= 2]:
            al[y, x] = 1
            rgb[y, x] = (230, 230, 230)
        return rgb, al, (0, 0, 0, 0)
    if n in (2533, 2534, 2535):  # casings: brass for the pistol, red plastic and brass for the shotgun
        from scipy import ndimage
        base = np.array([200, 150, 50]) if n == 2533 else np.array([190, 40, 30])
        light = 0.65 + 0.5 * (1 - (xx + yy) / (w + h))
        rgb = base * light[..., None]
        if n != 2533:  # brass head on the lower end
            ys = np.nonzero(al.any(1))[0]
            head = yy > ys.min() + (ys.max() - ys.min()) * 0.7
            rgb = np.where(head[..., None], np.array([210, 170, 70]) * light[..., None], rgb)
        edge = al.astype(bool) & ~ndimage.binary_erosion(al.astype(bool))
        rgb[edge] *= 0.6
        return np.clip(rgb, 0, 255), al, (0, 0, 0, 0)
    if n in (2541, 2542, 2543, 2614, 2615):
        return flame(al, rng, hot=1.2 if n in (2541, 2614) else 0.8), al, (0, 0, 0, 0)
    if n in (2538, 2539, 2540):  # the chaingun's barrels, glowing hotter: our 2537 plus a glow where Duke's glows
        rgb, a, pad = built[2537]
        orig = rgba[..., :3].astype(float)
        hot = ((orig[..., 0] > 150) & (orig[..., 0] > orig[..., 2] + 60) & (rgba[..., 3] > 0)).astype(float)
        from scipy import ndimage
        glow = np.clip(ndimage.gaussian_filter(hot, 1.5) * 2.2, 0, 1)
        glow = np.pad(glow, ((pad[1], pad[3]), (pad[0], pad[2])))  # 2537's padding
        fire = np.array([255, 150, 40])
        rgb = rgb * (1 - glow[..., None]) + fire * glow[..., None]
        return rgb, np.maximum(a, (glow > 0.3) * 1.0), pad
    raise KeyError(n)


def offsets(pa, w, h, pad, dx=0):
    """Duke's picanm with the offsets moved so a tile padded by pad = (left, top, right, bottom) draws its original
    pixels where his were (the game draws from the tile's centre plus its offsets), then dx screen pixels right."""
    L, T, R, B = pad
    W, H = w + L + R, h + T + B
    xo, yo = (pa >> 8) & 255, (pa >> 16) & 255
    xo, yo = (xo - 256 if xo > 127 else xo), (yo - 256 if yo > 127 else yo)
    xo += L + (w >> 1) - (W >> 1) - dx
    yo += T + (h >> 1) - (H >> 1)
    assert -128 <= xo <= 127 and -128 <= yo <= 127, (xo, yo)
    return (pa & ~0xFFFF00) | ((xo & 255) << 8) | ((yo & 255) << 16)


# boxes (x0, y0, x1, y1, in our tile's pixels) to clear: things fal painted that don't belong. 2536: a side-on cluster
# of barrels to the right of the chaingun's receiver (the barrels are their own tile, 2537, drawn above it)
ERASE = {2536: [(200, 0, 999, 66), (238, 0, 999, 999)]}

# screen pixels to move a weapon right (the shotgun: its barrel, carried on, passed left of the crosshair)
SHIFT = {n: 16 for n in range(2613, 2620)}


def iou(a, b):
    return (a & b).sum() / max(1, (a | b).sum())


def build():
    meta = {int(k): v for k, v in json.load(open(os.path.join(GUIDE, 'guides.json'))).items()}
    pal, usable, allc = palette()
    built, report = {}, []
    for n in TILES:
        if os.path.exists(os.path.join(RAW, '%d.png' % n)):
            built[n] = fit(n, meta)
    for n in FLIP:
        if n in built:
            rgb, al, pad = built[n]
            built[n] = (rgb[:, ::-1], al[:, ::-1], (pad[2], pad[1], pad[0], pad[3]))
    for n, src in COPIES.items():
        if src in built:
            built[n] = built[src]
    for n in LOCAL:
        if n in (2538, 2539, 2540) and 2537 not in built:
            continue
        built[n] = local(n, built)
    os.makedirs(OUT, exist_ok=True)
    sheet = []
    for first, last in GROUPS:
        tiles = []
        for n in range(first, last + 1):
            rgba, px, pa = duke(n)
            if n not in built:
                tiles.append((px, pa))  # not made yet: keep Duke's for now
                print('  %d: still Duke\'s' % n)
                continue
            rgb, al, pad = built[n]
            on = al >= 0.5
            for x0, y0, x1, y1 in ERASE.get(n, []):
                on[y0:y1, x0:x1] = False
            flash = n in (2541, 2542, 2543, 2614, 2615) or (n in TILES and TILES[n][3])
            idx = dither(rgb, on, pal, usable)
            if flash:  # flashes may glow: bright, warm pixels get the full palette (fullbright included)
                bright = (rgb.max(2) > 200) & (rgb[..., 0] > rgb[..., 2] + 40)
                idx = np.where(bright, match(rgb, pal, allc), idx)
            out = np.where(on, idx, 255).astype(np.uint8)
            h, w = px.shape
            tiles.append((out, offsets(pa, w, h, pad, SHIFT.get(n, 0))))
            L, T = pad[0], pad[1]
            score = iou(on[T:T + h, L:L + w], rgba[..., 3] > 0)
            report.append((n, score))
            sheet.append((n, out, px))
        mkfont.write_art(os.path.join(OUT, 'weapons_%d.art' % first), first, tiles)
    for n, s in report:
        print('  %d silhouette match %.2f%s' % (n, s, '  <-- check' if s < 0.75 else ''))
    # side-by-side check sheet: Duke's | ours, per tile
    rows = []
    for n, ours, theirs in sheet:
        a = np.where(theirs[..., None] != 255, pal[theirs], (255, 0, 255))
        b = np.where(ours[..., None] != 255, pal[ours], (255, 0, 255))
        hh = max(a.shape[0], b.shape[0])
        a, b = (np.pad(x, ((0, hh - x.shape[0]), (0, 0), (0, 0))) for x in (a, b))
        rows.append(np.concatenate([a, np.full((hh, 4, 3), 0), b], 1).astype(np.uint8))
    wmax = max(r.shape[1] for r in rows)
    img = np.concatenate([np.pad(r, ((0, 4), (0, wmax - r.shape[1]), (0, 0))) for r in rows], 0)
    Image.fromarray(img).save(os.path.join(ART, 'check.png'))


if __name__ == '__main__':
    cmd = sys.argv[1] if len(sys.argv) > 1 else 'build'
    if cmd == 'guides':
        guides()
    elif cmd == 'gen':
        gen([int(a) for a in sys.argv[2:]])
    else:
        build()
