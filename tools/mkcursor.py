"""The menu cursor: a grass block spinning about its upright axis, from our own block faces, in place of Duke's
spinning nuke sign (SPINNINGNUKEICON, tiles 2813-2819). Run: python tools/mkcursor.py -> game/textures/cursor.art

7 frames over a quarter turn (a cube looks the same every 90 degrees, so the loop is seamless; the right-hand cursor
plays them backwards). Ray traced from slightly above, 4x4 supersampled, matched to the game palette.
"""
import math, os, sys
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, '..')
BLOCKS = os.path.join(ROOT, 'game', 'textures', 'blocks')
FIRST, FRAMES = 2813, 7
W, H = 25, 21          # Duke's spinner tiles are 21 tall
PITCH = math.radians(28)  # looking down onto the top
SS = 4


def tex(name):
    return np.asarray(Image.open(os.path.join(BLOCKS, name + '.png')).convert('RGB'), np.float32)


def frame(yaw, top, side, bottom, scale):
    ys, xs = np.mgrid[0:H * SS, 0:W * SS].astype(np.float32)
    sx = ((xs + 0.5) / SS - W / 2) / scale
    sy = (H / 2 - (ys + 0.5) / SS) / scale
    # orthographic camera: right = x, up tilted by the pitch, looking along -z tilted down
    cp, sp = math.cos(PITCH), math.sin(PITCH)
    d = np.array([0, -sp, cp], np.float32)
    o = np.stack([sx, sy * cp, sy * sp], -1) - d * 5  # start well in front of the cube
    # into the cube's frame (turned by yaw about y)
    cy, syw = math.cos(yaw), math.sin(yaw)
    rot = np.array([[cy, 0, -syw], [0, 1, 0], [syw, 0, cy]], np.float32)
    o, d = o @ rot.T, d @ rot.T
    with np.errstate(divide='ignore', invalid='ignore'):
        t0 = (-0.5 - o) / d
        t1 = (0.5 - o) / d
    tmin = np.minimum(t0, t1).max(-1)
    tmax = np.maximum(t0, t1).min(-1)
    hit = tmax > np.maximum(tmin, 0)
    p = o + d * tmin[..., None]
    axis = np.minimum(t0, t1).argmax(-1)  # which slab was entered last: the face hit
    img = np.zeros((H * SS, W * SS, 3), np.float32)
    n = tex('grass_top').shape[0]
    def sample(t, u, v):
        return t[np.clip((v * n).astype(int), 0, n - 1), np.clip((u * n).astype(int), 0, n - 1)]
    u = np.zeros_like(sx); v = np.zeros_like(sx); light = np.zeros_like(sx)
    topm = hit & (axis == 1) & (p[..., 1] > 0)
    botm = hit & (axis == 1) & (p[..., 1] <= 0)
    xm = hit & (axis == 0)
    zm = hit & (axis == 2)
    img[topm] = sample(top, p[..., 0][topm] + 0.5, p[..., 2][topm] + 0.5)
    img[botm] = sample(bottom, p[..., 0][botm] + 0.5, p[..., 2][botm] + 0.5)
    img[xm] = sample(side, p[..., 2][xm] * np.sign(p[..., 0][xm]) + 0.5, 0.5 - p[..., 1][xm])
    img[zm] = sample(side, -p[..., 0][zm] * np.sign(p[..., 2][zm]) + 0.5, 0.5 - p[..., 1][zm])
    # light from above and the front left: the top brightest, the two sides a little apart
    lx = np.array([-0.5, 0, -0.7]) @ rot.T
    shade = np.where(topm, 1.0, np.where(botm, 0.4, 0.0))
    shade = np.where(xm, 0.62 + 0.25 * np.sign(p[..., 0]) * lx[0], shade)
    shade = np.where(zm, 0.62 + 0.25 * np.sign(p[..., 2]) * lx[2], shade)
    img *= shade[..., None] * 1.2  # a touch brighter than in the world: it's small
    rgb = img.reshape(H, SS, W, SS, 3).mean((1, 3))
    alpha = hit.reshape(H, SS, W, SS).mean((1, 3))
    rgb = rgb / np.maximum(alpha[..., None], 1e-6)  # un-premultiply the edges
    return rgb, alpha >= 0.5


def main():
    sys.path.insert(0, HERE)
    import mktitle
    pal, fullbright = mktitle.game_palette()
    top, side, bottom = tex('grass_top'), tex('grass_side'), tex('dirt')
    tiles, prev = [], []
    for k in range(FRAMES):
        if k == 0:
            # as big as fits the tile at every angle of the turn: start big and shrink
            scale = 16.0
            while True:
                ok = True
                for a in np.linspace(0, 90, FRAMES, endpoint=False):
                    m = frame(math.radians(a), top, side, bottom, scale)[1]
                    r, c = np.nonzero(m.any(1))[0], np.nonzero(m.any(0))[0]
                    if r.min() == 0 or r.max() == H - 1 or c.min() == 0 or c.max() == W - 1: ok = False; break
                if ok: break
                scale *= 0.97
        rgb, al = frame(math.radians(90 * k / FRAMES), top, side, bottom, scale)
        idx = mktitle.to_palette(Image.fromarray(np.clip(rgb, 0, 255).astype(np.uint8)), pal, fullbright)
        idx = np.where(al, idx, 255).astype(np.uint8)
        tiles.append(idx)
        p = pal[idx].astype(np.uint8); p[idx == 255] = (40, 40, 60); prev.append(p)
    path = os.path.join(ROOT, 'game', 'textures', 'cursor.art')
    import struct
    with open(path, 'wb') as f:
        f.write(struct.pack('<4i', 1, FRAMES, FIRST, FIRST + FRAMES - 1))
        for _ in tiles: f.write(struct.pack('<h', W))
        for _ in tiles: f.write(struct.pack('<h', H))
        for _ in tiles: f.write(struct.pack('<I', 0))
        for t in tiles: f.write(np.ascontiguousarray(t.T).tobytes())
    Image.fromarray(np.concatenate(prev, 1)).resize((W * FRAMES * 6, H * 6), Image.NEAREST).save(
        os.path.join(ROOT, 'art', 'title', 'cursor_preview.png'))
    print(path, 'scale %.1f' % scale)


if __name__ == '__main__':
    main()
