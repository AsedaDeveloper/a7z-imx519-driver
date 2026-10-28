#!/usr/bin/env python3
"""Hot / defective pixel map from sealed-lens frames (checklist C4). Run on the board with the lens light-tight.
Pins gain and exposure, averages N frames of the loopback node and lists pixels whose mean Y stands out.
Sees the pixels that survive the ISP's own defect-pixel correction at the loopback node's resolution, not the sensor's raw array.
usage: hotpix_map.py [gain_code=512] [exp_us=30000] [frames=60] [threshold=3]"""
import sys, time, cv2, numpy as np
gain = int(sys.argv[1]) if len(sys.argv) > 1 else 512
exp = int(sys.argv[2]) if len(sys.argv) > 2 else 30000
n = int(sys.argv[3]) if len(sys.argv) > 3 else 60
thr = float(sys.argv[4]) if len(sys.argv) > 4 else 3.0
def ctl(s): open('/run/a7zcam.ctl', 'w').write(s + '\n'); time.sleep(1.3)
cap = cv2.VideoCapture('/dev/video9', cv2.CAP_V4L2)
cap.set(cv2.CAP_PROP_CONVERT_RGB, 0)
W, H = 1920, 1080
for _ in range(10): cap.read()
ctl('exp %d' % exp); ctl('gain %d' % gain)
for _ in range(14): cap.read()
acc = None; cnt = 0
for _ in range(n):
    ok, f = cap.read()
    if not ok: continue
    raw = f.reshape(-1)                              # CONVERT_RGB=0 hands back the planar YU12 bytes; the Y plane is the first W*H of them
    y = raw[:W * H].reshape(H, W).astype(np.float32)
    acc = y if acc is None else acc + y; cnt += 1
ctl('free')
m = acc / cnt
print('frame %dx%d, %d frames, gain code %d (%.1fx), exp %d us' % (m.shape[1], m.shape[0], cnt, gain, gain / 16, exp))
print('mean %.3f  max %.1f  pixels >0.5: %d  >=%.0f: %d' % (m.mean(), m.max(), (m > 0.5).sum(), thr, (m >= thr).sum()))
ys, xs = np.where(m >= thr)
order = np.argsort(-m[ys, xs])[:40]
for i in order: print('  x %4d y %4d  mean %.1f' % (xs[i], ys[i], m[ys[i], xs[i]]))
np.save('/tmp/hotpix_mean.npy', m)
