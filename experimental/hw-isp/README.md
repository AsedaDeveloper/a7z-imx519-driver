# IMX519 through the Allwinner hardware ISP

*Experimental · Radxa Cubie A7Z (Allwinner A733, ISP602) · Sony IMX519 · vendor kernel 5.15*

Colour-processed video from the A733's **hardware ISP**, served as an ordinary V4L2 camera. Up to the sensor's full **4656×3496 (16.3 MP) at 15 fps** through the ISP, and
**1080p30 on `/dev/video9`** with autofocus, white balance, per-frame metadata and a watchdog, readable by `v4l2-ctl`, ffmpeg, GStreamer and OpenCV with nothing installed.

> **Status: experimental.** Everything here was measured on **one board, one IMX519 module, one lens**. [VERIFIED.md](VERIFIED.md) is the one-page answer to "can I rely on this?",
> and [CHECKLIST.md](CHECKLIST.md) is the running list (`tools/progress.sh` prints the tally). Nothing in this folder replaces the main driver in the repository root.

## Which driver do I want?

| | main driver (repo root) | **this folder** |
|---|---|---|
| OS / kernel | Radxa Debian 13 "trixie", 6.6.98 | Radxa Debian 11 "bullseye", **vendor kernel 5.15.147-21-a733** |
| BSP | `cubie-aiot-v1.5.0` | `cubie-aiot-v1.4.6` (`drivers/vin`) |
| Sensor driver | V4L2 sensor driver | driver for Allwinner's `vin` framework (`kernel/imx519_mipi.c`) |
| Image processing | CPU, from raw Bayer | Allwinner's ISP, through its closed-source library |
| Raw Bayer | yes | no (the ISP path lists Bayer formats but streams none) |

The two are not interchangeable. Boot the ISP overlay from this folder, not the main driver's overlay.

## What you get

| | |
|---|---|
| A normal camera | `/dev/video9`, YU12, 1920×1080 at 30 fps, fed from the ISP (see [V4L2_NODE.md](V4L2_NODE.md)) |
| Full resolution | 4656×3496 at up to 15 fps, 3840×2160 at 18 fps, 3264×2448 at ~13 fps, all seven sensor modes switchable without a reload ([MODES.md](MODES.md), [FULL_RES.md](FULL_RES.md)) |
| Autofocus | our own contrast search (libisp's never focuses on this module); a 1.5 dioptre knock is recovered in 1.8–2.0 s |
| Lens as a V4L2 control | `focus_absolute` and `focus_automatic_continuous` on `/dev/video9` ([CONTROLS.md](CONTROLS.md)) |
| Per-frame metadata | exposure, gain and lens position for every frame, matched by timestamp ([METADATA.md](METADATA.md)) |
| Hardware video encoding | H.264 / H.265 / JPEG at 1088p, ~110 / ~127 / ~80 fps ([ENCODER.md](ENCODER.md)) |
| Survives reboots and stalls | systemd service, watchdog, `a7zstatus` health check ([BOOT.md](BOOT.md), [STABILITY.md](STABILITY.md)) |
| No `sudo` day to day | for a user in the `video` group |
| Tuned colour | white balance, CCM and black level against a chart, in [`tuning/`](tuning/) |
| ISP stages by name | the 32 enable flags and every parameter block's offset ([tuning/ISP_STAGES.md](tuning/ISP_STAGES.md)) |

## Using it

```sh
v4l2-ctl -d /dev/video9 --list-formats-ext                       # an ordinary camera
ffmpeg -f v4l2 -i /dev/video9 -t 5 out.mp4                       # or GStreamer, OpenCV: cv2.VideoCapture(9)

v4l2-ctl -d /dev/video9 --set-ctrl focus_automatic_continuous=0   # hold the lens
v4l2-ctl -d /dev/video9 --set-ctrl focus_absolute=700             # 7.00 dioptre (hundredths of a dioptre, 0..1200)
a7zfocus af                                                       # one autofocus now, then hold

a7zstatus                                                         # service, frame rate, drops, focus, image, sensor, link, temperatures
```

```python
from a7z_v4l2 import Camera                       # python/a7z_v4l2.py: no dependencies
with Camera() as cam:
    y, u, v, meta = cam.read_with_meta()          # meta: exposure, gain, lens position of THIS frame
```

For full resolution or scripted capture, run `a7zcap` directly (it owns the camera while it runs, so stop the service first):
`A7Z_MERGE=1 A7Z_FPS=15 A7Z_AF=1 A7Z_MAXEXP=30000 A7Z_HEALTH=1 a7zcap 4656 3496 N PREFIX` (`A7Z_NBUF=4`). Settings are environment variables; see the header of
[`userspace/a7zcap.c`](userspace/a7zcap.c) and [FULL_RES.md](FULL_RES.md).

## Known limitations

* **One flaw in full resolution.** Roughly one 2-in-1 start in three comes up with the *left ISP half empty* for the whole run. `a7zcap` checks the first frames and exits with
  **code 6** so the caller can start again; the cause is not found. 1080p is unaffected.
* **Frames keep arriving while the picture is damaged.** The default 15 fps full-resolution mode runs the MIPI link with little margin (the next step up already shows a trace of
  damage). Score the picture, do not count frames; see [LINK.md](LINK.md) and [FULL_RES.md](FULL_RES.md).
* **Highlights.** One ISP stage (`defog_en`, flag 24) cuts clipped pixels by about a third at no cost. That is not real highlight protection; a sweep of the tone-mapping parameters found nothing
  further ([HIGHLIGHTS.md](HIGHLIGHTS.md)).
* **Not available:** raw Bayer on this path; phase-detect autofocus data (the receiver carries a single RAW10 stream); a second camera (the board has one connector); a closed-case thermal figure
  (tested open-air only, 51 °C peak at 16 MP).
* **Colour tuning is specific** to one module, lens and screen. Treat `tuning/tuning_v4.json` as an example, not a calibration for your camera. Warm and cool light are not yet measured.
* **Nothing uses the 2D engine (G2D) yet.** It works from user space ([G2D.md](G2D.md)) but the pipeline does not need it.

## Documents

| | |
|---|---|
| **[VERIFIED.md](VERIFIED.md)** | what is verified and what is not, on one page |
| [CHECKLIST.md](CHECKLIST.md) | the 39-item list behind it, ticked only when measured |
| [V4L2_NODE.md](V4L2_NODE.md) · [BOOT.md](BOOT.md) · [STABILITY.md](STABILITY.md) | the camera as a device, coming up by itself, staying up |
| [CONTROLS.md](CONTROLS.md) · [METADATA.md](METADATA.md) · [TIMING.md](TIMING.md) | focus control, per-frame metadata, timestamps |
| [MODES.md](MODES.md) · [FULL_RES.md](FULL_RES.md) · [LINK.md](LINK.md) | sensor modes, 16 MP and the 2-in-1 mode, the MIPI link's margin |
| [ENCODER.md](ENCODER.md) · [THERMAL.md](THERMAL.md) · [G2D.md](G2D.md) | hardware encoder, heat, the 2D engine |
| [HIGHLIGHTS.md](HIGHLIGHTS.md) · [tuning/ISP_STAGES.md](tuning/ISP_STAGES.md) · [tuning/WHITE_BALANCE.md](tuning/WHITE_BALANCE.md) | highlights, the ISP's stages, white balance |

## Layout

```
kernel/        imx519_mipi.c (sensor, vendor vin framework), ak7375_act.c (lens), actuator-mutex-fix.patch, v4l2loopback-focus.patch
overlay/       imx519-j5-isp.dts   device-tree overlay for the ISP path
userspace/     a7zcap.c (capture + autofocus + loopback feeder), a7zdual.c (older 2-in-1 preview), v4l2cap.c (plain V4L2 client), ispd.c, enctest.c, g2d*.c, liveview*.sh
python/        a7z_v4l2.py   dependency-free reader with the metadata ring
tools/         a7zstream.sh + a7z-camera.service (the service), a7zstatus, a7zfocus, v4l2node.sh, udev rules, and the measurement scripts
tuning/        build a libisp parameter file from YOUR OWN library: extract_isp_defaults.py, mkparam.py, deploy_param.sh, the JSON tunings
```

## Setting it up

You need the vendor stack, not just this folder: Radxa's Debian 11 image on kernel `5.15.147-21-a733`, the BSP tag `cubie-aiot-v1.4.6`, and Allwinner's closed-source libraries
(`libAWIspApi`, `libisp`, `libisp_ini`, package `libawispapi-isp-602-arm64`), which are **not in this repo**.

1. **Overlay.** Build `overlay/imx519-j5-isp.dts`, install it as a plain `.dtbo` and run `u-boot-update` (a `.trial` suffix is silently skipped, which loses the camera on every regeneration): [BOOT.md](BOOT.md).
2. **Kernel modules.** Build `kernel/imx519_mipi.c` and `kernel/ak7375_act.c` out of tree against the BSP's `drivers/vin`; apply `kernel/actuator-mutex-fix.patch` to the BSP's `actuator.c` first.
3. **Loopback.** Build v4l2loopback 0.12.7 with `kernel/v4l2loopback-focus.patch` for the focus controls (without the patch everything works except the V4L2 focus controls); options in `/etc/modprobe.d/v4l2loopback.conf`.
4. **Userspace.** `a7zcap` needs `--export-dynamic` (it exports its own `ioctl()` to swallow libisp's lens commands while it runs its own autofocus):
   ```sh
   gcc -O2 -Wl,--export-dynamic -o a7zcap userspace/a7zcap.c -lAWIspApi -lisp -lvencoder -lMemAdapter -lVE -lvenc_base -lcdc_base -lpthread -ldl -lm
   ```
5. **Service.** `tools/a7zstream.sh`, `tools/a7z-camera.service` and `tools/99-a7z-camera.rules` (device permissions for the `video` group): [BOOT.md](BOOT.md).
6. **Colour (optional).** Build a parameter file from your own library:
   ```sh
   tuning/extract_isp_defaults.py /usr/lib/aarch64-linux-gnu/libisp_ini.so defaults.bin
   tuning/mkparam.py defaults.bin isp_param_config.bin tuning/tuning_v7.json
   A7Z_HOST=radxa@board tuning/deploy_param.sh isp_param_config.bin     # -> /mnt/isp_param_config.bin
   ```

## Possible follow-up (not done)

The lane-rate limit is a property of the sensor and its 2-lane link, not of this software. The main driver's full-resolution mode runs at the stock 816 Mbit/s (9.4 fps); the same clock settings
(`fast_ivt`/`fast_iop`/`fast_hts`, `vts >= 3572`) might give it ~15 fps too. That has **not** been tried there. If you try it, score the picture against the stock clocks of the same scene with
`tools/corruption_check.py`.

## Licence

GPL-2.0-or-later, like the rest of the repo. The kernel files derive from Allwinner/Radxa BSP sources (GPL) and the Raspberry Pi IMX519 driver; see their headers.
