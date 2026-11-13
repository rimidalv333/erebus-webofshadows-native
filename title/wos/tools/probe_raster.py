#!/usr/bin/env python3
"""fix89: rasterise f*_probe*.bin meshes (CPU clip-space triangles) to 160x90 coverage masks.
Usage: probe_raster.py DUMPDIR [out.png]  - prints per-mesh coverage, writes a montage."""
import sys, struct, glob, os
import numpy as np
from PIL import Image
W, H = 160, 90
def clip_poly(poly):
    # clip against w > eps and z >= 0 (D3D) planes, homogeneous
    def clip(poly, f):
        out = []
        for i in range(len(poly)):
            a, b = poly[i], poly[(i + 1) % len(poly)]
            fa, fb = f(a), f(b)
            if fa >= 0: out.append(a)
            if (fa >= 0) != (fb >= 0):
                t = fa / (fa - fb); out.append(a + (b - a) * t)
        return out
    poly = clip(poly, lambda v: v[3] - 1e-5)
    if poly: poly = clip(poly, lambda v: v[2])
    if poly: poly = clip(poly, lambda v: v[3] - v[2])
    return poly
def raster(tris):
    m = np.zeros((H, W), bool)
    ys, xs = np.mgrid[0:H, 0:W]
    px = (xs + 0.5) / W * 2 - 1; py = 1 - (ys + 0.5) / H * 2
    for t in tris:
        poly = clip_poly([t[0:4], t[4:8], t[8:12]])
        if len(poly) < 3: continue
        pts = [(v[0] / v[3], v[1] / v[3]) for v in poly]
        for i in range(1, len(pts) - 1):
            (ax, ay), (bx, by), (cx, cy) = pts[0], pts[i], pts[i + 1]
            d = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax)
            if abs(d) < 1e-12: continue
            w0 = ((bx - px) * (cy - py) - (by - py) * (cx - px)) / d
            w1 = ((cx - px) * (ay - py) - (cy - py) * (ax - px)) / d
            m |= (w0 >= 0) & (w1 >= 0) & (1 - w0 - w1 >= 0)
    return m
files = sorted(glob.glob(os.path.join(sys.argv[1], '*_probe*.bin')))
ims = []
for f in files:
    b = open(f, 'rb').read()
    magic, ver = struct.unpack_from('<II', b, 0); h, = struct.unpack_from('<Q', b, 8)
    prim, cnt, tris = struct.unpack_from('<III', b, 16)
    arr = np.frombuffer(b, '<f4', tris * 12, 28).reshape(tris, 12).astype(np.float64)
    m = raster(arr)
    print(f'{os.path.basename(f)} vs {h:016x} prim {prim} idx {cnt} tris {tris} coverage {m.mean()*100:.1f}% centre {m[H//2-5:H//2+5, W//2-5:W//2+5].mean()*100:.0f}%')
    ims.append(Image.fromarray((m * 255).astype(np.uint8)))
if ims and len(sys.argv) > 2:
    cols = 6; rows = (len(ims) + cols - 1) // cols
    M = Image.new('L', (cols * (W + 4), rows * (H + 4)), 60)
    for i, im in enumerate(ims): M.paste(im, ((i % cols) * (W + 4), (i // cols) * (H + 4)))
    M.save(sys.argv[2])
