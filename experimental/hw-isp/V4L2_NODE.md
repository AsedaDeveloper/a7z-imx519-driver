# A standard V4L2 device for the IMX519 (checklist B1)

`sudo v4l2node start` publishes the ISP's output — autofocused, white-balanced, exposure-capped — on `/dev/video9`, where any ordinary V4L2 application can open it.
`v4l2node stop` / `v4l2node status` do the obvious things. The script is `tools/v4l2node.sh`.

```
$ sudo v4l2node start
/dev/video9 serving 1920x1080 YU12 at 30 fps (ISP, autofocus, exposure cap)
```

## Why it is a loopback and not just /dev/video0
The obvious design is a daemon that runs libisp while applications open the camera node directly. **It does not work.** `userspace/ispd.c` does exactly that —
sets the sensor mode and format, calls `ispStart`, then idles — and `userspace/v4l2cap.c` is a plain V4L2 client issuing only standard calls, no vendor ioctls,
the same sequence ffmpeg or OpenCV would use. With the daemon running, that client gets:

```
S_FMT:    Device or resource busy
STREAMON: Operation not permitted
```

The vendor driver ties ISP operation to whichever process owns the stream, so libisp and the capture loop must live in the same process. A standard node
therefore has to be *fed*, not shared: `a7zcap` owns `/dev/video0` plus the ISP and republishes every frame into a v4l2loopback device (`A7Z_LOOPBACK=/dev/videoN`).
Frames are published from the autofocus loop too, so a refocus does not interrupt the node.

## Verified
A plain V4L2 client (`userspace/v4l2cap.c`, no vendor ioctls, no libisp) read **20 of 20 frames at 1920x1080** from `/dev/video9`, 3110400 bytes each, exactly
`1920*1080*3/2`. The decoded frame is a correct ISP picture: in focus, correctly white-balanced, full-range YUV.

The node presents itself as **single-planar YU12**, which matters: `/dev/video0` is multi-planar (`V4L2_CAP_VIDEO_CAPTURE_MPLANE`), and ffmpeg's and OpenCV's V4L2
backends only handle single-planar. The loopback removes that obstacle as well as the ISP one.

## Build and install (done on this board)
```sh
tar xzf v4l2loopback-0.12.7.tar.gz
make -C /lib/modules/$(uname -r)/build M=$PWD modules     # kernel headers are installed; builds clean
sudo cp v4l2loopback.ko /lib/modules/$(uname -r)/extra/ && sudo depmod -a
```
The module is third-party (github.com/umlaeute/v4l2loopback, v0.12.7) and is not vendored here.

## Python with nothing installed
`python/a7z_v4l2.py` speaks V4L2 straight to the kernel through `ioctl` and `mmap` — no OpenCV, no GStreamer, no bindings, standard library only:

```python
from a7z_v4l2 import Camera
with Camera() as cam:                 # /dev/video9, 1920x1080
    for y, u, v in cam.frames(10):
        ...                           # Y is width*height bytes, already through the ISP
```
`cam.rgb()` returns an (h, w, 3) array when numpy is present. Measured on the board: **30 frames in 1.07 s = 28.1 fps** at 1080p, and the saved frame is a
correct, in-focus picture.

## Standard applications (checklist B19) — all four verified

| tool | command | result |
|---|---|---|
| `v4l2-ctl` | `--stream-mmap=3 --stream-count=20 --stream-to=...` | 62,208,000 bytes = 20 x 1920x1080 YU12 |
| ffmpeg | `-f v4l2 -video_size 1920x1080 -i /dev/video9` | h264 1920x1080, 20 frames |
| GStreamer | `v4l2src device=/dev/video9 ! video/x-raw,format=I420 ! jpegenc ! multifilesink` | 20 JPEGs, 1920x1080 |
| OpenCV | `cv2.VideoCapture(9)` | 20/20 frames, shape (1080, 1920, 3) |
| Python, no deps | `python/a7z_v4l2.py` | 28.1 fps at 1080p |

### Two things had to be fixed on the board first
* **Debian 11 is end-of-life.** `/etc/apt/sources.list.d/50-bullseye-security.list` still pointed at `deb.debian.org/debian-security`, whose pool is gone, so every
  install 404'd. Repointed at `archive.debian.org/debian-security` (original kept as `.bak`, plus `Acquire::Check-Valid-Until "false"`).
* **Radxa's GStreamer build does not work with standard V4L2 devices.** `gstreamer1.0-plugins-good` 1.18.4-2+radxadeb11u2 patches `v4l2src` to issue the
  sunxi-specific `VIDIOC_SET_DMA_MERGE` on open and to fail when it is not supported:
  `v4l2_calls.c:579:gst_v4l2_open: VIDIOC_SET_DMA_MERGE error!` → "Failed to set pipeline to PAUSED". Installing Debian's stock
  1.18.4-2+deb11u4 fixes it. To go back: `apt-get install --allow-downgrades gstreamer1.0-plugins-good=1.18.4-2+radxadeb11u2`.
  A stale vendor package, `gstreamer1.0-plugins-rtp` 1.14.4.1, also breaks the plugin registry for the 1.18 stack
  (`cannot register existing type 'GstRtpSrc'`); its `libgstnrtp.so` is moved aside to `/root/libgstnrtp.so.disabled`.

The board has no network of its own. For these installs it was given temporary internet over the existing USB link with a reverse SOCKS proxy
(`ssh -p 2222 -N -R 1080 radxa@127.0.0.1`, apt pointed at `socks5h://127.0.0.1:1080` in `/etc/apt/apt.conf.d/99socks`). It dies with the SSH session.
