#!/usr/bin/env python3
#!/usr/bin/env python3
"""Build /mnt/isp_param_config.bin for libisp (ISP602): [u32 0x1c63c][20B][50B note][116284B param struct].

param = test(124) + 3a(2724) + tuning(99440) + iso(13996).
The base struct is NOT shipped (closed-source Allwinner data): make it from your own library first with
    extract_isp_defaults.py  ->  defaults.bin
usage: mkparam.py DEFAULTS.bin OUT.bin [config.json | '{"kr":1.66,...}']
config keys: kr kb (white-balance scale of the 9 AWB lights), light_rb (absolute [r, b] for each of the 9 AWB lights, applied after kr/kb), ccm_fix/ccm_offset, intpatch, testint, blcpatch, flags_all_on,
             flagbytes, lsc_pattern, lsc_alpha
"""
import sys, struct, json, os, numpy as np
if len(sys.argv) < 3: sys.exit(__doc__)
base = open(sys.argv[1], "rb").read()
if len(base) != 116284: sys.exit("DEFAULTS.bin must be 116284 bytes (made by extract_isp_defaults.py)")
cfgarg = sys.argv[3] if len(sys.argv) > 3 else "{}"
cfg = json.load(open(cfgarg)) if os.path.isfile(cfgarg) else json.loads(cfgarg)
p = bytearray(base)
A3 = 124                      # 3a block offset
LIGHT0 = A3 + 245 * 4         # first awb_light_info entry (10 ints each, 9 lights found; stride 10 ints)
def lights():
    for i in range(9): yield LIGHT0 + i * 40
kr, kb = cfg.get("kr", 1.0), cfg.get("kb", 1.0)
for o in lights():
    r, g, b = struct.unpack_from("<3i", p, o)
    struct.pack_into("<3i", p, o, int(round(r * kr)), g, int(round(b * kb)))
# absolute (r, b) per AWB light, in the ISP's own units (not ratio*256: the response is ~gain^-2.5, see tuning/README); calibrated against measured neutral white points
if cfg.get("light_rb"):
    for o, (r, b) in zip(lights(), cfg["light_rb"]):
        _, g, _ = struct.unpack_from("<3i", p, o)
        struct.pack_into("<3i", p, o, int(r), g, int(b))
# CCM: three 3x3 int16 matrices (unity 256) + 3 offsets each, 12 int16 apart, at tuning-block int16 index 42181 (2800K, 4000K, 6500K)
if cfg.get("ccm_fix"):
    Mf = np.array(cfg["ccm_fix"], dtype=float).reshape(3, 3)
    ofs = np.array(cfg.get("ccm_offset", [0, 0, 0]), dtype=float)
    base = 124 + 2724 + 42181 * 2
    for k in range(3):
        o = base + k * 24
        old = np.frombuffer(bytes(p[o:o + 18]), dtype="<i2").astype(float).reshape(3, 3) / 256.0
        new = np.rint((Mf @ old) * 256.0).astype("<i2")
        p[o:o + 18] = new.tobytes()
        if ofs.any():
            p[o + 18:o + 24] = np.rint(ofs * cfg.get("ccm_offset_scale", 255)).astype("<i2").tobytes()
# generic int32 patch in the tuning block: {"intpatch": {"21147": 100}}  (index in tuning int32 units)
for k, v in (cfg.get("intpatch") or {}).items():
    struct.pack_into("<i", p, 124 + 2724 + 4 * int(k), int(v))
# int32 patch in the test block (31 ints): {"testint": {"20": 1, "21": 35}}
for k, v in (cfg.get("testint") or {}).items():
    struct.pack_into("<i", p, 4 * int(k), int(v))
# per-ISO black-level offsets: 14 records in the iso block (stride 493 int16), 4 int16 each (r, gr, gb, b) at int16 idx 217 + 493*k
if cfg.get("blcpatch"):
    vals = cfg["blcpatch"]
    for k in range(14):
        o = 124 + 2724 + 99440 + 2 * (217 + 493 * k)
        for j in range(4): struct.pack_into("<h", p, o + 2 * j, int(vals[j]))
# stage enable flags live in the test block as packed bytes (ints 22..29 = bytes 88..119)
if cfg.get("flags_all_on"):
    for i in range(88, 120): p[i] = 1
for k, v in (cfg.get("flagbytes") or {}).items(): p[88 + int(k)] = v
# experimental LSC patterns (all 12 tables identical): flat / colramp / rowramp / planeramp
pat = cfg.get("lsc_pattern")
if pat:
    T0 = 124 + 2724 + 144 * 2
    idx = np.arange(768)
    if pat == "flat": v = np.full(768, 512)
    elif pat == "colramp": v = 512 + 48 * (idx % 16)
    elif pat == "rowramp": v = 512 + 12 * (idx // 16)
    elif pat == "planeramp": v = 512 + 128 * (idx // 192)
    v = v.astype("<u2")
    for k in range(12): p[T0 + k * 1536:T0 + (k + 1) * 1536] = v.tobytes()
# lens-shading tables: 12 tables x 768 u16 in the tuning block starting at u16 index 144; value 512 = 1.0x
alpha = cfg.get("lsc_alpha", 1.0)
if alpha != 1.0:
    T0 = 124 + 2724 + 144 * 2
    for k in range(12):
        o = T0 + k * 768 * 2
        a = np.frombuffer(bytes(p[o:o + 1536]), dtype="<u2").astype(float)
        n = np.clip(512 + alpha * (a - 512), 128, 16383).astype("<u2")
        p[o:o + 1536] = n.tobytes()
out = struct.pack("<I", 116284) + b"a7z-imx519-tune".ljust(20, b"\0") + b"a7z imx519 calibrated".ljust(50, b"\0") + bytes(p)
open(sys.argv[2], "wb").write(out)
print("wrote", sys.argv[2], len(out), "kr=%.3f kb=%.3f" % (kr, kb))
