#!/usr/bin/env python3
"""a7zcam - camera tool for the Waveshare/Arducam IMX519-78 module on the Radxa Cubie A7Z.

Works on top of the imx519_mipi + ak7375_act kernel drivers (raw RAW10 Bayer, no ISP). Everything the Raspberry
Pi's ISP would do (black level, white balance, demosaic, gamma, auto exposure, contrast autofocus) is done here
in user space.

  a7zcam still photo.jpg [--size 1080p|4k|full] [--af] [--exposure 0.02 --gain 2] [--raw photo.dng]
  a7zcam focus --af            run autofocus and print the lens position
  a7zcam focus --set 2400      move the lens (0 = infinity .. 4095 = nearest)
  a7zcam live [--port 8080]    web live view with controls

Run it as a user in the groups video + i2c (the default 'radxa' user is).
"""
import os, sys, time, json, fcntl, struct, argparse, threading, subprocess
import numpy as np, cv2

BLACK, FULL = 64.0, 1023.0
PIXCLK = 426_666_667
# name: (width, height, buffer rows, line length in pixel clocks, frame length in lines)
MODES = {
    "720p": (1280, 720, 720, 6144, 820),
    "1080p": (1920, 1080, 1088, 6027, 1180),
    "2k": (2328, 1748, 1766, 6512, 2184),
    "4k": (3840, 2160, 2160, 10486, 2260),
    "full": (4224, 3496, 3504, 12650, 3572),     # the sensor is 4656 wide, the VIN keeps the left 4224
}
I2C_BUS, SENSOR_ADDR, VCM_ADDR = "/dev/i2c-3", 0x1A, 0x0C
PARAMS = "/sys/module/imx519_mipi/parameters"


class I2C:
    def __init__(self):
        self.fd = os.open(I2C_BUS, os.O_RDWR)
        self.lock = threading.Lock()

    def write(self, addr, data):
        with self.lock:
            fcntl.ioctl(self.fd, 0x0706, addr)            # I2C_SLAVE_FORCE
            os.write(self.fd, bytes(data))


class Camera:
    """One camera stream (a V4L2 session held open by a v4l2-ctl helper) plus live control of the sensor and lens."""

    def __init__(self, mode="1080p", fifo="/tmp/a7zcam.fifo"):
        self.mode = mode
        self.W, self.H, self.SH, self.hts, self.vts = MODES[mode]
        self.line_us = self.hts / (PIXCLK / 1e6)
        self.fsz = self.W * self.SH * 2
        self.fifo = fifo
        self.i2c = I2C()
        self.exp, self.gain, self.pos = 0.02, 2.0, 2400
        self.lock = threading.Lock()
        self.raw, self.seq, self.fps = None, 0, 0.0
        self.proc, self.stop_flag = None, False
        self.wb = np.array([1.9, 1.0, 1.7], np.float32)

    # ------------------------------------------------------------------ sensor and lens (live, over I2C)
    def _w(self, reg, val):
        self.i2c.write(SENSOR_ADDR, [reg >> 8, reg & 255, val])

    def set_exposure(self, sec):
        self.exp = min(max(sec, 1e-4), 60.0)
        lines = max(20, round(self.exp * 1e6 / self.line_us))
        frame, shift = max(lines + 32, self.vts), 0
        while frame > 65535 and shift < 7:
            shift += 1; frame >>= 1
        frame = min(frame, 65535)
        coarse = max(1, lines >> shift)
        self._w(0x0104, 1); self._w(0x3100, shift)
        self._w(0x0340, frame >> 8); self._w(0x0341, frame & 255)
        self._w(0x0202, coarse >> 8); self._w(0x0203, coarse & 255); self._w(0x0104, 0)

    def set_gain(self, g):
        self.gain = g = min(max(g, 1.0), 64.0)
        ana, dig = (int(1024 - 1024 / g), 0x100) if g <= 16 else (960, int(g / 16 * 256))
        self._w(0x0104, 1)
        self._w(0x0204, min(ana, 960) >> 8); self._w(0x0205, min(ana, 960) & 255)
        self._w(0x020E, min(dig, 0xFFFF) >> 8); self._w(0x020F, min(dig, 0xFFFF) & 255); self._w(0x0104, 0)

    def set_focus(self, pos):
        """AK7375 lens motor: 0 = infinity .. 4095 = nearest. The position must be ONE 3-byte write."""
        self.pos = int(min(max(pos, 0), 4095))
        self.i2c.write(VCM_ADDR, [0x02, 0x00])
        self.i2c.write(VCM_ADDR, [0x00, (self.pos >> 4) & 0xFF, (self.pos & 0xF) << 4])

    # ------------------------------------------------------------------ stream
    def start(self):
        for p, v in (("exp_us", int(self.exp * 1e6)), ("gain_x16", int(self.gain * 16))):
            with open(f"{PARAMS}/{p}", "w") as f:
                f.write(str(v))
        if os.path.exists(self.fifo):
            os.remove(self.fifo)
        os.mkfifo(self.fifo)
        self.proc = subprocess.Popen(
            ["v4l2-ctl", "-d", "/dev/video0", "--set-input=0",
             f"--set-fmt-video=width={self.W},height={self.H},pixelformat=RG10", "--stream-mmap=4",
             "--stream-count=1000000000", f"--stream-to={self.fifo}"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        threading.Thread(target=self._reader, daemon=True).start()
        self.set_focus(self.pos)
        if self.latest(timeout=15)[0] is None:
            raise RuntimeError("no frames from the camera (is the imx519_mipi driver loaded?)")
        return self

    def _reader(self):
        f = open(self.fifo, "rb", buffering=0)
        buf = bytearray(self.fsz); mv = memoryview(buf)
        t0, n = time.time(), 0
        while not self.stop_flag:
            got = 0
            while got < self.fsz:
                r = f.readinto(mv[got:])
                if not r:
                    return
                got += r
            a = np.frombuffer(buf, "<u2").reshape(self.SH, self.W)[:self.H].copy()
            with self.lock:
                self.raw, self.seq = a, self.seq + 1
            n += 1
            if time.time() - t0 >= 1:
                self.fps, n, t0 = n / (time.time() - t0), 0, time.time()

    def latest(self, after=None, timeout=5.0):
        t0 = time.time()
        while time.time() - t0 < timeout:
            with self.lock:
                if self.raw is not None and (after is None or self.seq > after):
                    return self.raw, self.seq
            time.sleep(0.003)
        return None, None

    def wait_frames(self, n=2):
        """Return a frame exposed entirely AFTER now (needs n further frames because of the sensor pipeline)."""
        _, s = self.latest()
        a = None
        for _ in range(n):
            a, s = self.latest(after=s, timeout=max(5.0, 4 * self.exp + 3))
        return a

    def close(self):
        self.stop_flag = True
        if self.proc:
            self.proc.terminate()
            try:
                self.proc.wait(5)
            except Exception:
                self.proc.kill()
        try:
            os.remove(self.fifo)
        except OSError:
            pass

    def __enter__(self): return self.start()
    def __exit__(self, *a): self.close()

    # ------------------------------------------------------------------ image processing
    @staticmethod
    def planes(a):
        a = a.astype(np.float32) - BLACK
        return a[0::2, 0::2], (a[0::2, 1::2] + a[1::2, 0::2]) * 0.5, a[1::2, 1::2]

    def awb_gains(self, a):
        r, g, b = self.planes(a)
        mr, mg, mb = max(r.mean(), 1.0), max(g.mean(), 1.0), max(b.mean(), 1.0)
        return np.clip(np.array([mg / mr, 1.0, mg / mb], np.float32), 0.3, 6.0)

    def to_rgb(self, a, full_res=False, wb=None):
        """16-bit RAW10 mosaic -> 8-bit RGB. full_res=True demosaics (cv2); otherwise 2x2 binning (fast)."""
        wb = self.wb if wb is None else wb
        if full_res:
            lin = np.clip((a.astype(np.float32) - BLACK) / (FULL - BLACK), 0, 1)
            m = (lin * 65535).astype(np.uint16)
            rgb = cv2.cvtColor(m, cv2.COLOR_BAYER_BG2RGB_EA).astype(np.float32) / 65535.0   # OpenCV names Bayer by the 2nd row
            rgb *= wb[None, None, :]
        else:
            r, g, b = self.planes(a)
            rgb = np.dstack([r * wb[0], g * wb[1], b * wb[2]]) / (FULL - BLACK)
        return (np.clip(rgb, 0, 1) ** (1 / 2.2) * 255).astype(np.uint8)

    def sharpness(self, a):
        _, g, _ = self.planes(a)
        h, w = g.shape
        c = g[h // 3: 2 * h // 3, w // 3: 2 * w // 3]
        m = max(float(c.mean()), 1.0)
        return float(cv2.Laplacian(cv2.GaussianBlur(c, (0, 0), 1.2), cv2.CV_32F).var() / (m * m) * 1000.0)

    # ------------------------------------------------------------------ auto exposure / autofocus
    def auto_exposure(self, target=0.30, max_exp=0.5, iters=14, settle=3):
        """Closed-loop AE: aim for the 60th percentile of green at `target` of full scale."""
        for _ in range(iters):
            a = self.wait_frames(settle)
            _, g, _ = self.planes(a)
            med = float(np.percentile(g[::4, ::4], 60)) / (FULL - BLACK)
            sat = float((g[::4, ::4] > 0.95 * (FULL - BLACK)).mean())
            if sat > 0.02:
                med = max(med, 0.6) * (1 + 4 * sat)
            ratio = 4.0 if med <= 0.004 else (target / med) ** 0.8
            if abs(ratio - 1) < 0.1:
                break
            total = min(max(self.exp * self.gain * ratio, 1e-4), max_exp * 32.0)
            e = min(total, 1 / 15); g_ = max(total / e, 1.0)
            if g_ > 8.0:
                e = min(total / 8.0, max_exp); g_ = max(total / e, 1.0)
            self.set_exposure(e); self.set_gain(min(g_, 32.0))
        return self.exp, self.gain

    def _measure(self, pos, n=2):
        self.set_focus(pos)
        time.sleep(max(0.2, 3 * self.exp))
        vals = []
        for _ in range(n):
            a = self.wait_frames(2 if self.exp < 0.3 else 1)
            vals.append(self.sharpness(a))
        return float(np.mean(vals))

    def autofocus(self, coarse=256, fine=64, log=None):
        """Contrast-detect AF: coarse sweep of the whole range, then a fine sweep around the peak."""
        res = [(p, self._measure(p)) for p in range(0, 4096, coarse)]
        best = max(res, key=lambda r: r[1])[0]
        res2 = [(p, self._measure(p)) for p in range(max(0, int(best - 1.25 * coarse)), min(4095, int(best + 1.25 * coarse)) + 1, fine)]
        best = max(res2, key=lambda r: r[1])
        self.set_focus(best[0])
        if log is not None:
            log.extend(res + res2)
        return best


# ---------------------------------------------------------------------------------------- DNG writer
def write_dng(path, bayer, black=64, white=1023, exp=None, gain=None, as_shot=(1.0, 1.0, 1.0)):
    """Minimal uncompressed 16-bit CFA (RGGB) DNG so the raw frame opens in Lightroom/RawTherapee/darktable."""
    h, w = bayer.shape
    data = (bayer.astype("<u2") << 6).tobytes()                       # 10 bit -> 16 bit scale
    entries = []                                                      # (tag, type, count, value-bytes)

    def e(tag, typ, vals):
        fmt = {1: "B", 3: "H", 4: "I", 5: "II", 7: "B", 10: "ii", 2: "c"}[typ]
        if typ == 2:
            b = vals.encode() + b"\0"; cnt = len(b)
        elif typ in (5, 10):
            b = b"".join(struct.pack("<" + fmt, *v) for v in vals); cnt = len(vals)
        else:
            vals = list(vals) if hasattr(vals, "__iter__") else [vals]
            b = struct.pack("<" + fmt * len(vals), *vals); cnt = len(vals)
        entries.append((tag, typ, cnt, b))

    e(254, 4, 0); e(256, 4, w); e(257, 4, h); e(258, 3, 16); e(259, 3, 1); e(262, 3, 32803)   # CFA photometric
    e(273, 4, 0); e(277, 3, 1); e(278, 4, h); e(279, 4, len(data))
    e(33421, 3, [2, 2]); e(33422, 1, [0, 1, 1, 2])                    # CFARepeatPattern, CFAPattern: R G / G B
    e(50706, 1, [1, 4, 0, 0]); e(50708, 2, "IMX519 (Radxa Cubie A7Z)")
    e(50714, 3, [black << 6]); e(50717, 3, [white << 6])
    e(50721, 10, [(1000, 1000), (0, 1000), (0, 1000), (0, 1000), (1000, 1000), (0, 1000), (0, 1000), (0, 1000), (1000, 1000)])
    e(50728, 5, [(int(1000 / x), 1000) for x in as_shot])             # AsShotNeutral
    e(50778, 3, [21])                                                 # CalibrationIlluminant1 = D65
    entries.sort(key=lambda t: t[0])
    n = len(entries)
    ifd_off = 8
    ext_off = ifd_off + 2 + 12 * n + 4
    ext = b""; ifd = struct.pack("<H", n)
    data_off_pos = None
    for tag, typ, cnt, b in entries:
        if len(b) <= 4:
            val = b.ljust(4, b"\0")
        else:
            val = struct.pack("<I", ext_off + len(ext)); ext += b + (b"\0" if len(b) % 2 else b"")
        ifd += struct.pack("<HHI", tag, typ, cnt) + val
    ifd += struct.pack("<I", 0)
    data_off = ext_off + len(ext)
    # patch StripOffsets
    out = bytearray(b"II*\0" + struct.pack("<I", ifd_off) + ifd + ext + data)
    pos = ifd_off + 2
    for i, (tag, *_r) in enumerate(entries):
        if tag == 273:
            struct.pack_into("<I", out, pos + 12 * i + 8, data_off)
    open(path, "wb").write(out)


def orient(rgb, rotate=0, hflip=False, vflip=False):
    """Rotate (clockwise degrees) and mirror a processed picture. Done in software, so the Bayer order never changes."""
    if rotate == 90: rgb = cv2.rotate(rgb, cv2.ROTATE_90_CLOCKWISE)
    elif rotate == 180: rgb = cv2.rotate(rgb, cv2.ROTATE_180)
    elif rotate == 270: rgb = cv2.rotate(rgb, cv2.ROTATE_90_COUNTERCLOCKWISE)
    if hflip: rgb = cv2.flip(rgb, 1)
    if vflip: rgb = cv2.flip(rgb, 0)
    return np.ascontiguousarray(rgb)


# ---------------------------------------------------------------------------------------- commands
def cmd_still(a):
    size = {"1080p": "1080p", "720p": "720p", "4k": "4k", "2k": "2k", "full": "full"}[a.size]
    with Camera(size) as cam:
        if a.focus is not None:
            cam.set_focus(a.focus)
        if a.exposure:
            cam.set_exposure(a.exposure); cam.set_gain(a.gain or 1.0)
        else:
            if a.gain:
                cam.set_gain(a.gain)
            cam.auto_exposure()
        if a.af:
            best = cam.autofocus(); print(f"autofocus: lens position {best[0]} (sharpness {best[1]:.2f})")
            if not a.exposure:
                cam.auto_exposure(iters=4)
        raw = cam.wait_frames(2 if cam.exp < 0.3 else 1)
        wb = cam.awb_gains(raw)
        rgb = orient(cam.to_rgb(raw, full_res=True, wb=wb), a.rotate, a.hflip, a.vflip)
        cv2.imwrite(a.output, cv2.cvtColor(rgb, cv2.COLOR_RGB2BGR), [cv2.IMWRITE_JPEG_QUALITY, 95])
        if a.raw:
            write_dng(a.raw, raw, exp=cam.exp, gain=cam.gain, as_shot=wb.tolist())
        print(f"saved {a.output}  {rgb.shape[1]}x{rgb.shape[0]}  exposure {cam.exp:.4f}s gain {cam.gain:.1f}x focus {cam.pos}" +
              (f"  raw {a.raw}" if a.raw else ""))


def cmd_focus(a):
    with Camera("1080p") as cam:
        if a.set is not None:
            cam.set_focus(a.set); print("lens position", cam.pos)
        else:
            cam.auto_exposure()
            log = []
            best = cam.autofocus(log=log)
            print(f"autofocus done: lens position {best[0]}, sharpness {best[1]:.2f}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("still"); s.add_argument("output")
    s.add_argument("--size", default="full", choices=list(MODES))
    s.add_argument("--exposure", type=float); s.add_argument("--gain", type=float)
    s.add_argument("--focus", type=int); s.add_argument("--af", action="store_true"); s.add_argument("--raw")
    s.add_argument("--rotate", type=int, default=0, choices=[0, 90, 180, 270], help="rotate the picture (clockwise degrees)")
    s.add_argument("--hflip", action="store_true"); s.add_argument("--vflip", action="store_true")
    s.set_defaults(fn=cmd_still)
    f = sub.add_parser("focus"); f.add_argument("--set", type=int); f.add_argument("--af", action="store_true")
    f.set_defaults(fn=cmd_focus)
    l = sub.add_parser("live"); l.add_argument("--port", type=int, default=8080)
    l.set_defaults(fn=lambda a: __import__("liveview").main())
    a = ap.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
