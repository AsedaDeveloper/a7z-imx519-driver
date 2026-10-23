# IMX519 camera driver for the Radxa Cubie A7Z

Linux driver, device-tree overlay and tools for the **Sony IMX519 16 MP autofocus camera module** (Waveshare
"IMX519-78 16MP AF Camera", Arducam IMX519, and compatible Raspberry Pi 15-pin IMX519 modules) on the
**Radxa Cubie A7Z** (Allwinner A733) using Radxa's Debian image.

> **Status: beta.** It captures real images, runs manual exposure up to minutes, does contrast autofocus and a live
> view, and has survived stress tests. It is **not** a drop-in replacement for the Raspberry Pi camera stack:
> there is no hardware ISP (processing is done on the CPU); libcamera support is new and limited. See [What works](#what-works) and
> [Known limitations](#known-limitations) before you build on it.
> A separate, experimental path through the SoC's hardware ISP lives in [`experimental/hw-isp/`](experimental/hw-isp/README.md).

Raspberry Pi OS supports this module out of the box. The Allwinner A733 does not: Radxa ships no IMX519 driver, the
SoC's camera framework (VIN) differs completely from V4L2/libcamera on the Pi, and the board's camera connector
has a few traps that cost the author days. This repository documents and fixes all of them.

## What works

| Feature | State |
|---|---|
| RAW10 Bayer capture at the sensor's rated speeds: 720p 85 fps, 1080p 60, 2328×1748 30, 3840×2160 18, **full 4656×3496 9.4 fps** | works |
| Exact manual exposure, 1/10000 s … 15 s tested (hardware allows ~118 s) | works |
| Manual gain 1× … 64× (analog to 16×, then digital) | works |
| Standard V4L2 controls on `/dev/video0`: `exposure`, `gain`, `focus_absolute` | works (needs the VIN patch) |
| Standard V4L2 exposure / gain on the sensor sub-device `/dev/v4l-subdev0`, live while streaming | works |
| Autofocus motor (AK7375) as a V4L2 actuator sub-device | works |
| Contrast autofocus (one-shot sweep) and continuous autofocus (user space) | works |
| JPEG stills + raw **DNG** (opens in Lightroom / RawTherapee / darktable / dcraw); colour matrix from the Pi's IMX519 tuning | works (`a7zcam still`) |
| Sensor test pattern as the standard `test_pattern` V4L2 control | works |
| Lens-shading calibration + dark-frame / hot-pixel correction (`a7zcam lsc-calibrate`, `a7zcam dark`) | works on synthetic data and a smoke test; needs your own calibration shots |
| H.264 video recording with the **hardware encoder** (Allwinner Cedar VE): 1080p at 30 fps, 720p 30 fps, 4K ~17 fps | works (`a7zcam video`; needs GStreamer + the Allwinner OMX plugin, otherwise falls back to software x264 at ~17 fps) |
| **libcamera** (`cam`, GStreamer `libcamerasrc`, Python bindings) with software ISP: auto exposure, white balance, colour | works, beta (`scripts/build-libcamera.sh`, see [docs/LIBCAMERA.md](docs/LIBCAMERA.md)); Picamera2 not supported |
| Virtual webcam `/dev/video20` (YUYV 1080p30) for browsers, ffmpeg, OpenCV, Frigate | works (`a7zcam webcam`, needs `v4l2loopback`) |
| Web live view with exposure, gain, focus, AE, AWB, autofocus | works (`a7zcam live`) |
| Long-exposure / astro use (persistent stream, no wasted first frame) | works, see [docs/USAGE.md](docs/USAGE.md) |

## Known limitations

* **Video:** H.264 uses the hardware encoder when GStreamer and the Allwinner OMX plugin are installed (see docs/USAGE.md), otherwise libx264 in software (~17 fps at 1080p). The demosaic and tone curve still run on the CPU (~3 cores at 1080p30). No audio, no raw video, only H.264 in MP4.
* **No ISP.** The A733's ISP needs Allwinner's closed user-space libraries. Everything an ISP does (black level,
  white balance, demosaic, auto exposure, autofocus) is done in user space by the tools here. Colour is
  "good", not calibrated; lens shading is correctable but only after you calibrate your module; no noise reduction.
* **Only the RAW formats stream** (`RG10`, `RG12`); the ISP-processed formats (`NV12`, `RGB3`, …) deliver no frames.
* Frame timing is stable: the same setting gave 282.827-282.836 ms over four reboots (0.003 %), about 0.1 % longer than the nominal line-time calculation.
* Exposure changes need ~2 frames to reach the output (a sensor property). A fresh stream always wastes its first
  frame (a VIN property): keep the stream open for continuous capture.
* Flip / mirror are not implemented in the sensor (they would change the Bayer order); the tools flip in software.
* Tested on **one board, one module, one kernel (6.6.98-5-aw2511)**. Reports from other setups are welcome.

## Hardware

* Radxa Cubie A7Z (the 31-pin camera connector `J5`)
* IMX519 module with the standard **15-pin 1.0 mm** Raspberry Pi connector (Waveshare IMX519-78, Arducam B0371, …)
* **Radxa camera cable AC008** (31-pin 0.3 mm → 15-pin 1.0 mm, "same side") - the module's own Pi Zero / Pi 5 cable will
  not fit the A7Z. See [docs/HARDWARE.md](docs/HARDWARE.md) for the pinout and the orientation warning.

## Install

On the A7Z (Radxa Debian 13, kernel `6.6.98-5-aw2511`, network access):

```bash
sudo apt install git build-essential device-tree-compiler patch python3-numpy python3-opencv \
                 linux-headers-$(uname -r)
git clone https://github.com/AsedaDeveloper/a7z-imx519-driver.git && cd a7z-imx519-driver
sudo ./scripts/install.sh
sudo reboot
```

Then:

```bash
a7zcam still ~/photo.jpg --size 1080p --af --rotate 0 --raw ~/photo.dng
a7zcam live                      # http://<board-ip>:8080 in a browser
v4l2-ctl -d /dev/video0 --set-input=0 --list-ctrls
```

Details, options and removal: [docs/INSTALL.md](docs/INSTALL.md), [docs/USAGE.md](docs/USAGE.md),
[docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md), [docs/TESTED.md](docs/TESTED.md) (what has and has not been tested - reports welcome). How and why it works (read this if you port it to another
sensor): [docs/HOW_IT_WORKS.md](docs/HOW_IT_WORKS.md).

## What is in this repository

```
driver/imx519_mipi.c     sensor driver for the Allwinner VIN framework (GPL-2.0)
driver/ak7375_act.c      autofocus lens motor driver (VIN "actuator" subdevice) (GPL-2.0)
overlay/imx519-j5.dts    device-tree overlay for connector J5 (csi1/mipi1, TWI3 at 100 kHz, PE8/PE9)
patches/                 patch for the BSP's VIN core (stops kernel crashes with standard V4L2 tools)
scripts/                 install.sh, uninstall.sh, collect-info.sh (for bug reports), udev rule
tools/a7zcam.py          still / raw DNG / focus tool + library      (MIT)
tools/liveview.py        web live view with AE, AWB, autofocus       (MIT)
tests/                   unit tests for the calibration code (no camera needed): python3 -m unittest discover -s tests
docs/                    install, usage, hardware, troubleshooting, how it works
```

## How this was made

The idea, the hardware, the direction and all of the real-world testing came from **AsedaDeveloper**, who wanted this
camera to work on the Radxa Cubie A7Z and kept pushing until it did. **All of the code and documentation in this
repository was written with [Claude](https://claude.com/claude-code) (Anthropic's AI coding assistant)**, working
interactively on the real board: reading the schematics and the vendor BSP source, building and loading the kernel
modules, measuring what the hardware actually did, and iterating until it worked. Every claim in the documentation was
checked on a real A7Z with a real module; where something is untested the text says so.

## Credits and licences

* Register tables and sensor logic derived from the Raspberry Pi kernel's `imx519` driver (GPL-2.0).
* Driver structure from Radxa's / Allwinner's BSP sensor and actuator drivers (GPL-2.0).
* Kernel code: **GPL-2.0** (`LICENSE`); user-space tools: **MIT** (`LICENSE-MIT`).
* IMX519 is a trademark of Sony; this project is not affiliated with Sony, Radxa, Allwinner, Waveshare or Arducam.
