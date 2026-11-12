"""The last of the menu graphics, ours in Duke's sizes: the options slider (bar and knob), the frame round a save's
picture and the box behind a save's name. Run: python tools/mkui.py -> game/textures/ui_slider.art, ui_frame.art

  SLIDEBAR 2489 (80x14) a stone bar with a dark slot; SLIDEBAR+1 2490 (14x12) the knob: a little grass block
  WINDOWBORDER1 2494 (2x160), TEXTBOX 2495 (82x9), WINDOWBORDER2 2496 (2x100): stone-grey lines and frame
Same sizes and anchor offsets as the tiles they replace, so the menu code places them as before. Game palette.
"""
import os, struct, sys
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, '..')
BLOCKS = os.path.join(ROOT, 'game', 'textures', 'blocks')
OUT = os.path.join(ROOT, 'game', 'textures')


def picanm(xofs, yofs):
    return ((xofs & 0xff) << 8) | ((yofs & 0xff) << 16)


def write_art(path, first, tiles):
    with open(path, 'wb') as f:
        f.write(struct.pack('<4i', 1, len(tiles), first, first + len(tiles) - 1))
        for px, _ in tiles: f.write(struct.pack('<h', px.shape[1]))
        for px, _ in tiles: f.write(struct.pack('<h', px.shape[0]))
        for _, pa in tiles: f.write(struct.pack('<I', pa))
        for px, _ in tiles: f.write(np.ascontiguousarray(px.T).tobytes())
    print(os.path.relpath(path, ROOT))


def stone(w, h, light=1.0):
    """Our stone face, tiled to w x h (RGB float)."""
    t = np.asarray(Image.open(os.path.join(BLOCKS, 'stone.png')).convert('RGB'), np.float32)
    reps = (h // t.shape[0] + 1, w // t.shape[1] + 1, 1)
    return np.tile(t, reps)[:h, :w] * light


def main():
    sys.path.insert(0, HERE)
    import mktitle
    pal, fullbright = mktitle.game_palette()
    q = lambda rgb: mktitle.to_palette(Image.fromarray(np.clip(rgb, 0, 255).astype(np.uint8)), pal, fullbright)

    # slider bar: a stone bar, lit edge on top, shadowed below, with a dark slot along the middle
    w, h = 80, 14
    bar = stone(w, h, 1.1)
    bar[0] *= 1.35; bar[-1] *= 0.45; bar[:, 0] *= 1.2; bar[:, -1] *= 0.6
    bar[4:10, 3:w - 3] = stone(w - 6, 6, 0.18)
    bar[4, 3:w - 3] *= 0.5; bar[9, 3:w - 3] *= 2.2  # the slot's top in shadow, its bottom lip catching light
    slider = q(bar)

    # knob: the grass block from the menu cursor, shrunk
    import mkcursor, math
    top, side, bottom = mkcursor.tex('grass_top'), mkcursor.tex('grass_side'), mkcursor.tex('dirt')
    rgb, al = mkcursor.frame(math.radians(30), top, side, bottom, 12.9)
    ys, xs = np.nonzero(al)
    rgb, al = rgb[ys.min():ys.max() + 1, xs.min():xs.max() + 1], al[ys.min():ys.max() + 1, xs.min():xs.max() + 1]
    kw, kh = 14, 12
    img = Image.fromarray(np.clip(rgb, 0, 255).astype(np.uint8)).resize((kw, kh), Image.LANCZOS)
    a = np.asarray(Image.fromarray((al * 255).astype(np.uint8)).resize((kw, kh), Image.LANCZOS)) >= 128
    knob = np.where(a, q(np.asarray(img, np.float32)), 255).astype(np.uint8)
    write_art(os.path.join(OUT, 'ui_slider.art'), 2489, [(slider, picanm(-1, -1)), (knob, picanm(10, 3))])

    # the frame round a save's picture: lines, light on one side, dark on the other, and the name box
    def line(length):
        rgb = stone(2, length, 1.0)
        rgb[:, 0] *= 1.3; rgb[:, 1] *= 0.55
        return q(rgb)
    box = np.full((9, 82), 255, np.uint8)
    frame = stone(82, 9, 1.0)
    frame[0] *= 1.3; frame[-1] *= 0.55
    fr = q(frame)
    box[0], box[-1], box[:, 0], box[:, -1] = fr[0], fr[-1], fr[:, 0], fr[:, -1]
    write_art(os.path.join(OUT, 'ui_frame.art'), 2494,
              [(line(160), picanm(3, 2)), (box, picanm(-11, 6)), (line(100), picanm(1, 0))])


if __name__ == '__main__':
    main()
