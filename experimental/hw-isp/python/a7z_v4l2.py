"""Read the A7Z/IMX519 camera from Python with nothing installed — no OpenCV, no GStreamer, no bindings.

The camera is published as an ordinary V4L2 device by `sudo v4l2node start` (see experimental/hw-isp/V4L2_NODE.md), so this is just the standard
capture sequence — QUERYCAP, S_FMT, REQBUFS, QUERYBUF+mmap, QBUF, STREAMON, DQBUF — spoken directly to the kernel with ioctl and mmap.
Frames arrive already through the hardware ISP: autofocused, white-balanced, exposure-capped.

    from a7z_v4l2 import Camera
    with Camera() as cam:
        for y, u, v in cam.frames(10):
            print(len(y))               # Y plane, width*height bytes

numpy is optional: Camera.rgb() uses it when present.

Per-frame metadata (what the sensor and lens were doing for each frame) comes from the feeder's ring file, matched by timestamp:

    with Camera() as cam:
        y, u, v, meta = cam.read_with_meta()
        print(meta["exp_us"], meta["gain"] / 16, meta["lens_dpt"])
"""
import ctypes
import fcntl
import mmap
import os
import select
import struct

# ---- ioctl plumbing -------------------------------------------------------------------------------------------------
_IOC_WRITE, _IOC_READ = 1, 2


def _IOC(d, t, nr, size):
    return (d << 30) | (size << 16) | (ord(t) << 8) | nr


def _IOR(t, nr, size):
    return _IOC(_IOC_READ, t, nr, size)


def _IOW(t, nr, size):
    return _IOC(_IOC_WRITE, t, nr, size)


def _IOWR(t, nr, size):
    return _IOC(_IOC_READ | _IOC_WRITE, t, nr, size)


# struct sizes on 64-bit Linux
SZ_CAP, SZ_FMT, SZ_REQBUF, SZ_BUF = 104, 208, 20, 88

VIDIOC_QUERYCAP = _IOR("V", 0, SZ_CAP)
VIDIOC_S_FMT = _IOWR("V", 5, SZ_FMT)
VIDIOC_G_FMT = _IOWR("V", 4, SZ_FMT)
VIDIOC_REQBUFS = _IOWR("V", 8, SZ_REQBUF)
VIDIOC_QUERYBUF = _IOWR("V", 9, SZ_BUF)
VIDIOC_QBUF = _IOWR("V", 15, SZ_BUF)
VIDIOC_DQBUF = _IOWR("V", 17, SZ_BUF)
VIDIOC_STREAMON = _IOW("V", 18, 4)
VIDIOC_STREAMOFF = _IOW("V", 19, 4)

BUF_TYPE_CAPTURE = 1
MEMORY_MMAP = 1


def fourcc(s):
    return s[0] | (s[1] << 8) | (s[2] << 16) | (s[3] << 24)


PIX_YUV420 = fourcc(b"YU12")      # planar Y, then U, then V — what the node serves


METARING = os.environ.get("A7Z_METARING", "/run/a7zcam.meta")
_MR_HDR, _MR_REC = 32, 40


class MetaRing:
    """Reader for the feeder's per-frame metadata ring (A7Z_METARING). Records: ts_ns (CLOCK_MONOTONIC), seq, exp_us, gain (16 = 1x), lens_code,
    lens_dpt, r_gain, b_gain, flags (bit0 autofocus sweeping, bit1 lens held)."""

    def __init__(self, path=METARING):
        self.f = open(path, "rb")
        self.m = mmap.mmap(self.f.fileno(), 0, access=mmap.ACCESS_READ)
        magic, ver, self.slots, self.rec = struct.unpack_from("<IIII", self.m, 0)
        if magic != 0x4d5a3741:
            raise RuntimeError("%s is not an A7Z metadata ring" % path)

    def _rec(self, n):
        o = _MR_HDR + (n % self.slots) * self.rec
        ts, seq, exp, gain, code, r, b, flags, dpt = struct.unpack_from("<QIIIIIIIf", self.m, o)
        return {"ts_ns": ts, "seq": seq, "exp_us": exp, "gain": gain, "lens_code": code, "lens_dpt": round(dpt, 3), "r_gain": r, "b_gain": b, "flags": flags}

    def head(self):
        return struct.unpack_from("<Q", self.m, 16)[0]

    def nearest(self, ts_ns, tol_ns=16_000_000):
        """The record whose timestamp is closest to ts_ns (a frame's V4L2 buffer time), or None if nothing is within tol_ns."""
        h = self.head()
        best = None
        for n in range(h - 1, max(-1, h - 1 - 90), -1):        # last ~3 s
            r = self._rec(n)
            d = abs(r["ts_ns"] - ts_ns)
            if best is None or d < best[0]:
                best = (d, r)
            elif r["ts_ns"] < ts_ns - tol_ns:
                break
        return best[1] if best and best[0] <= tol_ns else None

    def close(self):
        self.m.close()
        self.f.close()


class Camera:
    def __init__(self, device="/dev/video9", width=1920, height=1080, nbuffers=4):
        self.device, self.width, self.height, self.nbuffers = device, width, height, nbuffers
        self.fd = None
        self.buffers = []           # (mmap, length)
        self.streaming = False

    # -- lifecycle ----------------------------------------------------------------------------------------------------
    def open(self):
        self.fd = os.open(self.device, os.O_RDWR)
        cap = bytearray(SZ_CAP)
        fcntl.ioctl(self.fd, VIDIOC_QUERYCAP, cap, True)
        self.driver = bytes(cap[0:16]).split(b"\0")[0].decode(errors="replace")
        self.card = bytes(cap[16:48]).split(b"\0")[0].decode(errors="replace")

        fmt = bytearray(SZ_FMT)
        struct.pack_into("<I", fmt, 0, BUF_TYPE_CAPTURE)
        # struct v4l2_pix_format starts at offset 8 (type, then 4 bytes of padding for 8-byte union alignment)
        struct.pack_into("<IIIIII", fmt, 8, self.width, self.height, PIX_YUV420, 1, self.width,
                         self.width * self.height * 3 // 2)
        fcntl.ioctl(self.fd, VIDIOC_S_FMT, fmt, True)
        self.width, self.height, pf, _, _, self.sizeimage = struct.unpack_from("<IIIIII", fmt, 8)
        if pf != PIX_YUV420:
            raise RuntimeError("device did not accept YU12 (got %r)" % pf.to_bytes(4, "little"))

        rb = bytearray(SZ_REQBUF)
        struct.pack_into("<III", rb, 0, self.nbuffers, BUF_TYPE_CAPTURE, MEMORY_MMAP)
        fcntl.ioctl(self.fd, VIDIOC_REQBUFS, rb, True)
        count = struct.unpack_from("<I", rb, 0)[0]
        if count < 2:
            raise RuntimeError("driver gave only %d buffers" % count)

        for i in range(count):
            b = bytearray(SZ_BUF)
            struct.pack_into("<II", b, 0, i, BUF_TYPE_CAPTURE)
            struct.pack_into("<I", b, 60, MEMORY_MMAP)
            fcntl.ioctl(self.fd, VIDIOC_QUERYBUF, b, True)
            offset = struct.unpack_from("<I", b, 64)[0]      # union m.offset
            length = struct.unpack_from("<I", b, 72)[0]
            self.buffers.append((mmap.mmap(self.fd, length, mmap.MAP_SHARED,
                                           mmap.PROT_READ | mmap.PROT_WRITE, offset=offset), length))
            self._qbuf(i)

        fcntl.ioctl(self.fd, VIDIOC_STREAMON, struct.pack("<I", BUF_TYPE_CAPTURE))
        self.streaming = True
        return self

    def close(self):
        if self.fd is None:
            return
        if self.streaming:
            try:
                fcntl.ioctl(self.fd, VIDIOC_STREAMOFF, struct.pack("<I", BUF_TYPE_CAPTURE))
            except OSError:
                pass
            self.streaming = False
        for m, _ in self.buffers:
            m.close()
        self.buffers = []
        os.close(self.fd)
        self.fd = None

    def __enter__(self):
        return self.open()

    def __exit__(self, *exc):
        self.close()

    # -- capture ------------------------------------------------------------------------------------------------------
    def _qbuf(self, index):
        b = bytearray(SZ_BUF)
        struct.pack_into("<II", b, 0, index, BUF_TYPE_CAPTURE)
        struct.pack_into("<I", b, 60, MEMORY_MMAP)
        fcntl.ioctl(self.fd, VIDIOC_QBUF, b, True)

    def read(self, timeout=3.0):
        """One frame as (Y, U, V) bytes. Copies out of the mapped buffer, so the data stays valid after requeueing."""
        if not select.select([self.fd], [], [], timeout)[0]:
            raise TimeoutError("no frame within %.1f s" % timeout)
        b = bytearray(SZ_BUF)
        struct.pack_into("<I", b, 4, BUF_TYPE_CAPTURE)
        struct.pack_into("<I", b, 60, MEMORY_MMAP)
        fcntl.ioctl(self.fd, VIDIOC_DQBUF, b, True)
        index, used = struct.unpack_from("<I", b, 0)[0], struct.unpack_from("<I", b, 8)[0]
        sec, usec = struct.unpack_from("<qq", b, 24)              # v4l2_buffer.timestamp: CLOCK_MONOTONIC, stamped by the loopback when the feeder wrote the frame
        self.last_ts_ns = sec * 1_000_000_000 + usec * 1000
        self.last_seq = struct.unpack_from("<I", b, 56)[0]
        m, length = self.buffers[index]
        n = used or length
        ysz = self.width * self.height
        csz = ysz // 4
        data = m[:n]
        self._qbuf(index)
        return data[:ysz], data[ysz:ysz + csz], data[ysz + csz:ysz + 2 * csz]

    def read_with_meta(self, timeout=3.0):
        """(Y, U, V, meta): the frame plus its metadata record, or meta=None if the ring is missing or no record matches."""
        y, u, v = self.read(timeout)
        meta = None
        try:
            if getattr(self, "_ring", None) is None:
                self._ring = MetaRing()
            meta = self._ring.nearest(self.last_ts_ns)
        except (OSError, RuntimeError, ValueError):
            self._ring = None
        return y, u, v, meta

    def frames(self, count=None):
        n = 0
        while count is None or n < count:
            yield self.read()
            n += 1

    def rgb(self):
        """One frame as an (h, w, 3) uint8 numpy array, full-range BT.601. Requires numpy."""
        import numpy as np
        y, u, v = self.read()
        h, w = self.height, self.width
        Y = np.frombuffer(y, np.uint8).reshape(h, w).astype(np.float32)
        U = np.frombuffer(u, np.uint8).reshape(h // 2, w // 2).astype(np.float32) - 128
        V = np.frombuffer(v, np.uint8).reshape(h // 2, w // 2).astype(np.float32) - 128
        U = U.repeat(2, 0).repeat(2, 1)
        V = V.repeat(2, 0).repeat(2, 1)
        return np.clip(np.stack([Y + 1.402 * V, Y - 0.344136 * U - 0.714136 * V, Y + 1.772 * U], -1), 0, 255).astype(np.uint8)


if __name__ == "__main__":
    import sys
    import time
    dev = sys.argv[1] if len(sys.argv) > 1 else "/dev/video9"
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 30
    with Camera(dev) as cam:
        print("%s: %s / %s, %dx%d" % (dev, cam.driver, cam.card, cam.width, cam.height))
        t0 = time.monotonic()
        last = None
        for i, (y, u, v) in enumerate(cam.frames(n)):
            last = (y, u, v)
            if i == 0:
                print("first frame: Y %d  U %d  V %d bytes" % (len(y), len(u), len(v)))
        dt = time.monotonic() - t0
        print("%d frames in %.2f s = %.1f fps" % (n, dt, n / dt))
        with open("/tmp/py_frame.pgm", "wb") as f:      # grey preview anything can open, no libraries involved
            f.write(b"P5\n%d %d\n255\n" % (cam.width, cam.height))
            f.write(last[0])
        print("wrote /tmp/py_frame.pgm")
