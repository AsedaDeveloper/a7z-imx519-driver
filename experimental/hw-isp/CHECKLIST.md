# IMX519 on Cubie A7Z: road to a Raspberry-Pi-level camera

Tick a box (`- [x]`) when an item is finished AND verified. `tools/progress.sh` prints the overall and per-section percentage.
Priority order to start with: **B9 (hardware encoder) -> B4/B5/B7 (drops, stalls, soak test) -> A1/B13 (highlights, ISP stages) -> B1 (proper V4L2 node) -> B14 (calibration)**.
Items marked (verify) depend on a hardware block that must first be confirmed on our vendor kernel.

## A. Carried over from the first "still to do" list
- [ ] A1 Highlight protection without the sharpness loss. **Oct 26: partial — ISP stage flag 24 (`tuning_v7`) cuts clipped pixels by ~32% (3 paired runs) with no cost to noise, colour or detail, unlike `A7Z_HAE`. Not enough on its own; PLTM/DRC curves still untouched.** See HIGHLIGHTS.md
- [ ] A2 Check white balance (tuning_v6) under daylight and lamps, not only the iPad chart **Oct 27: daylight passes (paper towel, lit area: R/G 0.986, B/G 0.985 vs targets 0.98-1.00 / 1.00-1.04; B/G ~1.5% low, within the 0.979-0.992 spread across regions). Warm and cool cases still open: no lamps available, plan is iPad as light source (screen-simulated) then real low-sun / open-shade daylight.**
- [x] A3 Autofocus with a moving subject: settle-wait, 0.90 / 0.80 triggers, futile-refocus guard (done Oct 24, verified live)
- [x] A4 Newer autofocus watcher, exposure cap and fixes at full resolution — done by teaching `a7zcap` the 2-in-1 mode (`A7Z_MERGE=1`) instead of porting into `a7zdual`: **4656x3496 at 15.1 fps, 1491 frames, 0 dropped; lens knocked 1.5 dpt out of focus recovered 7/7 in 1.83-1.96 s; 2 ms exposure cap clamps to 2000-2008 us**. Flaw found: about one start in three has the left ISP half empty; detected at start-up, exit code 6 (retry). See README.md
- [ ] A5 Cool light above ~8000 K still reads blue (find the AWB limit that stops libisp choosing the coolest lights)
- [x] A6 Higher-resolution / higher-fps preview — full 1080p30 over USB via hardware H.264 (~0.75 MB/s at 6 Mbit/s); viewer `lv_enc.sh`

## B. Make it a proper camera device, stability, hardware blocks, tuning
### B-1 Standard interfaces
- [x] B1 Standard V4L2 node — `sudo v4l2node start` publishes the ISP output (autofocus, white balance, exposure cap) on `/dev/video9` as single-planar YU12 1080p30; a plain V4L2 client with no vendor ioctls read 20/20 frames. Needs a loopback because libisp cannot run outside the streaming process (proof in V4L2_NODE.md). A media-controller topology for sensor/ISP/lens is still not exposed
- [x] B19 Standard apps read the camera with no custom tool — **verified on /dev/video9: `v4l2-ctl` 20 frames, `ffmpeg` h264 1080p, GStreamer `v4l2src`+jpegenc 20 JPEGs, `cv2.VideoCapture(9)` 20/20 at (1080,1920,3), plus `python/a7z_v4l2.py` at 28.1 fps with no dependencies.** Needed Debian's stock gstreamer1.0-plugins-good (Radxa's patched v4l2src fails on any standard device) — see V4L2_NODE.md. Control of focus/exposure through V4L2 is still B2
- [ ] B2 Lens as a standard V4L2 control (`FOCUS_ABSOLUTE`) instead of vendor ioctls. **Oct 26: not possible as-is and measured why — the `ak7375_act` lens subdev exposes no controls at all, and the `focus_absolute` on the vin nodes reaches libisp but never moves the lens (sharpness flat across 0..127). Shipped `a7zfocus <dpt|af|auto>` over a control file instead (works from any language, verified from OpenCV: 3.7/8.3/6.2 sharpness at 3/7/11 dpt).** A real control needs the kernel actuator driver to register it plus loopback proxying. See CONTROLS.md
- [x] B3 Device-tree overlay and a service — the overlay is now a plain `.dtbo` so `u-boot-update` puts it on every generated entry (the `.trial` suffix was why `default l0` kept losing the camera, twice in one day), and `a7z-camera.service` brings up the loopback and the supervised feeder at boot. Verified across two reboots: service active, `/dev/video9` present, OpenCV 15/15 frames, nothing typed. See BOOT.md
### B-2 Stability
- [x] B4 Dropped frames — autofocus now encodes every frame it dequeues (no ~1.5 s hole per refocus) and the capture queue is 8 buffers deep (`A7Z_NBUF`): **0 dropped frames in 9.6 min at 1080p30 across 8 refocuses**. Cached buffers / DMA-buf not needed so far; revisit if a heavier consumer is added
- [x] B5 Stall recovery watchdog — `tools/a7zstream.sh` supervises a7zcap (exit 3 = encoder wedged -> reload `sunxi_ve`, exit 4 = capture stalled, other -> retry); verified by killing the process mid-stream and by a busy device. See STABILITY.md
- [ ] B6 MIPI link margin: error counters, checks across lane speeds, CRC/ECC handling (link-damage metric exists)
- [x] B7 Soak test and regression script — `tools/soak.sh`; 9.6 min at 1080p30 after the autofocus fix: 17900 frames, **min and median 30.0 fps, 0 dropped frames over 8 refocuses**, 0 restarts, 0 errors. Hours-long runs and replug still to do
- [ ] B8 Exposure/gain latency compensation per sensor (settings take effect a couple of frames late) **Oct 27: the latency is now measured, 4 frames at 1080p30 (see METADATA.md) and used when labelling frames; whether libisp's own AE loop allows for it (and hunts because it does not) is not examined, so not ticked.**
### B-3 Hardware blocks not used yet
- [x] B9 Hardware video encoder, H.264 / H.265 — works and is wired into a7zcap (`A7Z_ENC=`); 1088p at ~110 fps H.264 / ~127 fps H.265. **Ceiling: nothing above 1080p encodes** (vendor demo fails too). See ENCODER.md
- [x] B10 Hardware JPEG encoder — works, ~80 fps at 1920x1088, valid JPEG verified (`A7Z_ENC=jpeg`)
- [ ] B11 2D engine (G2D) for scaling / rotation / colour conversion instead of the CPU box downscale **Oct 27: `/dev/g2d` (5440000.g2d, sunxi-g2d.h) and dma-buf heaps exist, but `G2D_CMD_BITBLT_H` scaling 1920x1080 -> 640x360 returns 0 and writes nothing, in 4 variants (Y8 / ARGB8888, system / reserved heap, dma-buf sync around CPU access). Not working yet; `userspace/g2dtest.c` is the repro. The CPU box downscale costs only ~3-5 ms/frame on one core, so the payoff is small.**
- [ ] B12 NPU for on-device detection (verify). **Oct 27: `/dev/vipcore` and the `vipcore` module are present (VeriSilicon-style NPU), but no userspace runtime (libVIPlite / OpenVX / awnn) is installed; untested**
- [ ] B13 More ISP stages: local tone mapping / DRC, 2D and 3D noise reduction, sharpening, defect-pixel correction, lens shading. **Oct 26: all 32 stage flags swept one at a time (HIGHLIGHTS.md) — flag 24 helps and is deployed, flag 27 is sharpest but clips more, flag 6 blanks the frame, the other 10 are worse on both counts; `flags_all_on` gives a black frame. Stage identities and the curve parameters still unknown** **Oct 27: the V4L2 user controls (`sharpness`, `saturation`, `contrast`, `brightness`) are advertised but measurably inert, so the param file is the only way in; a fourth identification route (libisp verbose logging via the exported `isp_*_log_param` ints, `A7Z_ISPLOG=`) also failed to name the stages.**
### B-4 Tuning (the Pi's real advantage)
- [ ] B14 Per-module calibration: lens shading, black level, noise profile per ISO, colour matrix per illuminant, gamma / tone curve (white balance done: v6; matrix near identity, not deployed)
- [ ] B15 Proper chart (ColorChecker) and real lamps instead of the iPad
### B-5 Sensor modes and features
- [ ] B16 More sensor modes (binned, higher fps) and a clean mode switch without reload
- [ ] B17 Check whether the IMX519 phase-detect data is usable for much faster autofocus
- [x] B18 Per-frame metadata — one record per frame (exposure, gain, lens position, colour gains, autofocus flags) in a shared ring `/run/a7zcam.meta` and an optional JSON-lines log, matched to a `/dev/video9` frame by its timestamp: **90/90 frames matched as an ordinary user; recorded exposure correlates 1.000 with frame brightness at offset 0 (-0.76..+0.08 elsewhere)**. Measured the exposure pipeline latency at the same time: **4 frames** at 1080p30. `Camera.read_with_meta()` in `python/a7z_v4l2.py`. See METADATA.md

## C. Second list (smaller items and extras)
### C-1 Image quality and features
- [ ] C1 Raw Bayer capture bypassing the ISP (DNG-style files) for tuning, astro and own processing
- [ ] C2 Flicker (50/60 Hz banding) handling under artificial light
- [ ] C3 HDR / multi-exposure merge for highlights
- [ ] C4 Hot / defective pixel mapping (matters in long dark exposures)
- [ ] C5 Lens calibration per module: focus code to distance, infinity position, temperature drift
- [ ] C6 Autofocus extras: focus lock, tap-to-focus (AE/AF window controls exist), AF assist in the dark
### C-2 System side
- [x] C7 Run as a normal user instead of `sudo` — udev rules (`tools/99-a7z-camera.rules`) and a group-writable control file: reading `/dev/video9`, `a7zfocus` and a hand-run `a7zcap` (90 frames) all verified as `radxa` with no sudo. The systemd service stays root (module loading, `sunxi_ve` reload). See BOOT.md
- [ ] C8 Thermal and power behaviour in a closed case at full load; clock pinning versus power draw
- [ ] C9 Accurate timestamps / shared clock for the balloon and observatory logs
- [ ] C10 Dual-camera sync
- [ ] C11 One diagnostics tool (drops, link errors, exposure, white balance, autofocus state) **Oct 27: partial — `tools/a7zstatus` (human + `--json`, exit codes) reports service, drops, rate, autofocus, live frame stats and white-balance ratios, temperatures, libisp errors; exposure/gain/lens now come from the B18 metadata ring; only MIPI link errors (B6) are not covered. Building it found and fixed a silent failure: a feeder with no loopback ran "healthy" and published nothing; a7zcap now exits 5 and the supervisor reloads the loopback. See STABILITY.md**
### C-3 Project side
- [ ] C12 Documentation of what is verified, plus the soak-test script (see B7)
- [ ] C13 Clean the driver for the Radxa / mainline kernel (upstreaming)
- [ ] C14 Beta-tester feedback pass (Reddit post is live)

## Already done (for context, not counted above)
- Sweep autofocus 24.5 s -> ~1 s, local sweep ~0.6 s, defocus recovery ~0.9 s; frame-drop fix (cached ROI, big-core pinning)
- White-balance light table v5 -> v6 (grey R/G 0.98-1.00, B/G 1.00-1.04 on the chart); deployed
- Live preview 640x360 @ 30 fps over the USB link; exposure cap `A7Z_MAXEXP`; full-range decode in the viewer
- Driver + tooling published (github.com/AsedaDeveloper/a7z-imx519-driver), full-res 4656x3496 through the hardware ISP
