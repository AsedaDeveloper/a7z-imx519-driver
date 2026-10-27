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
out = open("/home/radxa/a1pair.jsonl", "a")
plan = []
for rep in range(3):
    plan += [("base", {}), ("e1=16", {1: 16}), ("base", {}), ("e1=8", {1: 8}), ("base", {}), ("e1=0", {1: 0})]
plan.append(("base", {}))
try:
    for name, patch in plan:
        deploy(patch); r = measure()
        out.write(json.dumps(dict(name=name, **r)) + "\n"); out.flush()
        print("%-6s clip %.2f%%  luma %.0f  p99 %.0f  hp %.3f" % (name, r["clip"], r["luma"], r["p99"], r["hp"]), flush=True)
finally:
    sh("sudo rm -f /mnt/isp0_*_ctx_saved.bin; sudo install -m644 %s %s; sudo systemctl restart a7z-camera" % (BK, P))
    print("restored", flush=True)
