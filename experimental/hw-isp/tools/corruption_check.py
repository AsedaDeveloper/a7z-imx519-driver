#!/usr/bin/env python3
"""Score a raw 4656x3496 frame from a7zdual for MIPI link damage.

usage: corruption_check.py FRAME.yuv
Prints the share of magenta-biased pixels, how scattered they are, and the largest jump in row brightness. On a clean link the first is 0.0000% (it was
0.77% at 1392 Mbit/s, 13.7% at 1500 Mbit/s) and the second stays near 1; broken lines give a jump in the hundreds.
Link errors are data dependent (they appear at particular brightness values), so score a frame with smooth gradients, not a flat patch.

Limits: it scores ONE frame, assumes the 4656x3496 a7zdual layout, and only looks for those two symptoms. A clean result is evidence for
that frame and scene, not proof that every frame is free of errors.
"""
import numpy as np, sys
if len(sys.argv) != 2:
    sys.exit(__doc__)
W, H, HP = 4656, 3496, 3504          # luma plane height is padded to 3504 rows
d = np.fromfile(sys.argv[1], np.uint8)
y = d[:W * HP].reshape(HP, W)[:H].astype(np.float32)
o = W * HP; cw, ch = W // 2, HP // 2
u = d[o:o + cw * ch].reshape(ch, cw)[:H // 2].astype(np.float32) - 128
v = d[o + cw * ch:o + 2 * cw * ch].reshape(ch, cw)[:H // 2].astype(np.float32) - 128
yy = y[::2, ::2]
r = yy + 1.402 * v; g = yy - 0.344136 * u - 0.714136 * v; b = yy + 1.772 * u
mag = ((r + b) / 2 - g > 30) & (g < yy)
n = int(mag.sum())
# A real magenta object (a pink label) also scores. Link damage is scattered speckle; objects are dense blobs. So also report the share of
# magenta pixels that sit in a sparse 9x9 neighbourhood (<= 50% magenta): ~100% for speckle, ~5-8% for the blobs seen in a bottle scene.
sparse = 0.0
if n:
    k = 9
    c = np.pad(mag.astype(np.int32), ((k // 2 + 1, k // 2), (k // 2 + 1, k // 2))).cumsum(0).cumsum(1)
    nb = c[k:, k:] - c[:-k, k:] - c[k:, :-k] + c[:-k, :-k]
    sparse = 100.0 * (mag & (nb <= k * k // 2)).sum() / n
print("magenta=%.4f%% (%d px) sparse_share=%.1f%% rowjump_max=%.1f" % (100 * mag.mean(), n, sparse, np.abs(np.diff(y.mean(1))).max()))
rj = np.abs(np.diff(y.mean(1))).max()
if rj >= 20:
    print("LARGE row-brightness jumps: broken or missing lines, link damage very likely")
elif n == 0:
    print("no magenta pixels and no row jumps in this frame")
elif sparse > 50 and n >= 100:
    print("magenta is mostly SCATTERED: looks like link speckle")
elif 100.0 * mag.mean() > 2:
    print("LARGE magenta area: a dense ring/blotch of this size was seen as damage at 1500 Mbit/s; look at the picture before trusting it")
else:
    print("magenta is mostly in dense blobs: probably real content - check the picture; a few speckle pixels could hide in it")
