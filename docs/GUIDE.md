# The Camera Guide

A front-to-back manual for the IMX519 camera on the Radxa Cubie A7Z: what it is, how to take pictures and video,
how to control it live from a browser, how to program against it in Python, and how to get the best image quality
out of it. If you only read one document in this repository, read this one. For install steps see
[INSTALL.md](INSTALL.md); for hardware wiring see [HARDWARE.md](HARDWARE.md); for why things are built the way they
are (useful if you're porting this to another sensor) see [HOW_IT_WORKS.md](HOW_IT_WORKS.md).

---

## 1. What you actually have

A Raspberry Pi camera module (Sony IMX519, 16 MP, autofocus) plugged into a Radxa Cubie A7Z board. The Pi has a
dedicated camera chip (an ISP) that does all the hard work — demosaicing, white balance, autofocus — in hardware.
The A7Z's chip (Allwinner A733) has no equivalent that's usable without Allwinner's closed-source libraries, so
**everything the Pi's ISP would do, this project does in software**, on the CPU, in the tools in this repo. That's
the one fact that explains almost everything else in this guide: why stills take a moment to process, why video
tops out around 30 fps instead of 60, and why autofocus is a deliberate contrast-detect sweep rather than instant
hardware magic.

What you get in exchange: full manual control over exposure (down to microseconds, up to ~118 seconds), gain,
and focus; raw DNG files you can develop properly; a live view you can drive from any browser on the network; and
a command-line tool (`a7zcam`) that mirrors the feel of the Raspberry Pi's own `rpicam-still` / `rpicam-vid`.

## 2. The one rule

**Only one program can hold the camera at a time.** `/dev/video0` is exclusive — if `a7zcam live` is running, a
`a7zcam still` call will fail with "no frames from the camera" until you stop the live view (Ctrl-C it, or kill it).
This trips people up more than anything else in this guide. If something that worked a minute ago suddenly can't see
the camera, this is almost always why.

```bash
pgrep -af "a7zcam|liveview"       # see what's holding it
kill <pid>                        # or Ctrl-C the terminal it's running in
```

## 3. Taking a photo

```bash
a7zcam still photo.jpg
```
That's it — full resolution (4656×3496), automatic exposure. A few useful variations:

```bash
a7zcam still photo.jpg --size 1080p --af              # autofocus first, then shoot
a7zcam still photo.jpg --exposure 0.5 --gain 4 --focus 2400   # fully manual
a7zcam still photo.jpg --raw photo.dng                # also save the raw sensor data
a7zcam still photo.jpg --rotate 90 --hflip             # the module isn't mounted "up" by default
```

Sizes: `720p`, `1080p`, `2k` (2328×1748), `4k` (3840×2160), `full` (4656×3496). Smaller sizes read out faster and
focus/expose faster too, since there's less data to process per frame.

**Focus** is a lens position, not a distance: `0` = infinity, `4095` = closest. `--af` runs a contrast-detect sweep
(see §7) and picks the sharpest position automatically; `--focus N` sets it directly if you already know where you
want it.

## 4. Recording video

```bash
a7zcam video clip.mp4 --seconds 10 --size 1080p --fps 30
```
Measured real-world performance on the A7Z: **1080p ~30 fps**, 720p ~30 fps, 4K ~17 fps (which is the sensor's own
ceiling at that size — not a software bottleneck). It uses the Allwinner hardware H.264 encoder (Cedar VE) when
available, which is why it keeps up; without it, software encoding manages about 17 fps at 1080p, which is why the
installer sets up the hardware path for you (`gstreamer1.0-omx-allwinner` — see [USAGE.md](USAGE.md) if you need to
install it manually).

```bash
a7zcam video clip.mp4 --seconds 10 --af          # autofocus once at the start
a7zcam video clip.mp4 --encoder sw --crf 23      # force software encoding
```

If a clip looks choppy or shorter than requested: the board kept the *duration* right by repeating the last frame
when it fell behind, rather than speeding the clip up. That means a laggy clip is a real performance ceiling being
hit, not a bug losing time.

## 5. Live view — the real control panel

```bash
a7zcam live
```
Open `http://<board-ip>:8080` in any browser on the same network. This is the most direct way to actually *feel*
the camera: sliders for exposure and gain, auto-exposure and auto-white-balance toggles, a focus slider, a one-shot
autofocus button, and a continuous-autofocus toggle that keeps refocusing as the scene changes. It streams MJPEG, so
it works in any browser without plugins, but it is **not** the fastest way to view the feed (see §6.1) — it's the
way to *control* the camera, not the way to watch a fast-moving scene.

Everything the page controls is also available as an HTTP API (`/api/status`, `/api/set?...`, `/api/af`), so you can
script against it instead of clicking:
```bash
curl "http://<board-ip>:8080/api/set?exposure=0.02&gain=2"
curl "http://<board-ip>:8080/api/af"              # trigger one autofocus sweep
curl "http://<board-ip>:8080/api/status"          # current exposure/gain/focus/fps/af state as JSON
```

### 6. Watching the feed natively (not in a browser)

`a7zcam` doesn't currently have a built-in "stream to stdout" mode — `a7zcam video` always records a fixed-length
clip to a file. But the same hardware-encoded H.264 path it uses can be driven directly from a short script built
on the same `Camera` class (see §8) to stream continuously instead of writing a file, and piped over `adb` straight
into `ffplay` on your own computer — the same feeling as the classic `raspivid | ffplay` trick on a Pi:
```bash
adb exec-out "python3 your_stream_script.py --size 4k --fps 30" | ffplay -fflags nobuffer -flags low_delay -f h264 -i -
```
(`4k`, i.e. 3840×2160, is the real ceiling for this path — the hardware encoder rejects the full 4656×3496 sensor
resolution outright with a negotiation error, so 4K is the highest resolution it supports.) This bypasses the MJPEG
live view entirely and gets you the actual H.264 stream the hardware encoder produces, at native frame rate, with
much lower latency than a browser's MJPEG `<img>` tag — worth adding as a real `a7zcam` subcommand if you end up
wanting this often enough.

One gotcha worth knowing if you build this yourself: killing the script with `kill -9` (rather than letting it exit
cleanly, or using plain `kill`/Ctrl-C) can leave the `v4l2-ctl` subprocess it spawned internally still holding
`/dev/video0` open even though the parent process is gone. If the camera then reports "busy" for no visible reason,
`fuser /dev/video0` will show that orphaned PID — kill it directly (see §11).

## 7. Autofocus, honestly explained

There's no phase-detect autofocus hardware feeding this — no second sensor, no split-pixel data, nothing telling
the lens which way to move. All focus decisions come from **contrast-detect**: move the lens, measure how sharp the
image looks (a single number — higher means more contrast at edges, which roughly means better focus), move again,
repeat, and settle on whichever position scored highest.

The implementation here deliberately sweeps the *entire* lens range (0 to 4095 in coarse steps, then a fine sweep
around the best coarse position) every time — about 27 measurements, each one with a short settle delay to let
exposure stabilize. That's roughly 27 small moves of the lens motor per autofocus call. It is not fast, and it will
never feel like a phone's autofocus. But it's exhaustive, which means it reliably finds the real best focus instead
of guessing — an earlier attempt at a smarter, faster search (jump from the current position instead of sweeping
everything) saved real time but turned out to settle on different, sometimes-wrong positions depending on where the
lens happened to already be sitting. That version was reverted; what's in the repo now is the slower, exhaustive,
*consistent* sweep.

Practical tips:
- Autofocus needs a scene with real detail and light. Point it at a blank wall or a dark room and it has nothing to
  measure against, and will settle somewhere arbitrary.
- Continuous AF (in the live view) re-triggers a full sweep whenever the scene's sharpness drops — so moving the
  camera, or something moving through frame, can set off a new sweep. That's expected, not a bug.
- If you already know roughly where focus should sit (e.g., a fixed rig, a known subject distance), setting
  `--focus N` directly and skipping `--af` is both faster and just as accurate.

## 8. Programming against it directly (Python)

The command-line tool is a thin wrapper around a `Camera` class you can use yourself:
```python
import sys; sys.path.insert(0, "/opt/a7zcam")
from a7zcam import Camera

with Camera("1080p") as cam:
    cam.set_exposure(0.1)      # seconds
    cam.set_gain(2)            # 1x .. 64x
    cam.set_focus(2300)        # 0 (infinity) .. 4095 (closest)
    raw = cam.wait_frames(2)   # uint16 Bayer mosaic — wait 2 frames so the new settings are actually applied
    rgb = cam.to_rgb(raw, full_res=True)   # demosaic + white balance + colour matrix -> displayable RGB
```
A couple of things worth knowing before you build on this:
- **Exposure and gain changes take about 2 frames to actually land** on the sensor's output — this is a real sensor
  property, not a driver delay. `wait_frames(2)` after changing a setting is the idiom used throughout this
  codebase for exactly that reason.
- **The first frame of a new stream is always thrown away** (an Allwinner VIN property, not something the sensor
  does) — so a single `with Camera(...)` block that grabs one frame and exits pays that cost every time. For
  anything repeated — video, continuous capture, long exposures — keep one `Camera` open and read many frames from
  it rather than opening and closing it per shot.
- `cam.autofocus()` runs the full sweep described in §7 and returns `(lens_position, sharpness)`.
- `cam.sharpness(frame)` is the raw contrast metric, if you want to build your own focus logic.

For below-the-Python-layer access, standard V4L2 tools work directly against `/dev/video0` and the sensor's own
sub-device `/dev/v4l-subdev0` (see [USAGE.md](USAGE.md) for the exact `v4l2-ctl` incantations and the raw buffer
layout) — useful if you're integrating with something that already speaks V4L2, like OpenCV's `cv2.VideoCapture`,
GStreamer, or Frigate.

If you want the camera to look like an ordinary webcam to software that doesn't know about raw Bayer sensors at
all:
```bash
a7zcam webcam --size 1080p --fps 30
ffmpeg -f v4l2 -i /dev/video20 -t 5 out.mp4    # or cv2.VideoCapture("/dev/video20")
```

## 9. Getting better image quality (optional calibration)

Out of the box, colour comes from the Raspberry Pi's own published IMX519 colour-tuning data (same sensor, so the
numbers transfer), applied at reduced strength because the Pi's full pipeline does more correction around it than
this software path does. It's a reasonable rendering, not a calibrated one. Two calibration steps improve it further,
and both are genuinely optional:

**Lens shading (fixes dark/discoloured corners).** Point the camera at an evenly lit, out-of-focus white surface and:
```bash
a7zcam lsc-calibrate --size full -n 8
a7zcam still out.jpg --size full --lsc
```

**Dark-frame subtraction (fixes hot pixels on long exposures).** Cap the lens completely and shoot a dark frame at
the exposure/gain you'll actually use:
```bash
a7zcam dark --size full --exposure 15 --gain 1 -n 5
a7zcam still sky.jpg --size full --exposure 15 --gain 1 --dark ~/.config/a7zcam/dark_full_15s_g1.npz
```
Both are per-size and, for the dark frame, per-exposure/gain — recalibrate if you change modes or the board's
temperature changes a lot.

For raw files you intend to develop properly yourself (Lightroom, RawTherapee, darktable), skip both of these and
just use `--raw out.dng` — the DNG carries the untouched sensor data plus metadata, and you get full control in
your own raw converter instead of this project's judgment calls.

## 10. Raspberry-Pi-flavoured commands

If you're used to `rpicam-still` / `rpicam-vid` from the Pi, thin wrappers of the same names are installed and
translate the familiar options to `a7zcam` underneath:
```bash
rpicam-still -o photo.jpg --width 1920 --height 1080 --shutter 20000 --gain 2 --autofocus-mode auto -r
rpicam-vid   -o clip.h264 -t 10000 --width 1920 --height 1080 --framerate 30
```
This is a convenience layer, **not** libcamera — Picamera2-style code won't work here. If you specifically need
real libcamera (for GStreamer's `libcamerasrc`, or libcamera's own Python bindings), that exists separately and is
documented in [LIBCAMERA.md](LIBCAMERA.md), with its own, more serious limitations (no autofocus through libcamera,
slower/variable frame delivery from the software ISP).

## 11. When something doesn't work

The three things that explain 90% of problems, in order of likelihood:
1. **Something else still has the camera open.** See §2.
2. **A previous process crashed hard (`kill -9`) and left a `v4l2-ctl` child holding `/dev/video0` even though its
   parent is gone.** `fuser /dev/video0` shows the real PID; `kill -9` that PID specifically.
3. **The scene is too dark or too flat for the thing you're asking it to do** (autofocus, auto-exposure).

Beyond that, [TROUBLESHOOTING.md](TROUBLESHOOTING.md) has a symptom-by-symptom table (kernel oops, half-empty
frames, "chip id register = 0x0000", etc.) and `scripts/collect-info.sh` for filing a bug report.

## 12. Reference: every `a7zcam` subcommand

| Command | Does |
|---|---|
| `a7zcam still OUT.jpg [--size S] [--af] [--exposure S] [--gain N] [--focus N] [--raw OUT.dng] [--rotate deg] [--hflip/--vflip] [--lsc] [--dark FILE]` | one photo |
| `a7zcam video OUT.mp4 --seconds N [--size S] [--fps N] [--encoder auto\|hw\|sw] [--bitrate N] [--af]` | record video |
| `a7zcam focus [--af] [--set N]` | focus only, prints the resulting lens position |
| `a7zcam live [--port N]` | web live view + control API |
| `a7zcam webcam [--size S] [--fps N]` | publish `/dev/video20` for other apps |
| `a7zcam lsc-calibrate --size S [-n N]` | lens-shading calibration |
| `a7zcam dark --size S --exposure S --gain N [-n N]` | dark-frame calibration |

Sizes everywhere: `720p 1080p 2k 4k full`.
