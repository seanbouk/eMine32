"""A game box cover (it was the page's start screen; now only the itch cover): fal's painting (art/page/fal/boxart.png, prompt in
art/page/fal_prompts.json) at 960x720 with the game's "SHIFT HAPPENS" (the menu's logo, tools/mktitle.py) over the
night sky -> art/page/cover.png (630x500, the
itch.io cover image)."""
import os, sys
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, '..')
sys.path.insert(0, HERE)
import mktitle

W, H = 960, 720
art = Image.open(os.path.join(ROOT, 'art', 'page', 'fal', 'boxart.png')).convert('RGB')
aw, ah = art.size
if aw / ah > W / H:
    cw = round(ah * W / H); art = art.crop(((aw - cw) // 2, 0, (aw - cw) // 2 + cw, ah))
else:
    ch = round(aw * H / W); art = art.crop((0, 0, aw, ch))
art = np.asarray(art.resize((W, H), Image.LANCZOS), np.float32)
# darker at the top (under the title) and the bottom (under "Click to play")
y = np.linspace(0, 1, H)[:, None, None]
art *= 1 - 0.55 * np.clip(1 - y / 0.3, 0, 1) - 0.5 * np.clip((y - 0.82) / 0.18, 0, 1)

def put_logo(img, width, top):
    rgb, al = mktitle.wide_logo(width)
    h = round(rgb.height * mktitle.PIXEL_ASPECT)           # its pixels were tall (320x200); here they're square
    rgb = np.asarray(rgb.resize((width, h), Image.LANCZOS), np.float32)
    al = np.asarray(Image.fromarray((al * 255).astype(np.uint8)).resize((width, h), Image.LANCZOS), np.float32)[..., None] / 255
    x0 = (img.shape[1] - width) // 2
    sh = np.zeros_like(al); sh[6:, 6:] = al[:-6, :-6]      # a drop shadow
    reg = img[top:top + h, x0:x0 + width]
    reg[:] = reg * (1 - 0.7 * sh)
    reg[:] = reg * (1 - al) + rgb * al
    return img

out = put_logo(art.copy(), 800, 26)
os.makedirs(os.path.join(ROOT, 'web', 'itch'), exist_ok=True)
cov = Image.fromarray(np.clip(out, 0, 255).astype(np.uint8))
cw = round(H * 630 / 500)
cov.crop(((W - cw) // 2, 0, (W - cw) // 2 + cw, H)).resize((630, 500), Image.LANCZOS).save(os.path.join(ROOT, 'art', 'page', 'cover.png'))
print('art/page/cover.png (the itch.io cover)')
