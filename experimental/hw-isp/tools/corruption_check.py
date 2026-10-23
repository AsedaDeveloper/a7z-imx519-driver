#!/usr/bin/env python3
"""Score a raw 4656x3496 frame from a7zdual for MIPI link damage.

usage: corruption_check.py FRAME.yuv
Prints the share of magenta-biased pixels and the largest jump in row brightness. On a clean link the first is 0.0000% (it was
0.77% at 1392 Mbit/s, 13.7% at 1500 Mbit/s) and the second stays near 1; broken lines give a jump in the hundreds.
Link errors are data dependent (they appear at particular brightness values), so score a frame with smooth gradients, not a flat patch.
"""
import numpy as np, sys
W, H, HP = 4656, 3496, 3504          # luma plane height is padded to 3504 rows
d = np.fromfile(sys.argv[1], np.uint8)
y = d[:W * HP].reshape(HP, W)[:H].astype(np.float32)
o = W * HP; cw, ch = W // 2, HP // 2
u = d[o:o + cw * ch].reshape(ch, cw)[:H // 2].astype(np.float32) - 128
v = d[o + cw * ch:o + 2 * cw * ch].reshape(ch, cw)[:H // 2].astype(np.float32) - 128
yy = y[::2, ::2]
r = yy + 1.402 * v; g = yy - 0.344136 * u - 0.714136 * v; b = yy + 1.772 * u
mag = ((r + b) / 2 - g > 30) & (g < yy)
print("magenta=%.4f%% rowjump_max=%.1f" % (100 * mag.mean(), np.abs(np.diff(y.mean(1))).max()))
