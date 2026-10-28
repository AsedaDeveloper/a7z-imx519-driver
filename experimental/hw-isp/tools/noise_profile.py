#!/usr/bin/env python3
"""Temporal noise of the ISP's output at pinned sensor gains (checklist B14).

usage (on the board, service running, scene STATIC):  noise_profile.py [EXPOSURE_US] [FRAMES]      default 30000, 40
Pins the exposure and steps the sensor gain through the control file (`exp N`, `gain N`; 16 = 1x), holds the lens, and for each gain captures FRAMES frames, then reports the
mean luma and the per-pixel temporal standard deviation over a sparse grid, for pixels whose mean sits in the mid-tones (20..230). Lens covered gives the black level and the dark noise;
a lit static scene gives the noise the ISP leaves at that brightness. Ends with `free` (auto exposure back) and autofocus back on.
"""
import subprocess, sys, time
sys.path.insert(0, "/usr/local/lib")
from a7z_v4l2 import Camera
exp = int(sys.argv[1]) if len(sys.argv) > 1 else 30000
nfr = int(sys.argv[2]) if len(sys.argv) > 2 else 40
GAINS = ([int(g) for g in sys.argv[3].split(',')] if len(sys.argv) > 3
         else [16, 24, 32, 48, 64, 96, 128, 192, 256, 384, 512])
W, H = 1920, 1080
GRID = [(r * W + c) for r in range(60, H - 60, 24) for c in range(60, W - 60, 24)]
def ctl(s): open("/run/a7zcam.ctl", "w").write(s + "\n")
def v4l(name, v): subprocess.run(["v4l2-ctl", "-d", "/dev/video9", "--set-ctrl", "%s=%d" % (name, v)], check=True)
try:
    v4l("focus_automatic_continuous", 0)
    with Camera() as cam:
        ctl("exp %d" % exp); time.sleep(1.2)
        print("exposure pinned at %d us; grid %d pixels, %d frames per gain" % (exp, len(GRID), nfr))
        print("%6s %6s %9s %9s %9s %9s %7s" % ("gain", "x", "mean Y", "min Y", "std (dark)", "std (mid)", "n mid"))
        for g in GAINS:
            ctl("gain %d" % g); time.sleep(1.3)
            for _ in range(8): cam.read()
            S = [0.0] * len(GRID); Q = [0.0] * len(GRID); n = 0
            for _ in range(nfr):
                y = cam.read()[0]
                for i, p in enumerate(GRID):
                    v = y[p]; S[i] += v; Q[i] += v * v
                n += 1
            mean = [s / n for s in S]; var = [max(q / n - m * m, 0) for q, m in zip(Q, mean)]
            mid = [v for m, v in zip(mean, var) if 20 <= m <= 230]
            dark = [v for m, v in zip(mean, var) if m < 20]
            f = lambda x: (sum(x) / len(x)) ** 0.5 if x else float("nan")
            print("%6d %5.1fx %9.1f %9.1f %9.2f %9.2f %7d" % (g, g / 16, sum(mean) / len(mean), min(mean), f(dark), f(mid), len(mid)), flush=True)
finally:
    ctl("free"); time.sleep(0.5); v4l("focus_automatic_continuous", 1)
