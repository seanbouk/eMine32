"""Title screen: art/title/title.png -> game/textures/title.png, the tile EMINE_TITLE (7300) that replaces Duke's
title (screens.cpp). Run: python tools/mktitle.py

- widened for widescreen screens: the picture in the middle, its edges carried out to the sides (stretched,
  blurred and darkened, so the eye stays on the picture)
- dithered into Duke's game palette here (the engine would just take the nearest colour, which bands the sky)
"""
import os, sys
import numpy as np
from PIL import Image, ImageFilter

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
SRC = os.path.join(ROOT, 'art', 'title', 'title.png')
OUT = os.path.join(ROOT, 'game', 'textures', 'title.png')
ASPECT = 2.1  # wide enough for 21:9; narrower screens crop the sides


def widen(im):
    w, h = im.size
    W = int(round(h * ASPECT))
    pad = (W - w) // 2
    out = Image.new('RGB', (W, h))
    for side in (0, 1):
        strip = im.crop((0, 0, w // 6, h) if side == 0 else (w - w // 6, 0, w, h)).transpose(Image.FLIP_LEFT_RIGHT)
        strip = strip.resize((pad + 40, h), Image.BICUBIC).filter(ImageFilter.GaussianBlur(18))
        a = np.asarray(strip, np.float32)
        x = np.linspace(0, 1, a.shape[1])[None, :, None]
        fade = 0.25 + 0.5 * (x if side == 0 else 1 - x)  # darker toward the outer edge
        strip = Image.fromarray(np.clip(a * fade, 0, 255).astype(np.uint8))
        out.paste(strip, (0, 0) if side == 0 else (W - pad - 40, 0))
    # the picture, with its own edges feathered into the extensions
    mask = np.ones((h, w), np.float32)
    ramp = np.linspace(0, 1, 40)
    mask[:, :40] *= ramp; mask[:, -40:] *= ramp[::-1]
    out.paste(im, (pad, 0), Image.fromarray((mask * 255).astype(np.uint8)))
    return out


def dither(im):
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import mksprite
    d = mksprite.grp_file(mksprite.GRP, 'PALETTE.DAT')
    pal = np.frombuffer(d[:768], np.uint8).reshape(256, 3).astype(np.int32) << 2
    p = Image.new('P', (1, 1))
    flat = pal[:255].astype(np.uint8).flatten().tolist()  # 255 is see-through in Build
    p.putpalette(flat + flat[-3:])                        # pad 255 with a copy of 254: never chosen over it
    q = im.quantize(palette=p, dither=Image.Dither.FLOYDSTEINBERG)
    return q.convert('RGB')


if __name__ == '__main__':
    im = Image.open(SRC).convert('RGB')
    out = dither(widen(im))
    out.save(OUT)
    print('%s %dx%d' % (OUT, out.width, out.height))
