# The Camera Guide

A complete, front-to-back manual for the IMX519 camera driver (`a7z-imx519-driver`, command-line tool `a7zcam`) on
the Radxa Cubie A7Z. This document assumes the driver is already installed (see [INSTALL.md](INSTALL.md) if not)
and walks through everything you can actually do with the camera once it's running: every command, every flag,
every gotcha, with the real numbers measured on this hardware rather than vague claims. If you're looking for how
to physically wire the module or connect to the board itself, that's a different document — see the board guide
you were sent alongside this one. This one is purely about the camera.

---

## Table of contents

1. [What you actually have, and why it works the way it does](#1-what-you-actually-have-and-why-it-works-the-way-it-does)
2. [The one rule: the camera is exclusive](#2-the-one-rule-the-camera-is-exclusive)
3. [Taking a photo — every option explained](#3-taking-a-photo--every-option-explained)
4. [Recording video — every option explained](#4-recording-video--every-option-explained)
5. [Live view — the web control panel](#5-live-view--the-web-control-panel)
6. [Watching the feed natively, without a browser](#6-watching-the-feed-natively-without-a-browser)
7. [Autofocus, honestly explained](#7-autofocus-honestly-explained)
8. [Exposure, gain and focus — what the numbers actually mean](#8-exposure-gain-and-focus--what-the-numbers-actually-mean)
9. [Programming against it directly in Python](#9-programming-against-it-directly-in-python)
10. [Standard V4L2 access (below the Python layer)](#10-standard-v4l2-access-below-the-python-layer)
11. [The virtual webcam](#11-the-virtual-webcam)
12. [Colour, raw files, and getting better image quality](#12-colour-raw-files-and-getting-better-image-quality)
13. [Raspberry-Pi-flavoured commands (rpicam-*)](#13-raspberry-pi-flavoured-commands-rpicam-)
14. [libcamera (separate, more limited path)](#14-libcamera-separate-more-limited-path)
15. [When something doesn't work](#15-when-something-doesnt-work)
16. [Full command reference](#16-full-command-reference)

---

## 1. What you actually have, and why it works the way it does

A Raspberry Pi camera module (Sony IMX519, 16 MP, autofocus) plugged into a Radxa Cubie A7Z board. The one fact
that explains almost everything else in this guide:

> **The Raspberry Pi has a dedicated camera chip (an ISP) that does demosaicing, white balance and autofocus
> math in hardware. On the kernel this guide targets, the A7Z's chip (Allwinner A733) has no usable equivalent — Allwinner's own ISP needs
> closed libraries this project doesn't ship (an experimental path that does drive it exists for the older vendor kernel, in
> `experimental/hw-isp/`). So every one of those jobs is done in software, on the CPU, by the tools
> in this repository.**

That's why stills take a moment to process after the shot, why video tops out around 30 fps instead of something
higher, and why autofocus is a deliberate step-by-step sweep instead of an instant hardware snap. It's also why you
get things the Pi doesn't normally hand you directly: full manual exposure down to microseconds and up past a
minute, full manual gain, raw sensor data you can develop yourself in a proper raw converter, and complete visibility
into every decision the "automatic" modes make, because there's no black-box ISP hiding them.

What this project actually delivers, concretely:
- A command-line tool, `a7zcam`, that feels like the Raspberry Pi's own `rpicam-still` / `rpicam-vid`.
- A `Camera` Python class underneath it, usable directly in your own scripts.
- A web-based live view you can control from any browser on the network.
- Thin wrapper commands (`rpicam-still`, `rpicam-vid`, `rpicam-hello`) for scripts written for the Pi.
- A real raw-DNG path, standard V4L2 device access, and (separately, with real limitations) libcamera support.

## 2. The one rule: the camera is exclusive

**Only one program can hold the camera at a time.** `/dev/video0` is a single device file and the driver doesn't
allow two readers. If the live view (`a7zcam live`) is running and you then try `a7zcam still`, it will fail with
`RuntimeError: no frames from the camera (is the imx519_mipi driver loaded?)` — which sounds like a driver problem
but almost always just means *something else already has it open*.

This causes more confusion than anything else in this guide, so check it first whenever anything camera-related
fails unexpectedly:
```bash
pgrep -af "a7zcam|liveview"       # lists any process holding the camera
kill <pid>                        # or Ctrl-C the terminal it's running in
```

**A sharper version of the same trap:** if a camera-using process gets killed hard (`kill -9`, a crashed script,
an SSH session dropping mid-command), it can leave an internal `v4l2-ctl` child process still running and still
holding `/dev/video0`, even though the Python program that started it is long gone. `pgrep` for `a7zcam`/`liveview`
won't show this — you have to ask who actually holds the device file:
```bash
fuser /dev/video0        # prints the real PID holding it open
kill -9 <that pid>       # then it's actually free
```
This exact situation came up repeatedly during development — a stream died, the device stayed locked, and every
subsequent attempt failed with the same confusing "no frames" error until the orphaned `v4l2-ctl` was found and
killed directly. If `a7zcam` says "no frames from the camera" and you're sure nothing is obviously running, this
is almost certainly why — check `fuser` before assuming the driver itself is broken.

## 3. Taking a photo — every option explained

```bash
a7zcam still photo.jpg
```
That alone takes a full-resolution (4656×3496) photo with automatic exposure. Every flag, in full:

| Flag | Meaning |
|---|---|
| `--size S` | `720p`, `1080p`, `2k` (2328×1748), `4k` (3840×2160), or `full` (4656×3496, the default for stills). Smaller sizes read out faster, so AF and metering are quicker too. |
| `--af` | Run a full autofocus sweep (§7) before shooting. |
| `--exposure S` | Exposure time in **seconds** (e.g. `0.5`, `15`). Overrides auto-exposure. |
| `--gain N` | Analogue/digital gain as a multiplier (`1` to `64`). Overrides auto-exposure's own gain choice. |
| `--focus N` | Lens position directly, `0` (infinity) to `4095` (closest). Skips autofocus entirely — instant, if you already know where you want it. |
| `--raw OUT.dng` | Also save the untouched sensor data as a DNG, alongside the processed JPEG. |
| `--rotate 0\|90\|180\|270` | Rotate the output. The sensor itself can't flip or rotate (see §8), so this is done in software after capture. |
| `--hflip` / `--vflip` | Mirror horizontally / vertically, also done in software. |
| `--color tuned\|simple` | `tuned` (default) uses the Raspberry Pi's own published IMX519 colour matrix; `simple` is plain gray-world white balance with no colour correction. |
| `--saturation N` | Strength of the colour matrix, `0` (none) to `1` (the Pi's matrix at full strength — tends to look over-saturated without the rest of the Pi's pipeline around it). Default `0.6`. |
| `--lsc [FILE]` | Apply lens-shading (vignetting) correction from a calibration file made with `a7zcam lsc-calibrate` (§12). With no file, uses the default calibration path for the current size. |
| `--dark FILE` | Subtract a master dark frame made with `a7zcam dark` (§12) — also removes hot pixels. |

Worked examples:
```bash
a7zcam still photo.jpg --size 1080p --af                      # autofocus first, then shoot
a7zcam still photo.jpg --exposure 0.5 --gain 4 --focus 2400    # fully manual, no auto anything
a7zcam still photo.jpg --raw photo.dng                         # also keep the raw sensor data
a7zcam still photo.jpg --rotate 90 --hflip                     # correct for how the module is mounted
a7zcam still night.jpg --exposure 15 --gain 2 --raw night.dng  # a 15-second exposure
```
If neither `--exposure`/`--gain` nor `--af`/`--focus` are given, the tool runs its own auto-exposure and leaves
focus wherever the lens last was — pass `--af` explicitly if you want it refocused every time.

## 4. Recording video — every option explained

```bash
a7zcam video clip.mp4 --seconds 10 --size 1080p --fps 30
```

| Flag | Meaning |
|---|---|
| `--seconds N` | Clip length. Required — there's no "record until stopped" mode (see §6 for continuous streaming instead). |
| `--size S` | Same sizes as stills. 4K is capped by the sensor itself at ~18 fps regardless of encoder. |
| `--fps N` | Target frame rate. |
| `--encoder auto\|hw\|sw` | `hw` = the Allwinner Cedar VE hardware encoder via GStreamer; `sw` = software libx264; `auto` (default) uses hardware when it's available and falls back to software otherwise. |
| `--bitrate N` | Hardware encoder bitrate, bits/second. Left at `0` (default), it's chosen from size and fps. |
| `--crf N` / `--preset P` | Software (libx264) quality/speed controls, standard x264 meanings. |
| `--focus N` / `--af` | Set focus, or autofocus once, before recording starts. There's no continuous AF during a recording. |
| `--rotate` / `--hflip` / `--vflip` | Same as stills. |

Measured real-world performance on this board (indoor lighting, gain up to 32×):

| Size | Hardware encoder | Software (libx264) |
|---|---|---|
| 1080p | 29.6–29.9 fps (~290–340% CPU of 800%) | 16–17 fps (~590% CPU) |
| 720p | 29.8 fps | — |
| 4K | 17.3 fps (sensor's own ceiling is 18 fps at this size) | — |

The hardware path is the default because of that CPU gap — software encoding alone eats most of the board. One-time
setup it needs (the installer tells you if it's missing):
```bash
sudo apt install gstreamer1.0-tools gstreamer1.0-plugins-base gstreamer1.0-plugins-good \
                 gstreamer1.0-plugins-bad gstreamer1.0-omx-allwinner
```
You also need to be in the `video` group for the hardware encoder to work from SSH or a systemd service rather
than a local logged-in session (Radxa's own udev rule only covers the local seat) — `install.sh` installs the
needed rule, but you still need to log out and back in once for your own group membership to pick it up.

If a clip comes out looking choppy, or runs the right length but with fewer distinct frames than expected: the
tool keeps the *wall-clock duration* correct by repeating the last frame when the board falls behind, rather than
speeding the clip up to compensate. A choppy clip is a real performance ceiling being hit in that moment (CPU
contention, thermal throttling, a heavy scene), not a bug losing time.

## 5. Live view — the web control panel

```bash
a7zcam live                      # add --port N to use something other than 8080
```
Open `http://<board-ip>:8080` in any browser on the same network. This is the most direct way to actually *feel*
the camera: live sliders for exposure and gain, auto-exposure and auto-white-balance toggles, a focus slider, a
one-shot autofocus button, and a continuous-autofocus toggle that keeps refocusing as the scene changes.

It streams MJPEG (a sequence of JPEGs over one long HTTP connection), which is why it works in any browser with no
plugins — but it is **not** the lowest-latency or highest-framerate way to watch the feed. It's built to *control*
the camera interactively, not to watch a fast-moving scene smoothly (see §6 for that).

Environment variables for running it at boot or with fixed settings: `LIVEVIEW_PORT`, `LIVEVIEW_ROTATE=0|90|180|270`,
`LIVEVIEW_FLIP=h|v|hv`. A systemd unit template is at `tools/a7zcam-live.service` if you want it to start
automatically.

Everything the page's buttons and sliders do is also a plain HTTP endpoint, so you can script it instead of
clicking:
```bash
curl "http://<board-ip>:8080/api/set?exposure=0.02&gain=2"   # set exposure (seconds) and gain
curl "http://<board-ip>:8080/api/af"                         # trigger one autofocus sweep (returns once done)
curl "http://<board-ip>:8080/api/status"                     # current exposure/gain/focus/fps/af-state as JSON
```
`/api/status` is worth polling if you're building your own control surface — it returns the live exposure, gain,
focus position, measured fps, whether AE/AWB/continuous-AF are on, the current `af_log` (every position+sharpness
pair from the most recent sweep, useful for actually seeing what autofocus is doing), and the estimated white
balance gains.

## 6. Watching the feed natively, without a browser

`a7zcam` doesn't currently have a built-in "stream to stdout" subcommand — `a7zcam video` always writes a
fixed-length clip to a file, and the live view is MJPEG-in-a-browser only. But the same hardware encoder it uses can
be driven directly from a short script built on the `Camera` class (§9) that writes continuous H.264 to standard
output instead of a file, piped straight into `ffplay` on your own computer over `adb` — the same feeling as the
classic `raspivid | ffplay` trick on a Pi:
```bash
adb exec-out "python3 your_stream_script.py --size 4k --fps 30" | ffplay -fflags nobuffer -flags low_delay -f h264 -i -
```
`4k` (3840×2160) is the real resolution ceiling for this path: the hardware encoder rejects the full sensor
resolution (4656×3496) outright with a negotiation error (`not-negotiated (-4)`), confirmed on this hardware — it's
a genuine encoder limit, not a tuning problem. This technique bypasses the MJPEG live view entirely and gets the
actual H.264 stream the hardware encoder produces, at native frame rate, with much lower latency than a browser's
MJPEG tag — worth turning into a real `a7zcam` subcommand (e.g. `a7zcam video - --stdout`) if you find yourself
wanting it often.

Two gotchas to know if you build this yourself, both found the hard way during development:
- **Launching it with `adb shell "... &"` to background it doesn't reliably detach** — the adb shell session can
  hang waiting for file descriptors to close. Use `adb exec-out` piped directly into the consumer instead of trying
  to background a remote process.
- **Killing the script with `kill -9`** (rather than a clean exit or plain `kill`) can leave its internal
  `v4l2-ctl` child still holding `/dev/video0` even though the parent is gone — see the sharper trap in §2.

## 7. Autofocus, honestly explained

There is no phase-detect autofocus hardware feeding this — no second sensor, no split-pixel data, nothing telling
the lens which direction to move. Every focus decision comes from **contrast-detect**: move the lens, measure how
sharp the image looks (a single number — higher roughly means more edge contrast, which roughly means better
focus), move again, repeat, and settle on whichever position scored highest.

The implementation deliberately sweeps the **entire** lens range every time — a coarse pass across all 4096
positions, then a fine pass around whichever coarse position scored best — about 27 measurements total, each one
with a short settle delay so the exposure has actually stabilized before being measured. That's roughly 27 small
lens moves per autofocus call. It will never feel like a phone's near-instant autofocus, and it's not meant to.

**Why it's built this way on purpose:** an earlier version replaced the full sweep with a smarter search that
started from the lens's current position and stopped early once it found a local peak — genuinely faster,
confirmed to cut lens moves by about 45%. But testing it against the same unchanged scene, repeatedly, showed it
settling on three different final positions and sharpness values across three runs — it was finding local bumps
in the noisy contrast signal rather than the real global peak, and which bump it found depended on where the lens
happened to already be sitting. That version was reverted. What ships now is slower but exhaustive, and exhaustive
means *consistent* — it finds the same real best focus every time, which matters more than speed for a camera
driver.

Practical notes:
- Autofocus needs a scene with real detail and light to measure against. Point it at a blank wall or into darkness
  and there's no contrast signal to climb, so it settles somewhere arbitrary — this is a fundamental limit of
  contrast-detect AF, not something a software fix can patch around.
- The live view's continuous-AF re-triggers a full sweep whenever the scene's measured sharpness drops noticeably
  — so moving the camera, or something moving through frame, will set off a new sweep. That's expected behaviour,
  not a malfunction.
- If you already know roughly where focus should sit — a fixed rig, a known subject distance — setting
  `--focus N` directly and skipping `--af` entirely is both faster and exactly as accurate, since you're bypassing
  the measurement step altogether.

## 8. Exposure, gain and focus — what the numbers actually mean

- **Exposure** (`--exposure`, `cam.set_exposure()`) is in **seconds**, anywhere from about 1/10000 s up to roughly
  118 seconds (the hardware's real ceiling; 15 s has been tested directly, longer exposures are supported by the
  same mechanism). At the raw V4L2 layer the same value is expressed in *sensor lines × 16* — you won't normally
  need that unit unless you're working directly against `/dev/v4l-subdev0` (§10).
- **Gain** (`--gain`, `cam.set_gain()`) is a plain multiplier: `1` to `64`. Below about 16× it's pure analogue gain
  on the sensor; above that it blends in digital gain. At the raw V4L2 layer it's expressed as *gain × 16*
  (so 16 = 1×, 1024 = 64×).
- **Focus** (`--focus`, `cam.set_focus()`) is a **lens position**, not a distance in any physical unit: `0` means
  the lens is at infinity focus, `4095` means it's at its closest focus. There's no calibrated mapping from position
  to actual subject distance — if you need one, you'd build it yourself by focusing on known distances and
  recording the positions.
- **A real sensor property, not a driver quirk:** changing exposure or gain takes about 2 frames to actually show
  up in the output. This is physically how the sensor applies new settings, and it's why the Python API's idiom is
  always "change the setting, then wait 2 frames before reading" (§9) — reading immediately after a change gets you
  a frame still carrying the *old* settings.

## 9. Programming against it directly in Python

The command-line tool is a thin wrapper around a `Camera` class you can use directly:
```python
import sys; sys.path.insert(0, "/opt/a7zcam")
from a7zcam import Camera

with Camera("1080p") as cam:
    cam.set_exposure(0.1)      # seconds
    cam.set_gain(2)            # 1x .. 64x
    cam.set_focus(2300)        # 0 (infinity) .. 4095 (closest)
    raw = cam.wait_frames(2)   # uint16 Bayer mosaic -- 2 frames so the new settings have actually applied
    rgb = cam.to_rgb(raw, full_res=True)   # demosaic + white balance + colour matrix -> displayable RGB
```

Methods worth knowing, beyond the three setters above:

| Method | Does |
|---|---|
| `cam.latest(after=None, timeout=5.0)` | Returns the newest available frame, optionally only one newer than a given timestamp. |
| `cam.wait_frames(n=2)` | Waits for and returns the frame `n` captures from now — the idiom for "give the new settings time to land" (§8). |
| `cam.to_rgb(raw, full_res=False, wb=None, ct=None, tuned=False, strength=0.6)` | Full development pipeline: demosaic, white balance, optional Pi colour matrix, tone curve. |
| `cam.estimate_awb(raw)` | Gray-world auto white-balance gains estimated from a frame. |
| `cam.sharpness(raw)` | The raw contrast metric autofocus is built on — use it directly if you want custom focus logic. |
| `cam.autofocus(coarse=256, fine=64, log=None)` | Runs the full sweep from §7; returns `(lens_position, sharpness)`. Pass a list to `log` to capture every (position, sharpness) pair measured. |
| `cam.auto_exposure(target=0.30, max_exp=0.5, iters=14, settle=3)` | Runs the same auto-exposure loop the CLI uses when you don't pass `--exposure`/`--gain`. |

Two things that will bite you if you don't know them going in:
- **The first frame of every new stream is always discarded** — this is an Allwinner VIN platform property, not
  something the sensor itself does, and it means a single `with Camera(...)` block that opens, grabs one frame and
  closes pays that cost every single time. For anything repeated — video, continuous capture, long exposure
  sequences — keep one `Camera` open across many reads rather than opening and closing it per shot.
- **Exposure/gain changes need `wait_frames(2)` after them**, as covered in §8 — reading immediately after a
  change gets a stale frame.

## 10. Standard V4L2 access (below the Python layer)

The camera is also a completely ordinary V4L2 device at `/dev/video0`, with the sensor's own controls exposed on
its sub-device `/dev/v4l-subdev0` and the lens motor on `/dev/v4l-subdev1`. Useful if you're integrating with
something that already speaks V4L2 — OpenCV's `cv2.VideoCapture`, GStreamer, Frigate — rather than going through
Python.
```bash
v4l2-ctl -d /dev/video0 --set-input=0 --list-ctrls
# one capture with manual exposure (lines x16) and gain (gain x16) and focus:
v4l2-ctl -d /dev/video0 --set-input=0 --set-ctrl=exposure=1500,gain=64,focus_absolute=60 \
         --set-fmt-video=width=1920,height=1080,pixelformat=RG10 --stream-mmap=4 --stream-skip=4 --stream-count=1 --stream-to=frame.raw
```
Notes:
- Only `RG10`/`RG12` (raw Bayer) formats actually stream — the ISP-processed formats a real Pi ISP would expose
  deliver no frames here, since there is no hardware ISP on this driver's kernel to produce them (the experimental `experimental/hw-isp/` path uses it on the vendor 5.15 kernel).
- Raw buffer layout: 16-bit little-endian words, RGGB Bayer order, black level 64, white level 1023, buffer height
  padded by 8 rows.
- For live changes while already streaming (rather than at stream start), use the sub-device directly:
  `v4l2-ctl -d /dev/v4l-subdev0 --set-ctrl=exposure=...`/`gain=...`.

## 11. The virtual webcam

If you want the camera to look like an ordinary webcam to software that has no idea what a raw Bayer sensor is:
```bash
a7zcam webcam --size 1080p --fps 30          # add --rotate/--hflip/--vflip/--af/--focus as needed; Ctrl-C to stop
ffmpeg -f v4l2 -i /dev/video20 -t 5 out.mp4
# or, in Python: cv2.VideoCapture("/dev/video20", cv2.CAP_V4L2)
```
This publishes a normal YUYV webcam device, `/dev/video20`, through the `v4l2loopback` kernel module (one-time
setup: `sudo apt install v4l2loopback-dkms`; `install.sh` then loads it at boot). Verified working with both
`ffmpeg -f v4l2` and OpenCV reading 1080p at 30 fps directly. It costs about 4 CPU cores while running (demosaic +
tone curve + format conversion happening continuously) and, like everything else here, only one program can use the
real camera at a time — while the webcam is running, `a7zcam still` and the live view can't open it, and vice versa.
A systemd unit template is at `tools/a7zcam-webcam.service`.

## 12. Colour, raw files, and getting better image quality

**Colour, out of the box.** `a7zcam still` demosaics the raw frame, estimates white balance by gray-world, then
applies a colour matrix and tone curve taken from the **Raspberry Pi's own published IMX519 tuning data** (same
sensor, so the numbers transfer directly) — applied at 60% strength by default (`--saturation`), because the Pi's
full ISP pipeline does additional correction around that matrix that this software path doesn't replicate. This is
a *reasonable* rendering, judged by eye — not a colour-chart-calibrated one.

**If you want accurate colour instead of this project's judgment calls:** shoot `--raw out.dng` and develop it in a
real raw converter (Lightroom, RawTherapee, darktable, dcraw) with your own profile. The DNG carries the untouched
sensor data plus metadata; nothing in this project's own rendering pipeline touches it.

**Lens-shading calibration (fixes dark/discoloured corners — vignetting).** Point the camera at an evenly lit,
out-of-focus white or neutral-grey surface (a sheet of paper against a window works; the tool reports the
brightness level, aim for 300–700 out of 1023 so it's neither clipped nor too dark):
```bash
a7zcam lsc-calibrate --size full -n 8          # saves to ~/.config/a7zcam/lsc_full.json
a7zcam still out.jpg --size full --lsc         # applies it; -o FILE to use a non-default calibration
```
Calibrate separately per size (`--size`) — the smaller modes read out a different region of the sensor, so a
calibration from one size doesn't transfer to another. The correction only affects the JPEG; a DNG shot alongside
it stays untouched, since raw converters do their own vignetting correction.

**Dark-frame and hot-pixel correction (for long exposures).** Cap the lens completely and shoot a dark frame at
the *exact* exposure and gain you intend to actually use:
```bash
a7zcam dark --size full --exposure 15 --gain 1 -n 5     # saves to ~/.config/a7zcam/dark_full_15s_g1.npz
a7zcam still sky.jpg --size full --exposure 15 --gain 1 --dark ~/.config/a7zcam/dark_full_15s_g1.npz --raw sky.dng
```
This subtracts the dark frame (keeping the black level correct) and replaces any hot pixels found in it with their
same-colour neighbours — applied to both the JPEG and the accompanying DNG. Dark current is roughly 0.5 ADU/second
at 1× gain at room temperature, so this matters most for exposures of several seconds or more. Recalibrate if you
change exposure, gain, size, or if the board's ambient temperature changes meaningfully — a dark frame only matches
the conditions it was shot under.

## 13. Raspberry-Pi-flavoured commands (rpicam-*)

If you have existing scripts written for the Pi's `rpicam-still` / `rpicam-vid` / `rpicam-hello` (or the older
`libcamera-still` names), thin wrappers of the same names are installed and translate the familiar options to
`a7zcam` underneath:
```bash
rpicam-still -o photo.jpg --width 1920 --height 1080 --shutter 20000 --gain 2 --autofocus-mode auto -r   # -r also writes a .dng
rpicam-vid   -o clip.h264 -t 10000 --width 1920 --height 1080 --framerate 30 --bitrate 8000000
rpicam-hello --list-cameras
```
This is a **convenience translation layer, not libcamera** — Picamera2-style Python code will not work against it.
Sizes must land on one of the real sensor modes (1280×720, 1920×1080, 2328×1748, 3840×2160, 4656×3496); anything
else is rounded up to the nearest larger mode with a printed warning, since there's no hardware scaler to produce
an arbitrary size. Options the real Pi stack supports that have no equivalent here (`--tuning-file`, `--awb`,
`--roi`, …) are recognized, printed as a warning, and ignored rather than silently dropped. `--lens-position` in
dioptres is not calibrated to anything physical — only `0` (infinity) is honoured as a real value; everything else
falls back to the normal autofocus/manual-focus handling.

## 14. libcamera (separate, more limited path)

Real libcamera (`cam`, GStreamer's `libcamerasrc`, libcamera's own Python bindings) is also supported, separately
from everything above, with its own, more serious limitations: no autofocus through libcamera at all (use `a7zcam`
or raw V4L2 for that), and the software ISP's frame delivery is slower and less consistent than the fully manual
`a7zcam` path, because the demosaic cost competes with the sensor's own frame timing on this CPU. If you specifically
need libcamera for something (a tool that only speaks libcamera, a `libcamerasrc` GStreamer pipeline), it's fully
documented separately in [LIBCAMERA.md](LIBCAMERA.md), including the exact build step and every measured limitation.
For everything covered in this guide, you don't need it.

## 15. When something doesn't work

The three things that explain almost every problem, in order of likelihood:
1. **Something else still has the camera open.** §2.
2. **A previous process was killed hard and left an orphaned `v4l2-ctl` child still holding `/dev/video0`.**
   `fuser /dev/video0` finds the real PID to kill. §2 and §6.
3. **The scene is too dark or too flat for what you're asking it to do** — autofocus and auto-exposure both need
   real signal to work with.

| Symptom | Cause / fix |
|---|---|
| `a7zcam`: "no frames from the camera" | camera busy (see #1/#2 above) or the kernel modules didn't load: check `lsmod` and `dmesg \| grep -iE "vin\|imx519"` |
| `Device or resource busy` on `/dev/video0` | another program has the camera; only one at a time |
| picture very dark in a lit room | still on default auto-exposure guesses, or exposure/gain explicitly set too low; try plain `a7zcam still` (auto) or raise `--exposure`/`--gain` |
| autofocus lands somewhere clearly wrong | scene has too little detail or light for contrast-detect to measure against (§7) |
| `v4l2-ctl --list-ctrls` prints an error about "Visual Angle" or similar | harmless — an unsupported ISP-only control being probed |
| kernel oops / board reboots when running `v4l2-ctl` directly | the kernel VIN patch isn't installed; re-run `install.sh`, or stick to `/dev/v4l-subdev0` for exposure/gain in the meantime |

For install-time and kernel-level issues (module load failures, device-tree overlay problems, I2C timing), see
[TROUBLESHOOTING.md](TROUBLESHOOTING.md) — it has a longer symptom table and `scripts/collect-info.sh` for
gathering a full bug report.

## 16. Full command reference

| Command | Does |
|---|---|
| `a7zcam still OUT.jpg [--size S] [--af] [--exposure S] [--gain N] [--focus N] [--raw OUT.dng] [--rotate deg] [--hflip] [--vflip] [--color tuned\|simple] [--saturation N] [--lsc [FILE]] [--dark FILE]` | one photo |
| `a7zcam video OUT.mp4 --seconds N [--size S] [--fps N] [--encoder auto\|hw\|sw] [--bitrate N] [--crf N] [--preset P] [--focus N] [--af] [--rotate deg] [--hflip] [--vflip]` | record video |
| `a7zcam focus [--set N] [--af]` | focus only; prints the resulting lens position |
| `a7zcam live [--port N]` | web live view + control API (§5) |
| `a7zcam webcam [--device PATH] [--size S] [--fps N] [--focus N] [--af] [--rotate deg] [--hflip] [--vflip]` | publish `/dev/video20` for other apps (§11) |
| `a7zcam lsc-calibrate [--size S] [-n N] [-o FILE]` | lens-shading calibration (§12) |
| `a7zcam dark [--size S] --exposure S [--gain N] [-n N] [-o FILE]` | dark-frame calibration (§12) |

Sizes everywhere: `720p` (1280×720) · `1080p` (1920×1080) · `2k` (2328×1748) · `4k` (3840×2160) · `full` (4656×3496).
