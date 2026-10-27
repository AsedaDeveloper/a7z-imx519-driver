# IMX519 camera driver for the Radxa Cubie A7Z

![status: beta](https://img.shields.io/badge/status-beta-orange)
![board: Radxa Cubie A7Z](https://img.shields.io/badge/board-Radxa%20Cubie%20A7Z-blue)
![kernel: 6.6.98-5-aw2511](https://img.shields.io/badge/kernel-6.6.98--5--aw2511-lightgrey)
![licence: GPL-2.0 / MIT](https://img.shields.io/badge/licence-GPL--2.0%20%2F%20MIT-green)

Linux driver, device-tree overlay and tools for the **Sony IMX519 16 MP autofocus camera module** on the **Radxa Cubie A7Z** (Allwinner A733), using Radxa's Debian image.
Works with the Waveshare "IMX519-78 16MP AF Camera", the Arducam IMX519, and compatible Raspberry Pi 15-pin IMX519 modules.

**At a glance**

* Full-resolution **RAW10** capture up to 4656×3496, and the sensor's rated speeds at lower sizes (720p at 85 fps, 1080p at 60)
* Exact manual exposure and gain through standard V4L2 controls, plus contrast **autofocus**
* **JPEG and raw DNG** stills, **hardware H.264** video, a **virtual webcam**, a **web live view**, and beta **libcamera** support
* Everything an ISP normally does (black level, white balance, demosaic, auto exposure, autofocus) runs in user space on the CPU

> [!WARNING]
> **Status: beta.** It captures real images, runs manual exposure up to minutes, does contrast autofocus and a live view, and has survived stress tests. It is **not** a drop-in
> replacement for the Raspberry Pi camera stack: there is no hardware ISP on this driver's kernel, and libcamera support is new and limited.
> Read [What works](#what-works) and [Known limitations](#known-limitations) before you build on it.

> [!TIP]
> **Want the hardware ISP instead?** A separate, experimental path for Radxa's older vendor 5.15 kernel runs the sensor through the SoC's ISP: 1080p30 as an ordinary `/dev/video9` camera
> with autofocus, per-frame metadata and a watchdog, and the full 16 MP at up to 15 fps. It is a different software stack, not an upgrade of this one.
> See [`experimental/hw-isp/`](experimental/hw-isp/README.md) and its one-page [VERIFIED.md](experimental/hw-isp/VERIFIED.md).

## Contents

[Why this exists](#why-this-exists) · [Which one do I want?](#which-one-do-i-want) · [What works](#what-works) · [Known limitations](#known-limitations) · [Hardware](#hardware) ·
[Install](#install) · [Quick start](#quick-start) · [Documentation](#documentation) · [Repository layout](#repository-layout) · [Tested setups](#tested-setups-and-bug-reports) ·
[How this was made](#how-this-was-made) · [Credits and licences](#credits-and-licences)

## Why this exists

Raspberry Pi OS supports this module out of the box. The Allwinner A733 does not: Radxa ships no IMX519 driver, the SoC's camera framework (VIN) differs completely from V4L2/libcamera
on the Pi, and the board's camera connector has a few traps that cost the author days. This repository documents and fixes all of them.

## Which one do I want?

| | **This driver** (repo root) | **Hardware-ISP path** ([`experimental/hw-isp/`](experimental/hw-isp/README.md)) |
|---|---|---|
| Kernel / OS | 6.6.98-5-aw2511, Radxa Debian 13 "trixie" | vendor 5.15.147-21-a733, Radxa Debian 11 "bullseye" |
| Image processing | CPU, from raw Bayer | Allwinner's ISP, via its closed-source library |
| Raw Bayer | yes (`RG10`, `RG12`) | no |
| Processed formats (`NV12`, `RGB3` ...) | deliver no frames | yes, as a standard V4L2 node |
| Full 16 MP | 9.4 fps | up to 15 fps |
| Maturity | beta, documented, tested | experimental, one board / module / lens |

The two are not interchangeable and need different overlays.

## What works

### Capture and controls
| Feature | State |
|---|---|
| RAW10 Bayer capture at the sensor's rated speeds: 720p 85 fps, 1080p 60, 2328×1748 30, 3840×2160 18, **full 4656×3496 9.4 fps** | works |
| Exact manual exposure, 1/10000 s … 15 s tested (hardware allows ~118 s) | works |
| Manual gain 1× … 64× (analog to 16×, then digital) | works |
| Standard V4L2 controls on `/dev/video0`: `exposure`, `gain`, `focus_absolute` | works (needs the VIN patch) |
| Standard V4L2 exposure / gain on the sensor sub-device `/dev/v4l-subdev0`, live while streaming | works |
| Sensor test pattern as the standard `test_pattern` V4L2 control | works |
| Long-exposure / astro use (persistent stream, no wasted first frame) | works, see [docs/USAGE.md](docs/USAGE.md) |

### Focus
| Feature | State |
|---|---|
| Autofocus motor (AK7375) as a V4L2 actuator sub-device | works |
| Contrast autofocus (one-shot sweep) and continuous autofocus (user space) | works |

### Stills, video and streaming
| Feature | State |
|---|---|
| JPEG stills + raw **DNG** (opens in Lightroom / RawTherapee / darktable / dcraw); colour matrix from the Pi's IMX519 tuning | works (`a7zcam still`) |
| Lens-shading calibration + dark-frame / hot-pixel correction (`a7zcam lsc-calibrate`, `a7zcam dark`) | works on synthetic data and a smoke test; needs your own calibration shots |
| H.264 video with the **hardware encoder** (Allwinner Cedar VE): 1080p 30 fps, 720p 30 fps, 4K ~17 fps | works (`a7zcam video`; needs GStreamer + the Allwinner OMX plugin, otherwise falls back to software x264 at ~17 fps) |
| Virtual webcam `/dev/video20` (YUYV 1080p30) for browsers, ffmpeg, OpenCV, Frigate | works (`a7zcam webcam`, needs `v4l2loopback`) |
| Web live view with exposure, gain, focus, AE, AWB, autofocus | works (`a7zcam live`) |
| **libcamera** (`cam`, GStreamer `libcamerasrc`, Python bindings) with software ISP: auto exposure, white balance, colour | works, beta (`scripts/build-libcamera.sh`, see [docs/LIBCAMERA.md](docs/LIBCAMERA.md)); Picamera2 not supported |

## Known limitations

**Processing**
* **No ISP on this driver's kernel.** The A733's ISP needs Allwinner's closed user-space libraries (the experimental path in [`experimental/hw-isp/`](experimental/hw-isp/README.md) does drive it, but only on
  the vendor 5.15 kernel). Black level, white balance, demosaic, auto exposure and autofocus are done in user space by the tools here. Colour is "good", not calibrated; lens shading is correctable
  but only after you calibrate your module; there is no noise reduction.
* **Only the RAW formats stream** (`RG10`, `RG12`). The ISP-processed formats (`NV12`, `RGB3` ...) deliver no frames; they do on the vendor-kernel path.

**Video**
* H.264 uses the hardware encoder when GStreamer and the Allwinner OMX plugin are installed (see [docs/USAGE.md](docs/USAGE.md)), otherwise libx264 in software (~17 fps at 1080p).
  The demosaic and tone curve still run on the CPU (~3 cores at 1080p30). No audio, no raw video, only H.264 in MP4.

**Timing and orientation**
* Frame timing is stable: the same setting gave 282.827–282.836 ms over four reboots (0.003 %), about 0.1 % longer than the nominal line-time calculation.
* Exposure changes need ~2 frames to reach the output (a sensor property). A fresh stream always wastes its first frame (a VIN property): keep the stream open for continuous capture.
* Flip / mirror are not implemented in the sensor (they would change the Bayer order); the tools flip in software.

**Coverage**
* Tested on **one board, one module, one kernel (6.6.98-5-aw2511)**. Reports from other setups are welcome ([below](#tested-setups-and-bug-reports)).

## Hardware

| Part | What you need |
|---|---|
| Board | Radxa Cubie A7Z (the 31-pin camera connector `J5`) |
| Camera | IMX519 module with the standard **15-pin 1.0 mm** Raspberry Pi connector (Waveshare IMX519-78, Arducam B0371, ...) |
| Cable | **Radxa camera cable AC008** (31-pin 0.3 mm → 15-pin 1.0 mm, "same side"). The module's own Pi Zero / Pi 5 cable will **not** fit the A7Z |

See [docs/HARDWARE.md](docs/HARDWARE.md) for the pinout and the orientation warning.

## Install

On the A7Z (Radxa Debian 13, kernel `6.6.98-5-aw2511`, network access):

```bash
sudo apt install git build-essential device-tree-compiler patch python3-numpy python3-opencv \
                 linux-headers-$(uname -r)
git clone https://github.com/AsedaDeveloper/a7z-imx519-driver.git && cd a7z-imx519-driver
sudo ./scripts/install.sh
sudo reboot
```

`install.sh` refuses other kernels unless you pass `--force`. Details, options and removal: [docs/INSTALL.md](docs/INSTALL.md). If something goes wrong: [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md).

## Quick start

```bash
a7zcam still ~/photo.jpg --size 1080p --af --rotate 0 --raw ~/photo.dng   # JPEG + raw DNG, autofocused
a7zcam live                                                               # web live view at http://<board-ip>:8080
a7zcam video clip.mp4 --seconds 10 --size 1080p --fps 30              # H.264 clip (hardware encoder when available)
a7zcam webcam                                                             # virtual webcam /dev/video20
v4l2-ctl -d /dev/video0 --set-input=0 --list-ctrls                        # the standard V4L2 controls
```

All options: [docs/USAGE.md](docs/USAGE.md).

## Documentation

| Document | What it covers |
|---|---|
| [docs/INSTALL.md](docs/INSTALL.md) | install, options, removal |
| [docs/USAGE.md](docs/USAGE.md) | every `a7zcam` command, long exposures, video, webcam, live view |
| [docs/HARDWARE.md](docs/HARDWARE.md) | connector pinout, the cable, the orientation warning |
| [docs/LIBCAMERA.md](docs/LIBCAMERA.md) | building and using the patched libcamera |
| [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md) | symptoms and fixes |
| [docs/TESTED.md](docs/TESTED.md) | what has and has not been tested; reports welcome |
| [docs/HOW_IT_WORKS.md](docs/HOW_IT_WORKS.md) | how and why it works; read this if you port it to another sensor |
| [docs/GUIDE.md](docs/GUIDE.md) | a complete, front-to-back manual for the driver and `a7zcam` |
| [experimental/hw-isp/](experimental/hw-isp/README.md) | the hardware-ISP path, with [VERIFIED.md](experimental/hw-isp/VERIFIED.md) and [CHECKLIST.md](experimental/hw-isp/CHECKLIST.md) |

## Repository layout

```
driver/imx519_mipi.c     sensor driver for the Allwinner VIN framework                 (GPL-2.0)
driver/ak7375_act.c      autofocus lens motor driver (VIN "actuator" subdevice)         (GPL-2.0)
overlay/imx519-j5.dts    device-tree overlay for connector J5 (csi1/mipi1, TWI3 at 100 kHz, PE8/PE9)
patches/                 patch for the BSP's VIN core (stops kernel crashes with standard V4L2 tools)
scripts/                 install.sh, uninstall.sh, collect-info.sh (for bug reports), udev rule
tools/a7zcam.py          still / raw DNG / focus tool + library                         (MIT)
tools/liveview.py        web live view with AE, AWB, autofocus                          (MIT)
tests/                   unit tests for the calibration code (no camera needed):
                         python3 -m unittest discover -s tests
docs/                    install, usage, hardware, troubleshooting, how it works
experimental/hw-isp/     the separate hardware-ISP path for the vendor 5.15 kernel
```

## Tested setups and bug reports

Only one setup has been tested by the author: a Radxa Cubie A7Z (2 GB), Radxa Debian 13, kernel 6.6.98-5-aw2511, a Waveshare IMX519-78 on connector J5 with the AC008 cable.
Everything else is unknown until someone reports it. The full table, with what "works" means, is in [docs/TESTED.md](docs/TESTED.md).

If you try another board, kernel or module, please open an issue with the **"Tested setup report"** template and paste the output of `scripts/collect-info.sh`. It takes two minutes.

## How this was made

The idea, the hardware, the direction and all of the real-world testing came from **AsedaDeveloper**, who wanted this camera to work on the Radxa Cubie A7Z and kept pushing until it did.
**All of the code and documentation in this repository was written with [Claude](https://claude.com/claude-code) (Anthropic's AI coding assistant)**, working interactively on the real board:
reading the schematics and the vendor BSP source, building and loading the kernel modules, measuring what the hardware actually did, and iterating until it worked.
Every claim in the documentation was checked on a real A7Z with a real module; where something is untested the text says so.

## Credits and licences

* Register tables and sensor logic derived from the Raspberry Pi kernel's `imx519` driver (GPL-2.0).
* Driver structure from Radxa's / Allwinner's BSP sensor and actuator drivers (GPL-2.0).
* Kernel code: **GPL-2.0** ([`LICENSE`](LICENSE)); user-space tools: **MIT** ([`LICENSE-MIT`](LICENSE-MIT)).
* IMX519 is a trademark of Sony; this project is not affiliated with Sony, Radxa, Allwinner, Waveshare or Arducam.
