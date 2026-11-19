"""The detonator (the pipe bomb's remote, tiles 2570-2572): Sean's painting, art/weapons/free/detonator.jpg (a left
hand, thumb on the red button, the arm off its bottom-left corner, on magenta), set into the frame tools/mkweapons.py
fits the fal paintings to (art/weapons/guide/guides.json: Duke's tile inside a canvas) -> art/weapons/raw/2570.png.
mkweapons then cuts it out like the rest; 2571 and 2572 (the press) are the same picture (COPIES there).

The painting's arm stops at its left and bottom edges; here it's carried on down and to the left (along the arm) so
no cut shows on screen. HAND is where the device's middle lands in the canvas, SIZE its height there (antenna tip to
the bottom of the device), both in the guide canvas's pixels."""
import json, os
import numpy as np
from PIL import Image

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
SRC = os.path.join(ROOT, 'art', 'weapons', 'free', 'detonator.jpg')
OUT = os.path.join(ROOT, 'art', 'weapons', 'raw', '2570.png')
MAGENTA = (253, 0, 250)

# in the painting (2048 square): the device's middle, the antenna's tip, the device's foot
MID, TIP, FOOT = (1200, 1150), 70, 1720
HAND, SIZE = (700, 245), 300     # in the guide canvas (1024 x 439 for 2570)
UP = 4                           # the raw picture is this many times the canvas

meta = json.load(open(os.path.join(ROOT, 'art', 'weapons', 'guide', 'guides.json')))['2570']
cw, ch = meta['size']
im = np.asarray(Image.open(SRC).convert('RGB'))
H, W = im.shape[:2]
k = SIZE / (FOOT - TIP) * UP                       # painting pixels -> raw pixels
RW, RH = cw * UP, ch * UP
# raw pixel -> painting pixel; outside the painting to the left/below, step back along the arm (up and to the right)
ys, xs = np.mgrid[0:RH, 0:RW].astype(np.float32)
px = (xs - HAND[0] * UP) / k + MID[0]
py = (ys - HAND[1] * UP) / k + MID[1]
t = np.maximum(np.maximum(-px, py - (H - 1)), 0)
px, py = px + t, py - t
inside = (px >= 0) & (px <= W - 1) & (py >= 0) & (py <= H - 1)
out = np.empty((RH, RW, 3), np.uint8)
out[:] = MAGENTA
xi, yi = np.clip(px.round().astype(int), 0, W - 1), np.clip(py.round().astype(int), 0, H - 1)
src = im[yi, xi]
arm = np.sqrt(((src.astype(np.float32) - MAGENTA) ** 2).sum(2)) > 90   # carry on only what's painted (not JPEG noise)
keep = inside & ((t == 0) | arm)
out[keep] = src[keep]
Image.fromarray(out).save(OUT)
print(os.path.relpath(OUT, ROOT), out.shape[1], 'x', out.shape[0])
