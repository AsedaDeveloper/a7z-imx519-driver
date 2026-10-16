# libcamera / Raspberry Pi application compatibility

**Status: in progress.** libcamera is not usable with this camera yet. What exists today:

| Raspberry Pi thing | Equivalent here |
|---|---|
| `rpicam-still`, `rpicam-vid`, `rpicam-hello` (and the old `libcamera-*` names) | `rpicam.py` translates the common options onto `a7zcam` (installed as `/usr/local/bin/rpicam-still` etc.). Not libcamera. |
| Picamera2 (Python) | not available; use `a7zcam.Camera` (see USAGE.md) |
| Hardware H.264 encoder | works, see USAGE.md ("Video") |
| A normal V4L2 webcam for browsers / ffmpeg / OpenCV / Frigate | works: `a7zcam webcam` |
| libcamera (`cam`, GStreamer `libcamerasrc`, PipeWire, Picamera2) | **not working; plan below** |

## Why libcamera finds no camera
`cam -l` (libcamera 0.4 from Debian) lists nothing. Pipeline handlers are compiled into libcamera, and none of them knows
the Allwinner `sunxi-vin` media device. A new handler is required. Findings that decide whether that is practical
(all measured on the A7Z):

* `/dev/video0` is a multi-planar capture node. `S_FMT` (SRGGB10), `REQBUFS` (MMAP **and** DMABUF) and `EXPBUF` all work: good,
  that is exactly what libcamera needs for its buffers.
* The sensor sub-device (`/dev/v4l-subdev0`) supports `ENUM_MBUS_CODE` (SRGGB10_1X10), `ENUM_FRAME_SIZE` (the four sensor modes) and `G_FMT`.
* The rest of the VIN chain (mipi, csi, scaler, `vin_cap` sub-devices) is configured by the driver itself when `/dev/video0`
  is set up; the media graph has odd duplicated links, so libcamera's generic "simple" handler (which walks the graph and sets
  a format on every sub-device) is unlikely to work unchanged.
* libcamera's `CameraSensor` requires the standard controls `PIXEL_RATE`, `HBLANK`, `VBLANK`, `EXPOSURE` (in lines) and
  `ANALOGUE_GAIN` (register code). The driver today has `exposure` (lines x 16, BSP convention), `gain` (x16) and `frame_rate` only.
* No ISP: libcamera's software ISP (CPU, Bayer to RGB with simple auto exposure / white balance) would do the processing.

## Plan
1. Driver: add the standard sensor controls (done when this section says so).
2. Build libcamera from source into `/opt/libcamera-a7z` (does not touch the Debian packages).
3. Add a small `sunxi-vin` pipeline handler (video node + sensor only, software ISP) and test with `cam`.
4. Test Picamera2 / GStreamer `libcamerasrc` and document.
Each step is verified on the board before the next starts; this page is updated with what actually works.
