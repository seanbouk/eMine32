"""The weapon pickups in the mines' start case: art/props/fal/weapons.png (one fal sheet, prompt in
art/props/fal/fal_prompts.json) -> art/props/pickup_<name>.png, one cut-out each, laid down flat.
Then `python tools/mksprite.py` makes the tiles (art/sprites.json: pickup_*, tiles 7160-7164).

Run: python tools/mkpickups.py
"""
import os
import numpy as np
from PIL import Image
from scipy import ndimage

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
SHEET = os.path.join(ROOT, 'art', 'props', 'fal', 'weapons.png')
# left to right on the sheet; lie: turn it so its long side is level (painted standing on end, leaning right)
NAMES = ['shotgun', 'minigun', 'rocket', 'pipebombs', 'devastator']
LIE = {'shotgun': True, 'minigun': True, 'rocket': True, 'pipebombs': True, 'devastator': True}


def main():
    a = np.asarray(Image.open(SHEET).convert('RGB')).astype(np.int32)
    r, g, b = a[..., 0], a[..., 1], a[..., 2]
    # magenta: red and blue high, green low
    mag = (np.minimum(r, b) - g)
    bg = mag > 90
    keep = ndimage.binary_opening(~bg, iterations=2)
    lab, k = ndimage.label(keep)
    sizes = ndimage.sum(keep, lab, range(1, k + 1))
    big = sorted(1 + np.argsort(sizes)[::-1][:len(NAMES)], key=lambda i: ndimage.find_objects((lab == i).astype(int))[0][1].start)
    for name, i in zip(NAMES, big):
        sl = ndimage.find_objects((lab == i).astype(int))[0]
        m = lab[sl] == i                      # holes (the trigger guard, the minigun's handle) stay see-through
        rgb = a[sl].astype(np.float32)
        # de-spill the magenta fringe: pull red and blue down toward green where they both stand above it
        sp = np.clip(np.minimum(rgb[..., 0], rgb[..., 2]) - rgb[..., 1], 0, None)
        edge = m & ~ndimage.binary_erosion(m, iterations=3)
        rgb[..., 0] -= sp * edge
        rgb[..., 2] -= sp * edge
        im = Image.fromarray(np.dstack([np.clip(rgb, 0, 255), m * 255]).astype(np.uint8), 'RGBA')
        if LIE[name]:
            ys, xs = np.nonzero(m)
            c = np.cov(np.stack([xs, ys]))
            w, v = np.linalg.eigh(c)
            ax = v[:, 1]
            ang = np.degrees(np.arctan2(ax[1], ax[0]))     # image y runs down
            if ang > 90: ang -= 180
            if ang < -90: ang += 180
            im = im.rotate(ang, Image.BICUBIC, expand=True)   # PIL turns anticlockwise: undo the lean
            al = np.asarray(im)[..., 3]
            ys, xs = np.nonzero(al > 8)
            im = im.crop((xs.min(), ys.min(), xs.max() + 1, ys.max() + 1))
        out = os.path.join(ROOT, 'art', 'props', f'pickup_{name}.png')
        im.save(out)
        print(os.path.relpath(out, ROOT), im.size)


if __name__ == '__main__':
    main()
