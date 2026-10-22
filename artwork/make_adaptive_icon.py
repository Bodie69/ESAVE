"""Usage: python artwork/make_adaptive_icon.py artwork/app_icon.png <out_dir>  (needs numpy + Pillow)

Split app_icon.png into adaptive-icon layers (background gradient + transparent foreground glyph).

The source is a glyph on a rounded-square linear blue gradient with transparent corners. The
gradient is fitted per channel (c = a + b*x + c*y) on background pixels, then every pixel's alpha
is how far it departs from that gradient; its colour is un-mixed from the background.
"""
import sys

import numpy as np
from PIL import Image

src, out_dir = sys.argv[1], sys.argv[2]
SIZE = 432                     # 108dp @ xxxhdpi
SAFE_RADIUS = 0.305 * SIZE     # safe zone: 66dp-diameter circle of 108dp, minus a hair

img = np.asarray(Image.open(src).convert('RGBA')).astype(np.float64)
h, w = img.shape[:2]
rgb, a = img[..., :3], img[..., 3]
ys, xs = np.mgrid[0:h, 0:w]

# 1. Fit the background gradient on opaque pixels that look like the dark/mid blue backdrop.
r, g, b = rgb[..., 0], rgb[..., 1], rgb[..., 2]
backdrop = (a > 250) & (r < 40) & (b > 70) & (b < 200) & (g < 120)
X = np.stack([np.ones(backdrop.sum()), xs[backdrop], ys[backdrop]], axis=1)
coef = [np.linalg.lstsq(X, rgb[..., c][backdrop], rcond=None)[0] for c in range(3)]
bg = np.stack([coef[c][0] + coef[c][1] * xs + coef[c][2] * ys for c in range(3)], axis=-1)
resid = np.abs(rgb[backdrop] - bg[backdrop]).mean()
print('gradient fit mean abs residual:', round(float(resid), 2))

# 2. Alpha = departure from the gradient (ramped for anti-aliased edges); only inside the square.
diff = np.sqrt(((rgb - bg) ** 2).sum(axis=-1))
# Ignore a 30 px band inside the rounded square's edge (its anti-aliased rim is not glyph).
opaque = (a > 250).astype(np.int64)
k = 30
ii = np.pad(opaque, ((1, 0), (1, 0))).cumsum(0).cumsum(1)
pad = np.pad(ii, ((k, k), (k, k)), mode='edge')
win = (pad[2 * k + 1:, 2 * k + 1:] - pad[:-2 * k - 1, 2 * k + 1:] - pad[2 * k + 1:, :-2 * k - 1] + pad[:-2 * k - 1, :-2 * k - 1])
interior = win[:h, :w] >= (2 * k + 1) ** 2
alpha = np.clip((diff - 18.0) / (60.0 - 18.0), 0.0, 1.0) * interior
safe_alpha = np.maximum(alpha, 1e-6)[..., None]
fg = np.clip((rgb - (1 - alpha[..., None]) * bg) / safe_alpha, 0, 255)

# 3. Crop to the glyph, centre it, and scale so its farthest pixel sits inside the safe circle.
mask = alpha > 0.05
gy, gx = np.nonzero(mask)
cy, cx = (gy.min() + gy.max()) / 2.0, (gx.min() + gx.max()) / 2.0
max_r = np.sqrt((gy - cy) ** 2 + (gx - cx) ** 2).max()
scale = SAFE_RADIUS / max_r
print('glyph bbox', gx.min(), gx.max(), gy.min(), gy.max(), 'max radius', round(float(max_r)), 'scale', round(scale, 3))

glyph = np.dstack([fg, alpha * 255]).astype(np.uint8)
glyph_img = Image.fromarray(glyph, 'RGBA')
new_w, new_h = round(w * scale), round(h * scale)
glyph_img = glyph_img.resize((new_w, new_h), Image.LANCZOS)
fg_canvas = Image.new('RGBA', (SIZE, SIZE), (0, 0, 0, 0))
fg_canvas.paste(glyph_img, (round(SIZE / 2 - cx * scale), round(SIZE / 2 - cy * scale)), glyph_img)
fg_canvas.save(f'{out_dir}/ic_launcher_foreground.png')

# 4. Background layer: the same gradient, sampled across the source's square, full bleed.
inner = 40, w - 40  # the rounded square spans ~40..984 px in the 1024 px source
bx = np.linspace(inner[0], inner[1], SIZE)
byy, bxx = np.meshgrid(bx, bx, indexing='ij')
bg_layer = np.stack([coef[c][0] + coef[c][1] * bxx + coef[c][2] * byy for c in range(3)], axis=-1)
Image.fromarray(np.clip(bg_layer, 0, 255).astype(np.uint8), 'RGB').save(f'{out_dir}/ic_launcher_background.png')

# 5. Previews: circle, squircle-ish rounded square and Android 12 splash (on primary_deep).
fg_l = Image.open(f'{out_dir}/ic_launcher_foreground.png')
bg_l = Image.open(f'{out_dir}/ic_launcher_background.png').convert('RGBA')
composed = Image.alpha_composite(bg_l, fg_l)
vis = composed.crop((SIZE // 6, SIZE // 6, SIZE - SIZE // 6, SIZE - SIZE // 6))  # 72dp visible
previews = Image.new('RGBA', (vis.width * 3 + 40, vis.height), (245, 248, 252, 255))
from PIL import ImageDraw
for i, shape in enumerate(['circle', 'rounded']):
    m = Image.new('L', vis.size, 0)
    d = ImageDraw.Draw(m)
    if shape == 'circle':
        d.ellipse((0, 0, vis.width - 1, vis.height - 1), fill=255)
    else:
        d.rounded_rectangle((0, 0, vis.width - 1, vis.height - 1), radius=vis.width // 4, fill=255)
    previews.paste(vis, (i * (vis.width + 20), 0), m)
splash = Image.new('RGBA', vis.size, (5, 43, 85, 255))
splash = Image.alpha_composite(splash, fg_l.crop((SIZE // 6, SIZE // 6, SIZE - SIZE // 6, SIZE - SIZE // 6)))
previews.paste(splash, (2 * (vis.width + 20), 0))
previews.save(f'{out_dir}/icon_preview.png')
print('written')
