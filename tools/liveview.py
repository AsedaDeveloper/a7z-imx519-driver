#!/usr/bin/env python3
"""IMX519 live view for the Radxa Cubie A7Z.

Streams the camera as MJPEG on http://<board>:8080/ with exposure/gain, auto exposure, auto white balance,
manual focus and contrast-detect autofocus (the lens motor is an AK7375 on I2C address 0x0c).

Everything is done in user space on top of the raw RAW10 frames from the imx519_mipi VIN driver:
 - v4l2-ctl streams RAW10 (RGGB) frames into a FIFO
 - a reader thread keeps the newest frame
 - a processing thread converts it to RGB (2x2 binning, black level, white balance, gamma) -> JPEG
 - exposure/gain/lens are written straight to the sensor and lens motor over /dev/i2c-3
"""
import os, sys, time, json, math, fcntl, threading, subprocess, socket
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs
import numpy as np, cv2

W, H, SH = 1920, 1080, 1088            # sensor mode 1920x1080, VIN buffer height 1088
FSZ = W * SH * 2
BLACK = 64.0
FULL = 1023.0
LINE_US = 6027 / 426.666667            # 1080p line time (us)
VTS = 1180
I2C_BUS = "/dev/i2c-3"
SENSOR_ADDR, VCM_ADDR = 0x1A, 0x0C
FIFO = "/tmp/livecam.fifo"
PORT = int(os.environ.get("LIVEVIEW_PORT", "8080"))
ROTATE = int(os.environ.get("LIVEVIEW_ROTATE", "0"))     # 0/90/180/270 clockwise
FLIP = os.environ.get("LIVEVIEW_FLIP", "")                 # h, v or hv


class I2C:
    def __init__(self):
        self.fd = os.open(I2C_BUS, os.O_RDWR)
        self.lock = threading.Lock()

    def write(self, addr, data):
        with self.lock:
            fcntl.ioctl(self.fd, 0x0706, addr)          # I2C_SLAVE_FORCE
            os.write(self.fd, bytes(data))


class Sensor:
    """Live exposure / gain writes for the IMX519 (same register logic as the kernel driver)."""
    def __init__(self, i2c):
        self.i2c = i2c

    def w(self, reg, val):
        self.i2c.write(SENSOR_ADDR, [reg >> 8, reg & 255, val])

    def set_exposure(self, sec):
        lines = max(20, round(sec * 1e6 / LINE_US))
        frame = max(lines + 32, VTS)
        shift = 0
        while frame > 65535 and shift < 7:
            shift += 1
            frame >>= 1
        frame = min(frame, 65535)
        coarse = max(1, lines >> shift)
        self.w(0x0104, 1)
        self.w(0x3100, shift)
        self.w(0x0340, frame >> 8); self.w(0x0341, frame & 255)
        self.w(0x0202, coarse >> 8); self.w(0x0203, coarse & 255)
        self.w(0x0104, 0)

    def set_gain(self, g):
        g = min(max(g, 1.0), 64.0)
        if g <= 16.0:
            ana = int(1024 - 1024 / g); dig = 0x0100
        else:
            ana = 960; dig = int(g / 16.0 * 256)
        ana = min(ana, 960); dig = min(dig, 0xFFFF)
        self.w(0x0104, 1)
        self.w(0x0204, ana >> 8); self.w(0x0205, ana & 255)
        self.w(0x020E, dig >> 8); self.w(0x020F, dig & 255)
        self.w(0x0104, 0)


class Lens:
    """AK7375 voice-coil motor. Position 0..4095 (0 = far/infinity, 4095 = nearest).
    The position MUST be written as ONE 3-byte transaction [reg 0x00, high, low]."""
    def __init__(self, i2c):
        self.i2c, self.pos = i2c, 0
        self.i2c.write(VCM_ADDR, [0x02, 0x00])           # active mode

    def set(self, pos):
        pos = int(min(max(pos, 0), 4095))
        self.i2c.write(VCM_ADDR, [0x00, (pos >> 4) & 0xFF, (pos & 0xF) << 4])
        self.pos = pos


class Camera:
    def __init__(self):
        self.i2c = I2C(); self.sensor = Sensor(self.i2c); self.lens = Lens(self.i2c)
        self.lock = threading.Lock()
        self.raw = None; self.seq = 0; self.fps = 0.0
        self.exp, self.gain = 0.02, 2.0
        self.ae = True; self.awb = True
        self.focus = 2400
        self.af_state = "idle"; self.sharp = 0.0; self.af_log = []
        self.caf = False; self.caf_ref = 0.0; self.caf_bad = 0
        self.wb = np.array([1.9, 1.0, 1.7], dtype=np.float32)
        self.stats = {}
        self.stop = False
        self.last_ae_seq = 0
        self.proc = None

    # ----------------------------------------------------------------- camera stream
    def start_stream(self):
        for p in ("exp_us", "gain_x16"):
            open(f"/sys/module/imx519_mipi/parameters/{p}", "w").write("0")
        open("/sys/module/imx519_mipi/parameters/exp_us", "w").write(str(int(self.exp * 1e6)))
        open("/sys/module/imx519_mipi/parameters/gain_x16", "w").write(str(int(self.gain * 16)))
        if os.path.exists(FIFO):
            os.remove(FIFO)
        os.mkfifo(FIFO)
        self.proc = subprocess.Popen(
            ["v4l2-ctl", "-d", "/dev/video0", "--set-input=0",
             f"--set-fmt-video=width={W},height={H},pixelformat=RG10", "--stream-mmap=4",
             "--stream-count=100000000", f"--stream-to={FIFO}"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.lens.set(self.focus)
        threading.Thread(target=self.reader, daemon=True).start()
        threading.Thread(target=self.caf_loop, daemon=True).start()

    def reader(self):
        f = open(FIFO, "rb", buffering=0)
        buf = bytearray(FSZ); mv = memoryview(buf)
        t0, n = time.time(), 0
        while not self.stop:
            got = 0
            while got < FSZ:
                r = f.readinto(mv[got:])
                if not r:
                    time.sleep(0.5); got = -1; break
                got += r
            if got < 0:
                if self.proc.poll() is not None:
                    return
                continue
            a = np.frombuffer(buf, dtype="<u2").reshape(SH, W)[:H]
            with self.lock:
                self.raw = a.copy(); self.seq += 1
            n += 1
            if time.time() - t0 >= 1.0:
                self.fps = n / (time.time() - t0); n = 0; t0 = time.time()

    def latest(self, after_seq=None, timeout=3.0):
        t0 = time.time()
        while time.time() - t0 < timeout:
            with self.lock:
                if self.raw is not None and (after_seq is None or self.seq > after_seq):
                    return self.raw, self.seq
            time.sleep(0.005)
        return None, None

    # ----------------------------------------------------------------- image processing
    @staticmethod
    def planes(a):
        a = a.astype(np.float32) - BLACK
        r = a[0::2, 0::2]; g = (a[0::2, 1::2] + a[1::2, 0::2]) * 0.5; b = a[1::2, 1::2]
        return r, g, b

    def render(self, a):
        r, g, b = self.planes(a)
        if self.awb:                                          # gray-world white balance, smoothed
            mr, mg, mb = max(r.mean(), 1.0), max(g.mean(), 1.0), max(b.mean(), 1.0)
            target = np.array([mg / mr, 1.0, mg / mb], dtype=np.float32)
            self.wb = 0.85 * self.wb + 0.15 * np.clip(target, 0.3, 6.0)
        img = np.dstack([r * self.wb[0], g * self.wb[1], b * self.wb[2]]) / (FULL - BLACK)
        img = np.clip(img, 0, 1) ** (1 / 2.2)
        return (img * 255).astype(np.uint8)

    def sharpness(self, a):
        _, g, _ = self.planes(a)
        c = g[180:360, 260:700]
        m = max(float(c.mean()), 1.0)
        return float(cv2.Laplacian(cv2.GaussianBlur(c, (0, 0), 1.2), cv2.CV_32F).var() / (m * m) * 1000.0)

    # ----------------------------------------------------------------- auto exposure
    def auto_exposure(self, a, seq):
        if not self.ae or seq - self.last_ae_seq < 6:
            return
        _, g, _ = self.planes(a)
        med = float(np.percentile(g[::4, ::4], 60)) / (FULL - BLACK)
        sat = float((g[::4, ::4] > 0.95 * (FULL - BLACK)).mean())
        if sat > 0.02:
            med = max(med, 0.6) * (1 + 4 * sat)
        target = 0.30
        if med <= 0.005:
            ratio = 4.0
        else:
            ratio = (target / med) ** 0.7
        if abs(ratio - 1.0) < 0.12:
            return
        total = min(max(self.exp * self.gain * ratio, 1e-4), 0.5 * 32.0)
        # prefer more exposure up to 1/15 s, then gain up to 8x, then longer exposure (to 0.5 s), then more gain
        e = min(total, 1 / 15); g_ = max(total / e, 1.0)
        if g_ > 8.0:
            e = min(total / 8.0, 0.5); g_ = max(total / e, 1.0)
        self.exp, self.gain = e, min(g_, 32.0)
        self.sensor.set_exposure(self.exp); self.sensor.set_gain(self.gain)
        self.last_ae_seq = seq

    # ----------------------------------------------------------------- autofocus (contrast detect)
    def autofocus(self):
        if self.af_state == "running":
            return
        threading.Thread(target=self._af, daemon=True).start()

    def _measure(self, pos):
        self.lens.set(pos)
        _, s0 = self.latest()
        wait = max(0.25, 4.0 * max(self.exp, 1 / 60.0))
        time.sleep(wait)
        a, s = self.latest(after_seq=s0 + 1 if s0 is not None else None)
        if a is None:
            return 0.0
        vals = []
        for _ in range(2):
            a, s = self.latest(after_seq=s)
            if a is not None:
                vals.append(self.sharpness(a))
        return float(np.mean(vals)) if vals else 0.0

    def _af(self):
        self.af_state = "running"; self.af_log = []
        try:
            was_ae = self.ae
            res = []
            for pos in range(0, 4096, 256):
                res.append((pos, self._measure(pos))); self.af_log = res[:]
            best = max(res, key=lambda r: r[1])[0]
            fine = []
            for pos in range(max(0, best - 320), min(4095, best + 320) + 1, 64):
                fine.append((pos, self._measure(pos)))
            best = max(fine, key=lambda r: r[1])
            self.af_log = res + fine
            self.focus = best[0]; self.lens.set(self.focus)
            self.sharp = best[1]
            self.af_state = "done (position %d, sharpness %.2f)" % best
        except Exception as e:                       # never leave the lens in a random place silently
            self.af_state = f"failed: {e}"
            self.lens.set(self.focus)

    # ----------------------------------------------------------------- continuous autofocus
    def caf_loop(self):
        """Watch the sharpness of the live image; when it falls well below the value after the last focus run
        (for 3 checks in a row) re-focus: a quick local search first, a full sweep if the peak is at its edge."""
        while not self.stop:
            time.sleep(1.0)
            if not self.caf or self.af_state == "running":
                continue
            a, _ = self.latest(timeout=2)
            if a is None:
                continue
            s = self.sharpness(a)
            if self.caf_ref <= 0:
                self.caf_ref = s
                continue
            if s < 0.65 * self.caf_ref:
                self.caf_bad += 1
            else:
                self.caf_bad = 0
                self.caf_ref = 0.9 * self.caf_ref + 0.1 * s          # follow slow lighting changes
            if self.caf_bad >= 3:
                self.caf_bad = 0
                self._af_local()

    def _af_local(self):
        self.af_state = "running"
        try:
            centre = self.lens.pos
            res = [(p, self._measure(p)) for p in range(max(0, centre - 192), min(4095, centre + 192) + 1, 64)]
            self.af_log = res
            best = max(res, key=lambda r: r[1])
            if best[0] in (res[0][0], res[-1][0]) and best[1] < 3 * self.caf_ref:
                self.af_state = "idle"
                return self._af()                                  # peak at the edge: do the full search
            self.focus = best[0]; self.lens.set(self.focus); self.sharp = best[1]; self.caf_ref = best[1]
            self.af_state = "done (continuous: position %d)" % best[0]
        except Exception as e:
            self.af_state = f"failed: {e}"

    # ----------------------------------------------------------------- main processing loop
    def process_loop(self):
        last = 0
        while not self.stop:
            a, seq = self.latest(after_seq=last, timeout=2.0)
            if a is None:
                continue
            last = seq
            if self.af_state != "running":
                self.auto_exposure(a, seq)
            t = time.time()
            if t - getattr(self, "_lastjpg", 0) < 0.06:           # ~15 fps preview
                continue
            self._lastjpg = t
            rgb = self.render(a)
            if ROTATE == 90: rgb = cv2.rotate(rgb, cv2.ROTATE_90_CLOCKWISE)
            elif ROTATE == 180: rgb = cv2.rotate(rgb, cv2.ROTATE_180)
            elif ROTATE == 270: rgb = cv2.rotate(rgb, cv2.ROTATE_90_COUNTERCLOCKWISE)
            if "h" in FLIP: rgb = cv2.flip(rgb, 1)
            if "v" in FLIP: rgb = cv2.flip(rgb, 0)
            bgr = cv2.cvtColor(rgb, cv2.COLOR_RGB2BGR)
            txt = "exp %s  gain %.1fx  focus %d  %.0f fps  AE %s  AF %s" % (
                ("1/%d s" % round(1 / self.exp)) if self.exp < 0.5 else ("%.2f s" % self.exp),
                self.gain, self.lens.pos, self.fps, "on" if self.ae else "off", self.af_state.split(" ")[0])
            cv2.putText(bgr, txt, (10, 24), cv2.FONT_HERSHEY_SIMPLEX, 0.6, (0, 0, 0), 3, cv2.LINE_AA)
            cv2.putText(bgr, txt, (10, 24), cv2.FONT_HERSHEY_SIMPLEX, 0.6, (255, 255, 255), 1, cv2.LINE_AA)
            ok, jpg = cv2.imencode(".jpg", bgr, [cv2.IMWRITE_JPEG_QUALITY, 80])
            if ok:
                with self.lock:
                    self.jpg = jpg.tobytes(); self.jpg_seq = getattr(self, "jpg_seq", 0) + 1

    def status(self):
        return dict(exp=self.exp, gain=self.gain, ae=self.ae, awb=self.awb, focus=self.lens.pos, fps=round(self.fps, 1),
                    af=self.af_state, caf=self.caf, af_log=self.af_log[-40:], wb=[float(x) for x in self.wb])


CAM = Camera()

PAGE = b"""<!doctype html><html><head><meta name=viewport content="width=device-width,initial-scale=1">
<title>IMX519 live view</title>
<style>body{margin:0;background:#111;color:#ddd;font:14px system-ui}#v{width:100%;max-height:75vh;object-fit:contain;background:#000;display:block}
.row{display:flex;gap:12px;align-items:center;padding:6px 12px;flex-wrap:wrap}label{min-width:70px}input[type=range]{flex:1;min-width:160px}
button{padding:6px 12px;border-radius:6px;border:0;background:#2b6cb0;color:#fff}button.on{background:#2f855a}#st{padding:6px 12px;color:#9ae6b4}</style></head><body>
<img id=v src="/stream.mjpg">
<div class=row><button id=ae>Auto exposure</button><button id=awb>Auto white balance</button><button id=af>Autofocus</button><button id=caf>Continuous AF</button></div>
<div class=row><label>Exposure</label><input id=exp type=range min=0 max=100 step=1><span id=expv></span></div>
<div class=row><label>Gain</label><input id=gain type=range min=0 max=100 step=1><span id=gainv></span></div>
<div class=row><label>Focus</label><input id=focus type=range min=0 max=4095 step=16><span id=focv></span></div>
<div id=st></div>
<script>
const $=id=>document.getElementById(id);let busy=0;
const e2s=v=>0.0001*Math.pow(0.5/0.0001,v/100), s2e=s=>100*Math.log(s/0.0001)/Math.log(0.5/0.0001);
const g2s=v=>Math.pow(32,v/100), s2g=g=>100*Math.log(g)/Math.log(32);
function api(q){return fetch('/api/set?'+q).then(r=>r.json())}
$('ae').onclick=()=>api('ae='+(window.s&&window.s.ae?0:1));$('awb').onclick=()=>api('awb='+(window.s&&window.s.awb?0:1));
$('af').onclick=()=>fetch('/api/af').then(r=>r.json());
$('caf').onclick=()=>api('caf='+(window.s&&window.s.caf?0:1));
$('exp').oninput=e=>{busy=Date.now();api('ae=0&exp='+e2s(+e.target.value))};
$('gain').oninput=e=>{busy=Date.now();api('ae=0&gain='+g2s(+e.target.value))};
$('focus').oninput=e=>{busy=Date.now();api('focus='+e.target.value)};
function fmt(s){return s<0.5?('1/'+Math.round(1/s)+' s'):(s.toFixed(2)+' s')}
setInterval(()=>fetch('/api/status').then(r=>r.json()).then(s=>{window.s=s;$('ae').className=s.ae?'on':'';$('awb').className=s.awb?'on':'';$('caf').className=s.caf?'on':'';
 if(Date.now()-busy>1500){$('exp').value=s2e(s.exp);$('gain').value=s2g(s.gain);$('focus').value=s.focus}
 $('expv').textContent=fmt(s.exp);$('gainv').textContent=s.gain.toFixed(1)+'x';$('focv').textContent=s.focus;
 $('st').textContent='fps '+s.fps+'   autofocus: '+s.af}),700);
</script></body></html>"""


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a): pass

    def _json(self, obj):
        b = json.dumps(obj).encode()
        self.send_response(200); self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)

    def do_GET(self):
        u = urlparse(self.path); q = {k: v[0] for k, v in parse_qs(u.query).items()}
        if u.path == "/":
            self.send_response(200); self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(PAGE))); self.end_headers(); self.wfile.write(PAGE)
        elif u.path == "/stream.mjpg":
            self.send_response(200)
            self.send_header("Content-Type", "multipart/x-mixed-replace; boundary=frame"); self.end_headers()
            last = -1
            try:
                while True:
                    with CAM.lock:
                        j, s = getattr(CAM, "jpg", None), getattr(CAM, "jpg_seq", -1)
                    if j is None or s == last:
                        time.sleep(0.02); continue
                    last = s
                    self.wfile.write(b"--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n\r\n" % len(j))
                    self.wfile.write(j); self.wfile.write(b"\r\n")
            except (BrokenPipeError, ConnectionResetError):
                return
        elif u.path == "/snapshot.jpg":
            with CAM.lock: j = getattr(CAM, "jpg", b"")
            self.send_response(200); self.send_header("Content-Type", "image/jpeg")
            self.send_header("Content-Length", str(len(j))); self.end_headers(); self.wfile.write(j)
        elif u.path == "/api/status":
            self._json(CAM.status())
        elif u.path == "/api/af":
            CAM.autofocus(); self._json({"started": True})
        elif u.path == "/api/set":
            if "ae" in q: CAM.ae = q["ae"] == "1"
            if "awb" in q: CAM.awb = q["awb"] == "1"
            if "caf" in q:
                CAM.caf = q["caf"] == "1"; CAM.caf_ref = 0.0; CAM.caf_bad = 0
            if "exp" in q:
                CAM.exp = min(max(float(q["exp"]), 1e-4), 15.0); CAM.sensor.set_exposure(CAM.exp)
            if "gain" in q:
                CAM.gain = min(max(float(q["gain"]), 1.0), 64.0); CAM.sensor.set_gain(CAM.gain)
            if "focus" in q:
                CAM.focus = int(float(q["focus"])); CAM.lens.set(CAM.focus)
            self._json(CAM.status())
        else:
            self.send_error(404)


def main():
    CAM.start_stream()
    CAM.sensor.set_exposure(CAM.exp); CAM.sensor.set_gain(CAM.gain)
    threading.Thread(target=CAM.process_loop, daemon=True).start()
    srv = ThreadingHTTPServer(("0.0.0.0", PORT), Handler)
    ips = subprocess.run(["hostname", "-I"], capture_output=True, text=True).stdout.split()
    print("live view on port", PORT, "addresses:", ips, flush=True)
    try:
        srv.serve_forever()
    finally:
        CAM.stop = True
        if CAM.proc: CAM.proc.terminate()
        for p in ("exp_us", "gain_x16"):
            try: open(f"/sys/module/imx519_mipi/parameters/{p}", "w").write("0")
            except OSError: pass


if __name__ == "__main__":
    main()
