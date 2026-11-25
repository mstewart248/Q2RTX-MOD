import sys, glob, os, numpy as np
from PIL import Image, ImageDraw
d = sys.argv[1]
OW, OH = 1920, 1080
def load(p, ch):
    return np.fromfile(p, dtype=np.float32).reshape(OH, OW, ch)[..., :3]
def tm(x):
    x = np.clip(x, 0, None); return (x / (1 + x)) ** (1 / 2.2)
def lum(x): return x @ np.array([0.25, 0.5, 0.25], dtype=np.float32)
def grad(x):
    return np.abs(np.diff(x, axis=0))[:, :-1].mean() + np.abs(np.diff(x, axis=1))[:-1].mean()
gts = {k: load(glob.glob(f"{d}/gt_{k}_*.f32")[0], 3) for k in ("ripple", "static") if glob.glob(f"{d}/gt_{k}_*.f32")}
crop = (slice(380, 700), slice(760, 1160))   # around the torch
rows = []
tiles = []
for p in sorted(glob.glob(f"{d}/[0-9A-Z][0-9]_*.f32")):
    name = os.path.basename(p).split("_1920")[0]
    out = load(p, 4)
    own = glob.glob(f"{d}/gt_{name}_*.f32")
    gt = load(own[0], 3) if own else gts["static" if "static" in name else "ripple"]
    a, b = tm(out), tm(gt)
    m = (slice(100, OH - 100), slice(100, OW - 100))
    mse = ((a[m] - b[m]) ** 2).mean()
    psnr = 10 * np.log10(1 / mse)
    sharp = grad(lum(a[m])) / grad(lum(b[m]))
    rows.append((name, psnr, sharp, float(out[m].mean() / gt[m].mean())))
    tiles.append((name, a[crop]))
if "ripple" in gts: tiles.insert(0, ("GT ripple", tm(gts["ripple"])[crop]))
for g in sys.argv[2:]:
    tiles.insert(0, ("GT " + g, tm(load(glob.glob(f"{d}/gt_{g}*.f32")[0], 3))[crop]))
print(f"{'config':32s} {'PSNR':>6s} {'sharp':>6s} {'bright':>6s}")
for r in rows: print(f"{r[0]:32s} {r[1]:6.2f} {r[2]:6.2f} {r[3]:6.2f}")
sel = sys.argv[2:] 
def sheet(items, path, cols=4):
    th, tw = items[0][1].shape[:2]
    n = len(items); rws = (n + cols - 1) // cols
    im = Image.new("RGB", (cols * tw, rws * (th + 16)))
    dr = ImageDraw.Draw(im)
    for i, (nm, t) in enumerate(items):
        x, y = (i % cols) * tw, (i // cols) * (th + 16)
        im.paste(Image.fromarray((np.clip(t, 0, 1) * 255).astype(np.uint8)), (x, y + 16))
        dr.text((x + 3, y + 2), nm, fill=(255, 255, 255))
    im.save(path)
sheet(tiles[:16], f"{d}/sheet1.png")
if len(tiles) > 16: sheet(tiles[16:], f"{d}/sheet2.png")
