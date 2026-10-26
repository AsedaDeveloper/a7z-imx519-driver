# IMX519 on Cubie A7Z: road to a Raspberry-Pi-level camera

Tick a box (`- [x]`) when an item is finished AND verified. `tools/progress.sh` prints the overall and per-section percentage.
Priority order to start with: **B9 (hardware encoder) -> B4/B5/B7 (drops, stalls, soak test) -> A1/B13 (highlights, ISP stages) -> B1 (proper V4L2 node) -> B14 (calibration)**.
Items marked (verify) depend on a hardware block that must first be confirmed on our vendor kernel.

## A. Carried over from the first "still to do" list
- [ ] A1 Highlight protection without the sharpness loss (find libisp's own exposure target / tone-curve / DRC settings; own-exposure code `A7Z_HAE` costs sharpness)
- [ ] A2 Check white balance (tuning_v6) under daylight and lamps, not only the iPad chart
- [x] A3 Autofocus with a moving subject: settle-wait, 0.90 / 0.80 triggers, futile-refocus guard (done Oct 24, verified live)
- [ ] A4 Bring the newer autofocus watcher, exposure cap and fixes into the full-resolution tool (`a7zdual`)
- [ ] A5 Cool light above ~8000 K still reads blue (find the AWB limit that stops libisp choosing the coolest lights)
- [x] A6 Higher-resolution / higher-fps preview — full 1080p30 over USB via hardware H.264 (~0.75 MB/s at 6 Mbit/s); viewer `lv_enc.sh`

## B. Make it a proper camera device, stability, hardware blocks, tuning
### B-1 Standard interfaces
- [ ] B1 Standard V4L2 + media-controller node (sensor, ISP, lens) so GStreamer / ffmpeg / OpenCV work directly, not only through `a7zcap` / `a7zdual`
- [ ] B19 Standard apps get ISP-processed, autofocused frames with no custom tool: `cv2.VideoCapture(0)`, GStreamer `v4l2src`, `ffmpeg -f v4l2`, `v4l2-ctl` (focus / exposure / white-balance controls) and a Picamera2-style Python API. Needs B1, B2 and the ISP running as a daemon (libisp must be started by something); done when one test script passes with each of them (B1 is the V4L2 node itself, B19 is the acceptance test)
- [ ] B2 Lens as a standard V4L2 control (`FOCUS_ABSOLUTE`) instead of vendor ioctls
- [ ] B3 Device-tree overlay and a service so it works after every boot with no manual steps (**found Oct 26**: the boot default had reverted to `l0`, which has no camera overlay, so the camera failed to probe; set back to `l0cam`, but u-boot-update can reset it again)
### B-2 Stability
- [ ] B4 Dropped frames: cached buffers or DMA-buf, more queued buffers (VIN drops frames when userspace holds uncached buffers)
- [x] B5 Stall recovery watchdog — `tools/a7zstream.sh` supervises a7zcap (exit 3 = encoder wedged -> reload `sunxi_ve`, exit 4 = capture stalled, other -> retry); verified by killing the process mid-stream and by a busy device. See STABILITY.md
- [ ] B6 MIPI link margin: error counters, checks across lane speeds, CRC/ECC handling (link-damage metric exists)
- [x] B7 Soak test and regression script — `tools/soak.sh`; 9.7 min at 1080p30: 17621 frames, median 30.0 fps, 0 restarts, 0 errors. **Found: the only frame drops (228) are the ~45-frame holes each autofocus run punches in the stream.** Hours-long runs and replug still to do
- [ ] B8 Exposure/gain latency compensation per sensor (settings take effect a couple of frames late)
### B-3 Hardware blocks not used yet
- [x] B9 Hardware video encoder, H.264 / H.265 — works and is wired into a7zcap (`A7Z_ENC=`); 1088p at ~110 fps H.264 / ~127 fps H.265. **Ceiling: nothing above 1080p encodes** (vendor demo fails too). See ENCODER.md
- [x] B10 Hardware JPEG encoder — works, ~80 fps at 1920x1088, valid JPEG verified (`A7Z_ENC=jpeg`)
- [ ] B11 2D engine (G2D) for scaling / rotation / colour conversion instead of the CPU box downscale (verify)
- [ ] B12 NPU for on-device detection (verify)
- [ ] B13 More ISP stages: local tone mapping / DRC, 2D and 3D noise reduction, sharpening, defect-pixel correction, lens shading
### B-4 Tuning (the Pi's real advantage)
- [ ] B14 Per-module calibration: lens shading, black level, noise profile per ISO, colour matrix per illuminant, gamma / tone curve (white balance done: v6; matrix near identity, not deployed)
- [ ] B15 Proper chart (ColorChecker) and real lamps instead of the iPad
### B-5 Sensor modes and features
- [ ] B16 More sensor modes (binned, higher fps) and a clean mode switch without reload
- [ ] B17 Check whether the IMX519 phase-detect data is usable for much faster autofocus
- [ ] B18 Per-frame metadata (exposure, gain, lens position stored with every frame, like libcamera)

## C. Second list (smaller items and extras)
### C-1 Image quality and features
- [ ] C1 Raw Bayer capture bypassing the ISP (DNG-style files) for tuning, astro and own processing
- [ ] C2 Flicker (50/60 Hz banding) handling under artificial light
- [ ] C3 HDR / multi-exposure merge for highlights
- [ ] C4 Hot / defective pixel mapping (matters in long dark exposures)
- [ ] C5 Lens calibration per module: focus code to distance, infinity position, temperature drift
- [ ] C6 Autofocus extras: focus lock, tap-to-focus (AE/AF window controls exist), AF assist in the dark
### C-2 System side
- [ ] C7 Run as a normal user (udev rules / permissions) instead of `sudo`
- [ ] C8 Thermal and power behaviour in a closed case at full load; clock pinning versus power draw
- [ ] C9 Accurate timestamps / shared clock for the balloon and observatory logs
- [ ] C10 Dual-camera sync
- [ ] C11 One diagnostics tool (drops, link errors, exposure, white balance, autofocus state)
### C-3 Project side
- [ ] C12 Documentation of what is verified, plus the soak-test script (see B7)
- [ ] C13 Clean the driver for the Radxa / mainline kernel (upstreaming)
- [ ] C14 Beta-tester feedback pass (Reddit post is live)

## Already done (for context, not counted above)
- Sweep autofocus 24.5 s -> ~1 s, local sweep ~0.6 s, defocus recovery ~0.9 s; frame-drop fix (cached ROI, big-core pinning)
- White-balance light table v5 -> v6 (grey R/G 0.98-1.00, B/G 1.00-1.04 on the chart); deployed
- Live preview 640x360 @ 30 fps over the USB link; exposure cap `A7Z_MAXEXP`; full-range decode in the viewer
- Driver + tooling published (github.com/AsedaDeveloper/a7z-imx519-driver), full-res 4656x3496 through the hardware ISP
