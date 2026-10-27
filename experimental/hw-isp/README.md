# Experimental: Allwinner hardware ISP (ISP602) path for the IMX519

> **Status: experimental, tested on one board / one sensor module / one lens.** Nothing here replaces the main (raw-path)
> driver. It gets real, colour-processed frames out of the A733's *hardware* ISP, **up to the full 4656x3496 (16.3 MP) at 15 fps**.

The main driver in this repo gives you raw Bayer and does the processing on the CPU. This folder shows how to push the sensor
through Radxa's vendor `vin` stack and Allwinner's closed-source ISP library (`libAWIspApi` / `libisp`) instead.

## Requirements: this is a different software stack from the main driver

| | main driver (repo root) | this folder |
|---|---|---|
| OS / kernel | Radxa Debian 13 "trixie", kernel 6.6.98 | Radxa Debian 11 "bullseye", **vendor kernel 5.15.147-21-a733** |
| BSP | `cubie-aiot-v1.5.0` | `cubie-aiot-v1.4.6` (`drivers/vin`) |
| Sensor driver | V4L2 sensor driver | driver for Allwinner's `vin` sensor framework (`kernel/imx519_mipi.c`) |
| Processing | CPU (raw Bayer out) | Allwinner's ISP, via its closed-source library |
| Tested module | Waveshare IMX519-78 AF on connector J5 | the same module type on J5 |

The two are not interchangeable and were not tested on any other combination. Boot the ISP overlay from this folder, not the main
driver's overlay.

## What works

| | |
|---|---|
| Hardware-ISP frames (YUV420) | yes, 1920x1080 and 2328x1748 at 30 fps, **3264x2448 at ~13 fps** |
| **Full 16 MP through the ISP** | **yes, 4656x3496 at 9.4 fps (`A7Z_FPS=9`) or 15 fps (`A7Z_FPS=15`)**, and 3840x2160 at 18 fps, using the vendor 2-in-1 mode, see below (and [What was tested](#what-was-tested-and-what-that-does-not-show)) |
| Colour | white balance, CCM and black level tuned against a displayed 24-patch chart (see `tuning/`) |
| Autofocus | own userspace contrast-detect AF (libisp's own AF never focuses on this module), modelled on the Raspberry Pi libcamera search: a lens sweep in dioptre space, then 3 settled samples and a parabola. In `a7zcap` and in the `a7zdual` live preview (`A7Z_AF=1`). `a7zcap`: image-scored 10-cycle runs land on the sharpest lens position with no early stops, 34 frames per focus, 1.8-2.5 s. The earlier 3 s / ~10 s split was our own process: the capture buffers are uncached DMA memory, reading them pixel by pixel took 50-300 ms per frame on a little core, and the driver dropped 1 frame in 2 to 1 in 8 (frame sequence numbers jumped) while a buffer was held. `a7zcap` now copies the region with row `memcpy`, returns the buffer first, and pins itself to a big core (`A7Z_CPU=n` overrides, `-1` disables). `A7Z_AF_CDAF=1` selects the plain libcamera-style stepper (72 frames, can stop early in dim scenes). The preview version was checked on a bottle 15 cm away (4 runs, 6.1-6.8 dioptre) |
| Lens driver | `ak7375_act.c` + a fix for a mutex that Radxa's `actuator.c` never releases (hangs the process in D state) |

## What does not work (yet)

* **Full resolution through `a7zcap` (`A7Z_MERGE=1`).** `a7zcap` now speaks the 2-in-1 mode too, so the whole current feature set (sweep autofocus with the settle-wait watcher, `A7Z_MAXEXP` cap, drop accounting, `A7Z_HEALTH`, `A7Z_FRESH`) works at 4656x3496: `A7Z_MERGE=1 A7Z_FPS=15 A7Z_AF=1 A7Z_MAXEXP=30000 A7Z_HEALTH=1 a7zcap 4656 3496 N PREFIX` (`A7Z_NBUF=4`). Verified: **15.1 fps for 1491 frames, 0 dropped**, a lens knocked 1.5 dioptre out of focus is detected and recovered in 1.83-1.96 s (7 of 7), and the exposure cap clamps (2 ms cap gives 2000-2008 us). **Known flaw:** roughly one start in three comes up with the *left ISP half empty for the whole run* (AE then blows out the right half); `a7zcap` checks the first frames and exits with code **6** so the caller can simply start again. No hardware encoder at this size (nothing above 1080p encodes), so no loopback/H.264 here.
* `a7zdual` is kept for its decimated live preview (`A7Z_STREAM`); it still has the older autofocus watcher, no exposure cap and no drop accounting.
* No seam handling has been needed so far, but only a handful of scenes were checked; look for a join at the middle column on yours.
* **Software flat-field (`A7Z_FLAT`) is off and ships no map.** Applying a gain map to the final 8-bit image clips highlights (27%
  of a bright test frame). A correct fix belongs in the ISP's own lens-shading stage on raw data.
* Colour tuning is specific to one module/lens/screen. Treat `tuning_v4.json` as an example, not a calibration for your camera.

## Why the sensor was stuck at 2328x1748 (the useful finding)

`sensor_find_frame_size()` in the BSP first keeps only the modes whose `fps_fixed` equals the frame rate requested through
`S_PARM`, then picks among those. Demo programs ask for 30 fps, so only the 30 fps mode is ever eligible, and other resolutions are
produced by the VIPP scaler from it. Select a mode by asking for **its** frame rate: `A7Z_FPS=13 a7zcap 3264 2448 ...`.
(9 = 4656x3496, 13 = 3264x2448, 18 = 3840x2160, 30 = 2328x1748, 60 = 1920x1080.)

## Full 16 MP: the 2-in-1 mode

The single ISP is limited to 3264 pixels per line (`vin_os.c: max_width = 3264`; `sunxi_isp.c` clamps the format to 3264 unless
`large_image == 3`). Wider modes use the vendor "2-in-1" mode: the CSI parser splits one sensor stream into two overlapping halves,
one per ISP, and both DMAs write into one buffer. **No kernel change is needed**, but three things must all be right
(`userspace/a7zdual.c`):

1. `SET_DMA_MERGE = 1` and open **only `/dev/video1`**. The driver forwards `S_INPUT`/`S_FMT`/`S_PARM` to `video0` itself (forcing
   `large_image = 3`) and streams both pipelines. Opening both nodes yourself makes them fight over the media links (`sunxi_csi.1
   setup link sunxi_tdm_rx.0 fail!`, because the media core refuses to change a link while its CSI is streaming).
2. Request the sensor mode's own frame rate through `S_PARM` (see the table above).
3. Call libisp's `ispSetIspLargeImage(id)` on **both** ISP instances before `ispStart`. The demo path never calls it. Without it the
   pipeline delivers one or two frames and then stalls with `tdm0 rx0 buffer full`, at every size.

```sh
A7Z_MERGE=1 A7Z_FPS=15 ./a7zdual 4656 3496 20 /tmp/cap/full    # full 16 MP, 15 fps (A7Z_FPS=9: 9.4 fps, stock clocks)
```

### Going faster than 9.4 fps at 16 MP

The stock full-res mode runs the sensor at 816 Mbit/s per lane (9.44 fps). `A7Z_FPS=15` selects a second full-res mode whose clocks
are module parameters (`/sys/module/imx519_mipi/parameters/fast_{ivt,iop,hts,vts}`, read at every stream start; try new values
with `tools/sweep_fps.sh`). The defaults give **15.06 fps at 1296 Mbit/s per lane**; see [What was tested](#what-was-tested-and-what-that-does-not-show) for exactly what that claim rests on.
What limits it, measured on one module and one 15-pin cable:

| lane rate | fps | isolated outlier pixels per Mpx | picture |
|---|---|---|---|
| 1224 Mbit/s | 14.2 | 0.1 | clean |
| 1296 Mbit/s | 15.06 | 0.1 | clean (default) |
| 1344 Mbit/s | 15.6 | **143** | **a trace: looked clean by eye and to the magenta test, but the outlier count is already 1000x the floor** |
| 1368 Mbit/s | 15.9 | 10,538 | damaged (scattered speckle) |
| 1392 Mbit/s | 15.1 | 39,103 | visible magenta ring |
| 1500 Mbit/s | 16.6 | 233,000 | badly damaged |
| 1560 Mbit/s and up | no frames at all | | |

(Each figure is one frame of a bright indoor scene; see the caveats below.)

* **The link, not the ISP, is the limit.** The module has two lanes. A frame carries ~81 Mbit per lane, so ~1.35 Gbit/s gives ~16 fps at best.
* **Frames keep arriving while the picture is damaged.** Counting frames or watching the kernel log tells you nothing; score the image with
  `tools/corruption_check.py`. The errors are data dependent (they show up at certain brightness values), so a flat test patch can pass
  while a gradient fails. Only a few indoor scenes (bright, dim, high-contrast) were tested for the defaults; a longer or different-scene soak would be wise before
  trusting them on another module or a longer cable.
* The IVT multiplier saturates at 511 (681 MHz pixel clock), and a frame length below 3572 stops the stream.

### What was tested, and what that does not show

| run | what was captured | what was checked |
|---|---|---|
| 16 MP at the defaults (`A7Z_FPS=15`) | one run of 300 frames, one bright, mostly smooth indoor scene | 300 frames delivered, 0 `buffer full` and 0 `frame lost` kernel messages, 15.06 fps measured over frames 5..300. **Only the 300th frame was scored** with `tools/corruption_check.py`: 0.0000% magenta, largest row-brightness jump 1.6 |
| 3840x2160 (`A7Z_FPS=18`, stock clocks) | one run of 60 frames, the same kind of scene | 60 frames delivered, 0 `buffer full`, 18.02 fps measured. **Only the 60th frame was scored**, with the same formula adapted to that frame size (the script in `tools/` handles the 4656x3496 layout only): 0.0000% magenta, row jump 1.3 |
| 16 MP at the defaults, a different scene | three 30-frame runs of a bottle scene (real pink/red label graphics in view) | last frame of each scored: 0.20-0.26% magenta, but only 5-8% of those pixels are in sparse neighbourhoods and the mask shows compact blobs on the label; the picture shows no ring, stripes or banding. This is real magenta content, not damage, **but a frame that contains real magenta cannot rule out a few hundred speckle pixels** |

| 16 MP at the defaults, a dim scene | 300-, 100- and 150-frame runs (three separate runs) of a dim scene (mean luma 12-17 of 255, no zero pixels), plus a 60-frame run at the stock clocks (`A7Z_FPS=9`) as a control | 0 `buffer full`, 0 `frame lost`, 0 I2C errors. One frame per run scored: **0.0 outlier pixels per Mpx at the defaults and at the stock clocks alike**, row/column brightness jumps below 0.35. The default ran at only 11.1-11.3 fps here (stock 9.44): in dim light auto-exposure lengthens the exposure past the frame time, so the frame rate is scene dependent. A fully covered lens gives an all-zero picture after black-level subtraction and tests nothing |

| 16 MP at the defaults, a high-contrast scene | six 60-frame runs alternating stock clocks (`A7Z_FPS=9`) and the default (three each), plus a 300-frame run of each, all on the same scene | 0 `buffer full`, 0 `frame lost`, 0 I2C errors. One frame per run scored: **stock 1.3-1.7 outliers per Mpx, default 1.7-2.4** (300-frame runs: 1.4 and 1.8). The default is consistently about 1 per Mpx (roughly 15 pixels a frame) above the control, against 143 at 1344 Mbit/s: effectively at the floor, but not identical to it. The default ran at 14.6-15.0 fps here |

These checks are **limited to those scenes and those frames. They do not prove that every frame is free of errors.**

* One frame per run was scored, not all of them. A rare, brief link error in another frame would not be seen.
* Link errors depend on the pixel data, and the checker only looks for two symptoms (magenta speckle and row-brightness jumps). Other
  patterns (very dark or fully saturated scenes, fine high-contrast detail, other colours) were not tested, and damage that does not
  produce those symptoms would pass.
* One module, one cable, one session, no long-duration or temperature test.
* The default (1296 Mbit/s) scored at the floor (0.1 outliers per Mpx) in the scenes tested, but the next step up (1344, only 4% faster) already shows a
  trace (143) and 1368 is clearly damaged. That is a small margin. Treat the default as a starting point: on your hardware, sweep with `tools/sweep_fps.sh`,
  score several different scenes, and drop `fast_iop` if anything looks wrong.
* `tools/corruption_check.py` counts **isolated outlier pixels** (a pixel more than 25 away from the mean of its four neighbours) and also checks
  row-brightness jumps and magenta speckle. The outlier count is the sensitive one: it rises steeply and monotonically with lane rate (table above) and
  caught the 1344 Mbit/s trace that the magenta test missed. It is still a heuristic. Very sharp detail (printed text, specular glints) also produces
  outliers, so a score must be judged against a capture of the **same scene at the stock clocks** (`A7Z_FPS=9`): in an earlier bottle scene (glints and
  label text) the default scored 49 per Mpx and no same-scene stock capture was taken; the later high-contrast scene with a proper control did not
  reproduce it (2 per Mpx), so the 49 was most likely real detail, but that was not proven. The magenta test is unreliable in dim scenes (a low-light colour cast scored as "large magenta" even at the stock clocks) and is skipped there.

The output file is raw planar 4:2:0 with **padded plane heights** (3504 luma rows for 3496), exactly as `bytesused` reports; decode it
with the padded sizes or the chroma planes come out misaligned.

## Live view at full resolution

```sh
A7Z_HOST=radxa@board userspace/liveview_full.sh          # full 4656x3496 at 15 fps (default)
A7Z_HOST=radxa@board userspace/liveview_full.sh uhd      # 3840x2160 at 18 fps
```

The sensor, the ISP and the capture run at full size and frame rate; only the copy sent over the link is **decimated** (every 4th
pixel for `full`, every 3rd for `uhd`), because a full 16 MP frame is 24 MB and 15 fps would need ~370 MB/s. The preview is 1164x874
(about 23 MB/s over USB). The first ~10 s are frozen while autofocus scans; it refocuses if the picture stays soft. The streaming
mode is `A7Z_STREAM=D` in `a7zdual`; full-resolution stills are the normal `a7zdual` capture.

## Layout

```
userspace/a7zcap.c      capture through libAWIspApi (single node, up to 3264); env: A7Z_FPS A7Z_AF A7Z_FOCUS A7Z_SWEEP A7Z_EVERY A7Z_STDOUT A7Z_FLAT
userspace/a7zdual.c     2-in-1 capture for widths above 3264, up to the full 4656x3496; env: A7Z_MERGE A7Z_FPS A7Z_STREAM A7Z_STREAM_SKIP A7Z_AF
userspace/liveview.sh   live view over ssh + ffplay for a7zcap (640x480)
userspace/liveview_full.sh   live view of the full-resolution a7zdual capture, with autofocus
kernel/imx519_mipi.c    sensor driver for the vendor vin framework (adds the 3264x2448 centre-crop mode and the tunable 15 fps full-res mode)
kernel/ak7375_act.c     AK7375 focus actuator
kernel/actuator-mutex-fix.patch   fix for the vendor actuator.c
overlay/imx519-j5-isp.dts         device-tree overlay for ISP mode
tuning/                 build a libisp parameter file from YOUR OWN library (see below)
tools/                  chart scorer, (experimental) flat-field map builder, fps sweep + link-corruption checker
```

## You need your own copy of the Allwinner library

`libAWIspApi`, `libisp`, `libisp_ini` and the default tuning data are closed source and **are not in this repo**. They come with
Radxa's image (package `libawispapi-isp-602-arm64`). `tuning/extract_isp_defaults.py` reads the default parameter struct straight
out of your installed `libisp_ini.so`; `tuning/mkparam.py` patches it:

```sh
tuning/extract_isp_defaults.py /usr/lib/aarch64-linux-gnu/libisp_ini.so defaults.bin
tuning/mkparam.py defaults.bin isp_param_config.bin tuning/tuning_v4.json
A7Z_HOST=radxa@board tuning/deploy_param.sh isp_param_config.bin     # -> /mnt/isp_param_config.bin
```

## Build and run (on the board)

```sh
# userspace (needs libc6-dev and the vendor headers sunxi_camera_v2.h / AWIspApi.h from the BSP)
gcc -O2 -o a7zcap userspace/a7zcap.c -Wl,--no-as-needed -Wl,--export-dynamic \
    -lAWIspApi -lisp -lm -lpthread -ldl -Wl,--allow-shlib-undefined
A7Z_AF=1 A7Z_EVERY=10 ./a7zcap 1920 1080 100 /tmp/cap/shot            # autofocus + frames
A7Z_FPS=13 A7Z_FOCUS=1830 ./a7zcap 3264 2448 12 /tmp/cap/big           # 8 MP
# a7zdual builds with the same gcc line (replace a7zcap.c / a7zcap)
```

`--export-dynamic` is required: `a7zcap` exports its own `ioctl()` so it can swallow libisp's lens commands while it runs its own
autofocus. Kernel modules are built out of tree against the Radxa BSP (`github.com/radxa/allwinner-bsp`, tag `cubie-aiot-v1.4.6`,
`drivers/vin`); apply `kernel/actuator-mutex-fix.patch` to the BSP `actuator.c` first. Boot with the ISP overlay (not the bypass
overlay from the main driver). YUV frames are written as raw planar 4:2:0 with 1088 padded luma rows at 1080p.

## Possible follow-up (not done)

The lane-rate limit found here is a property of the sensor and the 2-lane link, not of this software stack. The main driver's full-resolution
mode runs at the stock 816 Mbit/s (9.4 fps); the same clock settings (`fast_ivt`/`fast_iop`/`fast_hts`, `vts >= 3572`) might give it ~15 fps too.
That has **not** been tried on the main driver. If you try it, score the picture with `tools/corruption_check.py` against the stock clocks of
the same scene; frames keep arriving while the picture is damaged.

## Licence

GPL-2.0-or-later, like the rest of the repo. The kernel files derive from Allwinner/Radxa BSP sources (GPL) and the Raspberry Pi
IMX519 driver; see their headers.

### Preview / live-stream notes (Oct 24 2026)
* `A7Z_STREAM_D=n` box-downscales the preview n x n (USB link ~10 MB/s: D=3 is 640x360 @ 30 fps, D=2 is 960x540 @ ~13 fps). The ISP output is full-range YUV: decode with `scale=in_range=pc`.
* `A7Z_MAXEXP=<us>` caps exposure and raises gain instead (30000 keeps 30 fps in a dim room; more noise).
* Autofocus watcher: block-based sharpness metric, local +-2 dpt sweep first (~0.6 s), full sweep on failure (~1 s), trigger below 0.95 / above 1.2 of the reference. `A7Z_AF_TEST_DEFOCUS=<dpt>` knocks the lens once to time the recovery.
* `A7Z_HAE=1` highlight-aware exposure: replaces libisp's exposure request (its target/bias controls are inert). Sparse luma sample per frame; backs off when >1% of pixels clip (`A7Z_HAE_CLIP`), comes up toward mean luma `A7Z_HAE_TARGET` (118) only when nothing clips; frozen during autofocus; the first sweep waits for it to settle. `A7Z_HAE_LOG=1` prints it. **Off by default: it costs sharpness** (centre Laplacian variance 14.6 -> 9.2 in bright light, 13.4 -> 2.4-4.3 in dim light) because libisp keeps requesting more exposure than we apply and selects its noise reduction from that request. Use only when clipped highlights matter more than detail.
* AF watcher: checks every 2nd frame, triggers after 3 low checks (2 if the drop is below 0.7) when sharpness falls below 0.90 of the reference (0.80 for ~3 s after a refocus that moved the lens <0.2 dpt: stops hunting inside the depth of field). While the last 3 readings still fluctuate (spread >0.12, a moving subject) it waits up to ~2 s for the scene to settle before sweeping, so it does not lock onto a wrong peak.
