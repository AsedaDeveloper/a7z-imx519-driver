# Usage

## a7zcam (still, raw, focus, live view)
```bash
a7zcam still out.jpg                        # full resolution (4656x3496), auto exposure
a7zcam still out.jpg --size 1080p --af      # autofocus first
a7zcam still out.jpg --exposure 0.5 --gain 4 --focus 2400 --raw out.dng
a7zcam still out.jpg --rotate 90 --hflip    # orientation is done in software
a7zcam still out.jpg --color simple          # plain gray-world white balance; default is `--color tuned --saturation 0.6`
a7zcam video clip.mp4 --seconds 10 --size 1080p --fps 30 [--preset veryfast --crf 23 --rotate 90 --af]
a7zcam focus --af                           # autofocus, prints the lens position (0 = infinity .. 4095 = nearest)
a7zcam focus --set 2000
a7zcam live --port 8080                     # web live view: exposure, gain, focus, AE, AWB, autofocus, continuous AF
```
Sizes: `720p 1080p 2k (2328x1748) 4k (3840x2160) full (4656x3496)`. The camera device can only be used by one program at
a time (`/dev/video0` is exclusive): stop the live view before taking stills.

Environment for the live view: `LIVEVIEW_PORT`, `LIVEVIEW_ROTATE=0|90|180|270`, `LIVEVIEW_FLIP=h|v|hv`.
Run `tools/a7zcam-live.service` as a systemd unit if you want it at boot.

## Standard V4L2
```bash
v4l2-ctl -d /dev/video0 --set-input=0 --list-ctrls
# one capture with manual exposure (value = lines x 16; 1 line = 14.13 us at 1080p) and focus:
v4l2-ctl -d /dev/video0 --set-input=0 --set-ctrl=exposure=$((1500*16)),gain=64,focus_absolute=60 \
         --set-fmt-video=width=1920,height=1080,pixelformat=RG10 --stream-mmap=4 --stream-skip=4 --stream-count=1 --stream-to=frame.raw
```
* `gain` is gain × 16 (16 = 1×, 1024 = 64×); `exposure` is exposure lines × 16; `focus_absolute` 0..127.
* Raw layout: 16-bit little endian words, RGGB, black level 64, white 1023, buffer height padded by 8 rows.
  Only `RG10` / `RG12` stream.
* Live changes while streaming: `v4l2-ctl -d /dev/v4l-subdev0 --set-ctrl=exposure=...` / `gain=...`.
* Lens sub-device: `/dev/v4l-subdev1`.

## Python
```python
import sys; sys.path.insert(0, "/opt/a7zcam")
from a7zcam import Camera
with Camera("1080p") as cam:
    cam.set_exposure(0.1); cam.set_gain(2); cam.set_focus(2300)
    raw = cam.wait_frames(2)               # uint16 mosaic; fully exposed after the change
    rgb = cam.to_rgb(raw, full_res=True)
```

## Long exposures / astro
Use a persistent stream (the `Camera` class above): the first frame of every new stream is lost in the VIN, so a
one-shot 15 s exposure costs ~31 s, but successive frames of a running stream cost 15 s each. Dark current is
about 0.5 ADU/s at 1× gain at room temperature (+7 ADU in 15 s): prefer low gain with long exposure, and subtract
dark frames. Kernel module parameters (`/sys/module/imx519_mipi/parameters/`): `exp_us`, `gain_x16` (applied at every
stream start), `init_delay_ms`, `stream_delay_ms`, `term_res`, `phy_offset`.

## Orientation of the picture
The raw image is not rotated. Mounted with the cable on the left it appears rotated 90°; use `--rotate`.

## Colour
`a7zcam still` converts the raw frame itself: black-level subtraction, demosaic, white balance, then a colour
matrix and tone curve. The white-point curve and colour matrices come from the **Raspberry Pi tuning data for the
IMX519** (`tools/imx519_color.json`, BSD-2-Clause, libcamera project); the colour temperature is estimated from the
scene (gray-world ratios projected onto that curve). The Pi matrices are applied at 60 % strength by default because
the full strength looks over-saturated without the rest of the Pi's pipeline. This is a *reasonable* rendering, not
a calibrated one: there is no lens-shading correction, and the colour was judged by eye, not against a colour chart.
For accurate colour develop the raw DNG in a raw converter with your own profile.

## Video
`a7zcam video` tone-maps the raw frames, lets ffmpeg demosaic and encode them with libx264, and runs auto exposure
and white balance while recording. The Python loop and the software encoder limit it to roughly 17-27 fps at 1080p
(darker scenes are noisier and slower to encode). Every frame is stamped with its arrival time and the output is
constant-rate, so a clip always plays at real speed; frames are repeated when the board falls behind. Use
`--preset ultrafast` for more speed, `--size 720p` for less load. Full-resolution and 4K modes are far too slow to
encode live.
