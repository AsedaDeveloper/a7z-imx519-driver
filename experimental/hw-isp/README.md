# Experimental: Allwinner hardware ISP (ISP602) path for the IMX519

> **Status: experimental, tested on one board / one sensor module / one lens.** Nothing here replaces the main (raw-path)
> driver. It is a research branch of work that gets real, colour-processed frames out of the A733's *hardware* ISP.

The main driver in this repo gives you raw Bayer and does the processing on the CPU. This folder shows how to push the sensor
through Radxa's vendor `vin` stack and Allwinner's closed-source ISP library (`libAWIspApi` / `libisp`) instead.

## What works

| | |
|---|---|
| Hardware-ISP frames (YUV420) | yes, 1920x1080 and 2328x1748 at 30 fps, **3264x2448 at ~13 fps** |
| Colour | white balance, CCM and black level tuned against a displayed 24-patch chart (see `tuning/`) |
| Autofocus | own userspace contrast-detect AF in `a7zcap` (libisp's own AF never focuses on this module) |
| Lens driver | `ak7375_act.c` + a fix for a mutex that Radxa's `actuator.c` never releases (hangs the process in D state) |

## What does not work (yet)

* **Anything wider than 3264.** The single ISP is limited to 3264 pixels per line (`vin_os.c: max_width = 3264`, and
  `sunxi_isp.c` clamps the format to 3264 unless `large_image == 3`). 3840x2160 and the full 4656x3496 negotiate but never
  deliver a frame. The vendor "2-in-1" mode (CSI splits one sensor stream into two overlapping halves for ISP0/ISP1) is the way
  around it; `userspace/a7zdual.c` is an experiment that drives both video nodes. **It does not work yet**: both nodes and both
  ISPs start, but the kernel logs `sunxi_csi.1 setup link sunxi_tdm_rx.0 fail!` and no frames arrive, even for the known-good
  3264x2448 mode. The second CSI channel -> TDM rx1 media link is not being set up. Full 16 MP is only available through the
  raw-path driver.
* **Software flat-field (`A7Z_FLAT`) is off and ships no map.** Applying a gain map to the final 8-bit image clips highlights (27%
  of a bright test frame). A correct fix belongs in the ISP's own lens-shading stage on raw data.
* Colour tuning is specific to one module/lens/screen. Treat `tuning_v4.json` as an example, not a calibration for your camera.

## Why the sensor was stuck at 2328x1748 (the useful finding)

`sensor_find_frame_size()` in the BSP first keeps only the modes whose `fps_fixed` equals the frame rate requested through
`S_PARM`, then picks among those. Demo programs ask for 30 fps, so only the 30 fps mode is ever eligible, and other resolutions are
produced by the VIPP scaler from it. Select a mode by asking for **its** frame rate: `A7Z_FPS=13 a7zcap 3264 2448 ...`.
(9 = 4656x3496, 13 = 3264x2448, 18 = 3840x2160, 30 = 2328x1748, 60 = 1920x1080.)

## Layout

```
userspace/a7zcap.c      capture through libAWIspApi; env: A7Z_FPS A7Z_AF A7Z_FOCUS A7Z_SWEEP A7Z_EVERY A7Z_STDOUT A7Z_LARGE A7Z_FLAT
userspace/a7zdual.c     2-in-1 dual-node experiment (does not work yet, see above)
userspace/liveview.sh   live view over ssh + ffplay
kernel/imx519_mipi.c    sensor driver for the vendor vin framework (adds the 3264x2448 centre-crop mode)
kernel/ak7375_act.c     AK7375 focus actuator
kernel/actuator-mutex-fix.patch   fix for the vendor actuator.c
overlay/imx519-j5-isp.dts         device-tree overlay for ISP mode
tuning/                 build a libisp parameter file from YOUR OWN library (see below)
tools/                  chart scorer, (experimental) flat-field map builder
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
```

`--export-dynamic` is required: `a7zcap` exports its own `ioctl()` so it can swallow libisp's lens commands while it runs its own
autofocus. Kernel modules are built out of tree against the Radxa BSP (`github.com/radxa/allwinner-bsp`, tag `cubie-aiot-v1.4.6`,
`drivers/vin`); apply `kernel/actuator-mutex-fix.patch` to the BSP `actuator.c` first. Boot with the ISP overlay (not the bypass
overlay from the main driver). YUV frames are written as raw planar 4:2:0 with 1088 padded luma rows at 1080p.

## Licence

GPL-2.0-or-later, like the rest of the repo. The kernel files derive from Allwinner/Radxa BSP sources (GPL) and the Raspberry Pi
IMX519 driver; see their headers.
