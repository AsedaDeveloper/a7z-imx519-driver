#!/usr/bin/env python3
"""A1: one PLTM element at a time, x2 and x0.5 of its deployed value, against interleaved baselines. Restores the param file at the end."""
import subprocess, sys, time, os, shutil, struct, json
sys.path.insert(0, "/usr/local/lib")
from a7z_v4l2 import Camera
P = "/mnt/isp_param_config.bin"; BK = "/home/radxa/isp_param_config.bin.before_a1"
PLTM = 74 + 124 + 2724 + 84460
def sh(c): return subprocess.run(c, shell=True, capture_output=True, text=True)
def deploy(patch):
    d = bytearray(open(BK, "rb").read())
    for i, v in patch.items(): struct.pack_into("<i", d, PLTM + 4 * i, v)
    open("/tmp/variant.bin", "wb").write(d)
    sh("sudo rm -f /mnt/isp0_*_ctx_saved.bin; sudo install -m644 /tmp/variant.bin %s; sudo systemctl restart a7z-camera" % P)
def measure(n=100):
    time.sleep(16)
    clip = []; L = []; H = []; P99 = []
    with Camera() as cam:
        for _ in range(25): cam.read()
        for _ in range(n):
            y = cam.read()[0]
            a = y[::5]
            clip.append(sum(1 for v in a if v >= 250) / len(a))
            L.append(sum(a) / len(a)); P99.append(sorted(a[::40])[int(len(a[::40]) * .99)])
            rows = [y[i * 1920:(i + 1) * 1920] for i in range(200, 900, 100)]
            H.append(sum(abs(r[i] - r[i + 1]) for r in rows for i in range(0, 1900, 3)) / (len(rows) * 634))
    f = lambda x: sum(x) / len(x)
    return dict(clip=100 * f(clip), luma=f(L), p99=f(P99), hp=f(H))
if not os.path.exists(BK): shutil.copy(P, BK)
d0 = open(BK, "rb").read(); base = list(struct.unpack_from("<45i", d0, PLTM))
elems = [int(x) for x in sys.argv[1].split(",")] if len(sys.argv) > 1 else [i for i, v in enumerate(base) if v not in (0, 1)]
out = open("/home/radxa/a1sweep.jsonl", "a")
try:
    for k, i in enumerate(elems):
        if k % 4 == 0:
            r = measure() if (deploy({}) or True) else None
            out.write(json.dumps(dict(kind="base", elem=None, val=None, **r)) + "\n"); out.flush(); print("base", r, flush=True)
        for mult, name in ((2.0, "x2"), (0.5, "x0.5")):
            v = int(base[i] * mult)
            deploy({i: v}); r = measure()
            out.write(json.dumps(dict(kind=name, elem=i, val=v, orig=base[i], **r)) + "\n"); out.flush()
            print("elem %2d %-5s (%d -> %d) %s" % (i, name, base[i], v, r), flush=True)
finally:
    sh("sudo rm -f /mnt/isp0_*_ctx_saved.bin; sudo install -m644 %s %s; sudo systemctl restart a7z-camera" % (BK, P))
    print("restored", flush=True)
