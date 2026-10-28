#!/usr/bin/env python3
"""Exposure-bracket HDR merge (checklist C3). Run on the board while a7z-camera is running; the scene must hold still.
Pins gain (default 1x) and steps exposure through the list through the control file, grabs a frame at each, merges with
Mertens exposure fusion, and prints how many pixels are clipped (>=250) or crushed (<=5) in each frame and in the merge.
usage: hdr_merge.py [out_prefix] [exposures_us,comma,separated] [gain_code=16]"""
import sys, time, cv2, numpy as np
CTL = '/run/a7zcam.ctl'
gain = int(sys.argv[3]) if len(sys.argv) > 3 else 16
prefix = sys.argv[1] if len(sys.argv) > 1 else 'hdr'
exps = [int(x) for x in sys.argv[2].split(',')] if len(sys.argv) > 2 else [500, 2000, 8000, 30000]

def ctl(line):
    open(CTL, 'w').write(line + '\n'); time.sleep(1.3)      # the feeder polls the file's mtime at 1 s resolution

cap = cv2.VideoCapture('/dev/video9', cv2.CAP_V4L2)
cap.set(cv2.CAP_PROP_AUTOFOCUS, 0)                           # hold the lens so every frame sees the same focus
def grab(n):
    last = None
    for _ in range(n):
        ok, f = cap.read()
        if ok: last = f
    return last

grab(10)
ctl('gain %d' % gain)
frames = []
for e in exps:
    ctl('exp %d' % e)
    f = grab(14)                                             # pipeline latency is 4 frames; the rest lets it settle
    frames.append(f)
    y = cv2.cvtColor(f, cv2.COLOR_BGR2GRAY)
    print('exp %6d us  mean %6.1f  clipped %5.2f%%  crushed %5.2f%%' % (e, y.mean(), 100 * (y >= 250).mean(), 100 * (y <= 5).mean()))
    cv2.imwrite('%s_%d.png' % (prefix, e), f)
ctl('free')
cap.set(cv2.CAP_PROP_AUTOFOCUS, 1)

m = cv2.createMergeMertens().process(frames)                 # float, ~0..1
m8 = np.clip(m * 255, 0, 255).astype(np.uint8)
y = cv2.cvtColor(m8, cv2.COLOR_BGR2GRAY)
print('merged          mean %6.1f  clipped %5.2f%%  crushed %5.2f%%' % (y.mean(), 100 * (y >= 250).mean(), 100 * (y <= 5).mean()))
cv2.imwrite('%s_merged.png' % prefix, m8)
