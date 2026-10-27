#!/usr/bin/env python3
"""Run every declared IMX519 sensor mode through the hardware ISP and report what actually comes out (checklist B16).

usage (on the board, camera service stopped):  mode_matrix.py [--switch]
    BIN=/home/radxa/bin/a7zcap.enc mode_matrix.py        (sudo does not expand ~)

The driver picks a mode by the requested frame rate: `S_PARM` with denominator N selects the mode whose `fps_fixed` is N, and the VIPP scaler then resizes to the
`S_FMT` size. So a mode is addressed as (width, height, fps). Widths above 3264 need the 2-in-1 path (`A7Z_MERGE=1`, /dev/video1).

Each mode is captured twice: once with the exposure left to libisp, and once with `A7Z_MAXEXP=8000` (8 ms). The pair matters, because an exposure longer than the
frame interval holds the rate down -- in ordinary indoor light libisp asks for ~30 ms and every mode above 33 fps is pinned there, which looks exactly like a mode
that cannot reach its rated rate. The last frame is also scored for isolated outlier pixels (as corruption_check.py does), for whatever size the mode produced.

--switch runs the modes back to back, one process per mode and no driver reload in between, to see whether a mode change needs a reload.
"""
import os, re, subprocess, sys, time
import numpy as np

BIN = os.environ.get("BIN", os.path.expanduser("~/bin/a7zcap.enc"))
MODES = [(1280, 720, 85), (1920, 1080, 60), (2328, 1748, 30), (3264, 2448, 13), (3840, 2160, 18), (4656, 3496, 9), (4656, 3496, 15)]


def outliers(path, w, h):
    """Isolated outlier pixels per megapixel on the luma plane; the plane's row count is padded to a multiple of 16."""
    hp = (h + 15) // 16 * 16
    d = np.fromfile(path, np.uint8, count=w * hp)
    if d.size < w * hp:
        return None, None
    y = d.reshape(hp, w)[:h].astype(np.int16)
    c = y[1:-1, 1:-1]
    nb = (y[:-2, 1:-1] + y[2:, 1:-1] + y[1:-1, :-2] + y[1:-1, 2:]) // 4
    return (np.abs(c - nb) > 25).sum() / c.size * 1e6, float(y.mean())


def run(w, h, fps, nframes=90, maxexp=None):
    merge = w > 3264
    os.makedirs("/tmp/cap", exist_ok=True)
    for f in os.listdir("/tmp/cap"):
        if f.startswith("mm_"):
            os.remove("/tmp/cap/" + f)
    env = dict(os.environ, A7Z_FPS=str(fps), A7Z_HEALTH="1", A7Z_EVERY="100000", A7Z_NBUF="4")
    if merge:
        env["A7Z_MERGE"] = "1"
    if maxexp:
        env["A7Z_MAXEXP"] = str(maxexp)
    for _ in range(4):
        p = subprocess.run([BIN, str(w), str(h), str(nframes), "/tmp/cap/mm"], env=env, capture_output=True, text=True, timeout=180)
        if p.returncode != 6:
            break
    got = re.search(r"got (\d+)x(\d+)", p.stderr)
    rates = [float(x) for x in re.findall(r"HEALTH .*fps=([\d.]+)", p.stderr)][1:]
    fr = sorted(f for f in os.listdir("/tmp/cap") if f.startswith("mm_"))
    gw, gh = (int(got[1]), int(got[2])) if got else (w, h)
    sc = lum = None
    if fr:
        sc, lum = outliers("/tmp/cap/" + fr[-1], gw, gh)
    return {"rc": p.returncode, "got": (gw, gh), "fps": max(rates) if rates else None, "outl": sc, "luma": lum, "frames": bool(fr)}


def main():
    print("%-16s %-12s %-6s %-11s %-11s %-6s %s" % ("asked", "delivered", "rated", "auto exp", "8 ms cap", "luma", "outliers/Mpx"))
    for w, h, fps in MODES:
        a = run(w, h, fps, 200)
        b = run(w, h, fps, 200, maxexp=8000)
        fmt = lambda r: "no frames" if not r["frames"] else ("?" if r["fps"] is None else "%.1f fps" % r["fps"])
        print("%-16s %-12s %-6d %-11s %-11s %-6s %s" % ("%dx%d @%d" % (w, h, fps), "%dx%d" % b["got"], fps, fmt(a), fmt(b),
              "-" if b["luma"] is None else "%.0f" % b["luma"], "-" if b["outl"] is None else "%.1f" % b["outl"]), flush=True)
    if "--switch" in sys.argv:
        print("\nmode switch with no driver reload (each line is a fresh process, nothing unloaded in between):")
        seq = [(1280, 720, 85), (4656, 3496, 15), (1920, 1080, 60), (2328, 1748, 30), (1280, 720, 85)]
        for w, h, fps in seq:
            r = run(w, h, fps, max(150, fps * 4), maxexp=8000)
            if not r["frames"]:
                state = "NO FRAMES rc=%s" % r["rc"]
            else:
                state = "ok, %s" % ("rate not measured" if r["fps"] is None else "%.1f fps" % r["fps"])
            print("  -> %-16s %-12s %s" % ("%dx%d @%d" % (w, h, fps), "%dx%d" % r["got"], state), flush=True)


if __name__ == "__main__":
    main()
