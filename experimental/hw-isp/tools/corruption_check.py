#!/usr/bin/env python3
"""Score a raw 4656x3496 frame from a7zdual for MIPI link damage.

usage: corruption_check.py FRAME.yuv

Primary measure: isolated outlier pixels per megapixel (a pixel that differs from the mean of its 4 neighbours by more than 25).
Link bit errors scatter such pixels; on a clean frame there are essentially none. Measured on the test module (bright scenes with real
edges): 0.1 at 1224-1296 Mbit/s, 143 at 1344, 10538 at 1368, 39103 at 1392, 233000+ at 1500; 0.0 in a dim scene at 1296 and at the stock 816.
Secondary: magenta-biased pixels and how scattered they are, and the largest jump in row brightness (broken lines). The magenta test
is unreliable in dim scenes (the ISP's low-light colour cast scores as "magenta" even at the stock clocks), so it is skipped there.

Limits: it scores ONE frame, assumes the 4656x3496 a7zdual layout, and very sharp detail (printed text, hard edges) raises the outlier
floor, so judge a score against a capture of the SAME scene at the stock clocks (A7Z_FPS=9). A clean result is evidence for that frame and
scene, not proof that every frame is free of errors.
"""
import numpy as np, sys
if len(sys.argv) != 2:
    sys.exit(__doc__)
W, H, HP = 4656, 3496, 3504          # luma plane height is padded to 3504 rows
d = np.fromfile(sys.argv[1], np.uint8)
y = d[:W * HP].reshape(HP, W)[:H].astype(np.int16)
o = W * HP; cw, ch = W // 2, HP // 2
u = d[o:o + cw * ch].reshape(ch, cw)[:H // 2].astype(np.float32) - 128
v = d[o + cw * ch:o + 2 * cw * ch].reshape(ch, cw)[:H // 2].astype(np.float32) - 128
mean = float(y.mean())
c = y[1:-1, 1:-1]; nb4 = (y[:-2, 1:-1] + y[2:, 1:-1] + y[1:-1, :-2] + y[1:-1, 2:]) // 4
imp = (np.abs(c - nb4) > 25).sum() / c.size * 1e6
rj = np.abs(np.diff(y.astype(np.float32).mean(1))).max()
yy = y[::2, ::2].astype(np.float32)
r = yy + 1.402 * v; g = yy - 0.344136 * u - 0.714136 * v; b = yy + 1.772 * u
mag = ((r + b) / 2 - g > 30) & (g < yy)
n = int(mag.sum())
sparse = 0.0
if n:
    k = 9
    cs = np.pad(mag.astype(np.int32), ((k // 2 + 1, k // 2), (k // 2 + 1, k // 2))).cumsum(0).cumsum(1)
    nbm = cs[k:, k:] - cs[:-k, k:] - cs[k:, :-k] + cs[:-k, :-k]
    sparse = 100.0 * (mag & (nbm <= k * k // 2)).sum() / n
print("outliers=%.1f/Mpx  magenta=%.4f%% (%d px, sparse_share=%.1f%%)  rowjump_max=%.1f  mean_luma=%.1f" % (imp, 100 * mag.mean(), n, sparse, rj, mean))
if rj >= 20:
    print("LARGE row-brightness jumps: broken or missing lines, link damage very likely")
elif imp >= 1000:
    print("MANY isolated outlier pixels: link damage very likely")
elif imp >= 20:
    print("TRACE of isolated outlier pixels: possible early link damage, or very sharp detail; compare with the same scene at stock clocks")
elif mean < 40:
    print("no outliers and no row jumps (dim scene: the magenta test is not meaningful here)")
elif n == 0:
    print("no outliers, no magenta pixels, no row jumps in this frame")
elif sparse > 50 and n >= 100:
    print("magenta is mostly SCATTERED: looks like link speckle")
elif 100.0 * mag.mean() > 2:
    print("LARGE magenta area but no outliers: a colour cast or a very magenta scene; look at the picture")
else:
    print("magenta is mostly in dense blobs and no outliers: probably real content - check the picture")
