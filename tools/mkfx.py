"""Our own fire, explosions, smoke, sparks and water splashes, drawn procedurally: game/textures/fx_*.art

Run: python tools/mkfx.py      (writes the ART files and art/fx/check.png, a sheet of every frame)

Each effect keeps Duke's tile numbers, frame counts, tile sizes and offsets (his game scripts step through the
frames and the engine places them by their offsets), but the pictures are ours: fractal noise shaped into a fireball,
a flame, a puff or a spray and coloured through a heat ramp, matched to the palette. Hot colours may use the
palette's fullbright entries, so fire glows in the dark the way Duke's does.
"""
import os, sys
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, '..')
sys.path.insert(0, HERE)
import mksprite, mkfont, mkweapons

OUT = os.path.join(ROOT, 'game', 'textures')

# (first tile, frames, kind)
EFFECTS = [
    (1380, 5, 'splash'),      # WATERSPLASH2
    (1890, 20, 'explosion'),  # EXPLOSION2
    (2270, 14, 'flame'),      # BURNING / FIRE
    (2310, 14, 'flame2'),     # BURNING2 / FIRE2 (bigger)
    (2329, 4, 'smoke'),       # SMALLSMOKE
    (2595, 4, 'spark'),       # SHOTSPARK1 (a bullet hitting something)
]


# --- noise ------------------------------------------------------------------------------------------------------

def lattice(shape, rng):
    return rng.random(shape)


def value_noise(x, y, z, grid, period_z=None):
    """Smooth value noise at float coords (x, y, z) from a 3D lattice; z wraps at period_z (seamless loops)."""
    gx, gy, gz = grid.shape
    x0, y0, z0 = np.floor(x).astype(int), np.floor(y).astype(int), np.floor(z).astype(int)
    fx, fy, fz = x - x0, y - y0, z - z0
    sx, sy, sz = fx * fx * (3 - 2 * fx), fy * fy * (3 - 2 * fy), fz * fz * (3 - 2 * fz)
    pz = period_z or gz

    def g(i, j, k):
        return grid[i % gx, j % gy, k % pz]
    out = 0
    for dx in (0, 1):
        wx = sx if dx else 1 - sx
        for dy in (0, 1):
            wy = sy if dy else 1 - sy
            for dz in (0, 1):
                wz = sz if dz else 1 - sz
                out = out + wx * wy * wz * g(x0 + dx, y0 + dy, z0 + dz)
    return out


def fbm(x, y, z, seed, octaves=4, period_z=None):
    rng = np.random.default_rng(seed)
    total, amp, norm = 0, 1.0, 0
    for o in range(octaves):
        grid = lattice((64, 64, 64), rng)
        f = 2 ** o
        total = total + amp * value_noise(x * f, y * f, z * f if period_z is None else z * f, grid,
                                          None if period_z is None else period_z * f)
        norm += amp
        amp *= 0.5
    return total / norm


# --- colour -----------------------------------------------------------------------------------------------------

FIRE_RAMP = np.array([  # heat 0..1 -> colour
    [40, 10, 5], [120, 20, 0], [200, 60, 5], [245, 130, 20], [255, 200, 70], [255, 245, 170], [255, 255, 235]], float)
SMOKE = np.array([70, 64, 60], float)


def ramp(t, stops=FIRE_RAMP):
    t = np.clip(t, 0, 1) * (len(stops) - 1)
    i = np.minimum(t.astype(int), len(stops) - 2)
    f = (t - i)[..., None]
    return stops[i] * (1 - f) + stops[i + 1] * f


# --- effects (each returns rgb float HxWx3, alpha 0..1 HxW, glow mask HxW for a frame) ------------------------------

def explosion(w, h, k, n, seed=7):
    """A fireball that flashes white-hot, swells, cools to orange and red and breaks up into dark smoke."""
    t = k / (n - 1)
    yy, xx = np.mgrid[0:h, 0:w].astype(float)
    cx, cy = w / 2, h * 0.55
    r = min(w, h) * 0.5 * min(1.0, 0.35 + t * 2.2)  # quickly to full size
    d = np.hypot((xx - cx) / r, (yy - cy) / r)
    nz = fbm(xx / 14, yy / 14, np.full_like(xx, t * 3.0), seed, octaves=4)
    edge = d + (nz - 0.5) * 0.9
    body = np.clip((1.0 - edge) * 2.5, 0, 1)                       # 1 inside, 0 outside
    heat = np.clip(body * (1.15 - 1.5 * t) + (nz - 0.5) * 0.7, 0, 1)  # cools as it goes
    # break-up: holes grow late in the explosion
    holes = fbm(xx / 9 + 40, yy / 9, np.full_like(xx, t * 2.0), seed + 1, octaves=3)
    dens = body * np.clip(1.0 - np.clip(t - 0.55, 0, 1) * 1.7 + (holes - 0.5) * 1.6, 0, 1)
    rgb = ramp(0.1 + heat * 0.8)
    smoke = np.clip((0.3 - heat) * 3, 0, 1)[..., None]
    rgb = rgb * (1 - smoke) + SMOKE * smoke * (0.7 + nz[..., None] * 0.6)
    alpha = (dens > 0.35).astype(float)
    return rgb, alpha, heat > 0.45


def flame(w, h, k, n, seed=11, width=0.42):
    """A licking flame, looping over n frames: noise rising through a tapering envelope."""
    yy, xx = np.mgrid[0:h, 0:w].astype(float)
    up = 1 - yy / h                                   # 0 at the bottom, 1 at the top
    ang = 2 * np.pi * k / n                           # loop: walk round a circle in the noise's extra dimensions
    nz = fbm(xx / 7 + np.cos(ang) * 2, (yy + 0) / 9 + k * (h / 9 / n) * 0 + np.sin(ang) * 2,
             np.full_like(xx, np.cos(ang) * 1.5 + 3), seed, octaves=4)
    rise = fbm(xx / 9, yy / 12 + k * 4.0 / n * 3, np.full_like(xx, 1.0), seed + 3, octaves=3, period_z=None)
    half = w * width * (1 - up ** 1.3) + 0.3         # wide at the base, a point at the top
    across = np.abs(xx - w / 2) / half
    inten = (1 - across) * 1.15 - up * 0.6 + (nz - 0.5) * 1.8 + (rise - 0.5) * 0.7
    heat = np.clip(inten, 0, 1)
    rgb = ramp(0.15 + heat * 0.72)
    alpha = (inten > 0.18).astype(float)
    return rgb, alpha, heat > 0.3


def smoke(w, h, k, n, seed=21):
    """A small grey puff that swells and thins."""
    t = k / max(1, n - 1)
    yy, xx = np.mgrid[0:h, 0:w].astype(float)
    r = min(w, h) * (0.38 + 0.12 * t)
    d = np.hypot(xx - w / 2, yy - h / 2) / r
    nz = fbm(xx / 4, yy / 4, np.full_like(xx, t), seed, octaves=3)
    dens = (1.1 - d) + (nz - 0.5) * 0.9 - t * 0.35
    shade = 0.55 + (nz - 0.5) * 0.8 - (yy - h / 2) / h * 0.4  # lit from above
    rgb = np.array([150, 145, 140], float) * np.clip(shade, 0.3, 1.2)[..., None]
    return rgb, (dens > 0.25).astype(float), np.zeros((h, w), bool)


def spark(w, h, k, n, seed=31):
    """A bullet strike: a hot point, then a few sparks flying out and dying."""
    rng = np.random.default_rng(seed)
    rgb = np.zeros((h, w, 3)); alpha = np.zeros((h, w)); glow = np.zeros((h, w), bool)
    cx, cy = w / 2, h / 2
    t = k / max(1, n - 1)
    if k == 0:
        yy, xx = np.mgrid[0:h, 0:w]
        d = np.hypot(xx - cx, yy - cy)
        m = d < max(1.5, min(w, h) * 0.35)
        rgb[m] = (255, 250, 200); alpha[m] = 1; glow[m] = True
        return rgb, alpha, glow
    for i in range(10):
        a = rng.random() * 2 * np.pi
        sp = (0.4 + rng.random() * 0.6) * min(w, h) * 0.5
        px, py = cx + np.cos(a) * sp * t, cy + np.sin(a) * sp * t + t * t * h * 0.15
        if 0 <= int(px) < w and 0 <= int(py) < h and rng.random() > t * 0.5:
            rgb[int(py), int(px)] = ramp(np.array(1 - t * 0.7)); alpha[int(py), int(px)] = 1; glow[int(py), int(px)] = True
    return rgb, alpha, glow


def splash(w, h, k, n, seed=41):
    """Water thrown up where a shot hits it: a column of spray that rises, spreads and falls back."""
    rng = np.random.default_rng(seed)
    rgb = np.zeros((h, w, 3)); alpha = np.zeros((h, w))
    t = (k + 0.5) / n
    for i in range(160):
        vx = (rng.random() - 0.5) * 0.9
        vy = 0.5 + rng.random() * 0.9
        x = w / 2 + vx * w * t
        y = h - 1 - (vy * t - 0.9 * t * t) * h * 1.1
        if 0 <= x < w and 0 <= y < h and rng.random() > t * 0.4:
            c = (170, 205, 235) if rng.random() < 0.6 else (230, 245, 255)
            for dx, dy in ((0, 0), (0, 1)) if vy > 1.1 else ((0, 0),):
                X, Y = int(x) + dx, int(y) + dy
                if 0 <= X < w and 0 <= Y < h:
                    rgb[Y, X] = c; alpha[Y, X] = 1
    return rgb, alpha, np.zeros((h, w), bool)


KINDS = {'explosion': explosion, 'flame': flame, 'flame2': lambda w, h, k, n: flame(w, h, k, n, seed=13, width=0.46),
         'smoke': smoke, 'spark': spark, 'splash': splash}


def main():
    pal, usable, allc = mkweapons.palette()
    os.makedirs(OUT, exist_ok=True)
    rows = []
    for first, n, kind in EFFECTS:
        tiles, row = [], []
        for k in range(n):
            _, px, pa = mkweapons.duke(first + k)
            h, w = px.shape
            rgb, alpha, glow = KINDS[kind](w, h, k, n)
            on = alpha >= 0.5
            idx = mkweapons.match(rgb, pal, usable)
            idx = np.where(glow, mkweapons.match(rgb, pal, allc), idx)  # hot parts glow
            out = np.where(on, idx, 255).astype(np.uint8)
            tiles.append((out, pa))
            row.append(np.where(out[..., None] != 255, pal[out], (255, 0, 255)).astype(np.uint8))
        mkfont.write_art(os.path.join(OUT, 'fx_%d.art' % first), first, tiles)
        hh = max(r.shape[0] for r in row)
        rows.append(np.concatenate([np.pad(r, ((0, hh - r.shape[0]), (0, 4), (0, 0)), constant_values=0) for r in row], 1))
    wmax = max(r.shape[1] for r in rows)
    sheet = np.concatenate([np.pad(r, ((0, 6), (0, wmax - r.shape[1]), (0, 0))) for r in rows], 0)
    os.makedirs(os.path.join(ROOT, 'art', 'fx'), exist_ok=True)
    Image.fromarray(sheet).save(os.path.join(ROOT, 'art', 'fx', 'check.png'))


if __name__ == '__main__':
    main()
