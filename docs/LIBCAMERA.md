# libcamera / Raspberry Pi application compatibility

**Status: libcamera works (beta).** With the patched libcamera build in this repository the camera is a normal
libcamera camera: `cam`, GStreamer `libcamerasrc` and the libcamera Python bindings see it and get processed RGB
frames with automatic exposure, white balance and colour correction (libcamera's *software ISP*, running on the CPU).

| Raspberry Pi thing | Here | Status |
|---|---|---|
| libcamera `cam -l` / capture | `lc-a7z cam -l`, `lc-a7z cam -c1 -C30 -s width=1920,height=1080,pixelformat=BGR888 --file=f-#.ppm` | **works** (1080p 30 fps in good light) |
| GStreamer `libcamerasrc` | `lc-a7z gst-launch-1.0 libcamerasrc ! video/x-raw,format=RGBA,width=1916,height=1080 ! ...` | **works**, also into the hardware H.264 encoder (see below) |
| libcamera Python bindings | `lc-a7z python3 -c "import libcamera"` | **works** (camera, properties, capture) |
| Picamera2 | - | **not supported**: Picamera2 needs the Raspberry Pi pipeline handler / ISP |
| `rpicam-still/vid/hello` | `rpicam.py` wrappers on top of `a7zcam` (not libcamera) | works for the common options |
| Hardware H.264 | `a7zcam video`, or GStreamer `omxh264videoenc` | works |
| Webcam for browsers / ffmpeg / OpenCV | `a7zcam webcam` (/dev/video20) | works |

## Install
```bash
sudo ./scripts/build-libcamera.sh        # ~30-40 min on the A7Z; installs into /opt/libcamera-a7z
lc-a7z cam -l                             # lc-a7z = wrapper that points programs at that build
```
The Debian `libcamera` packages are not touched; programs started without `lc-a7z` keep using them (and see no camera).
The build needs the kernel side from `install.sh` (sensor driver with the standard controls and the patched VIN).

## Examples
```bash
# 10 RGB frames at 1080p
lc-a7z cam -c1 -C10 -s width=1920,height=1080,pixelformat=BGR888 --file=frame-#.ppm --metadata
# libcamera -> hardware H.264 (the software ISP crops 2 px per side, so 1920 becomes 1916; scale back for the encoder)
lc-a7z gst-launch-1.0 -e libcamerasrc ! video/x-raw,format=RGBA,width=1916,height=1080 ! queue ! videoconvert ! videoscale \
   ! video/x-raw,width=1920,height=1080,format=NV12 ! identity drop-allocation=true ! queue ! omxh264videoenc \
   ! h264parse ! mp4mux ! filesink location=clip.mp4
```

## Limitations (honest)
* **Software ISP:** debayering, white balance, colour matrix and gamma run on the CPU. Quality is "good webcam", below the
  Pi's hardware ISP; no denoise, no lens-shading correction, no sharpening. Tuning: `libcamera/imx519_mipi.yaml`
  (black level, Raspberry Pi colour matrices at 60 % strength). Auto white balance is grey-world and can be off in
  strongly coloured rooms.
* **Auto exposure prefers long exposures:** in a dim room it goes to ~1 s exposures (about 1 fps) unless the application
  limits `FrameDurationLimits`. `cam` does not limit it.
* **Controls:** the patched soft IPA now also exposes `AeEnable`, `ExposureTime` (microseconds) and `AnalogueGain`, verified
  from Python (`cam.create_request().set_control(...)`); the values are read back correctly in the request metadata.
  Autofocus (`LensPosition`, `AfMode`) is still not available through libcamera; use `a7zcam` / V4L2 for that.
* **Frame rate: partially fixed (multi-threaded demosaic), still not fully caught up.** Root cause (measured directly
  with a temporary per-call timer): the single-threaded demosaic + colour-matrix + gamma pipeline cost 26-39 us/frame
  at 1080p (average ~32 ms), more than the sensor's own 20.45 ms frame time, so the VIN had to skip frames to let the
  software catch up - always at least 2x (41 ms) or worse. **Fix applied:** the demosaic now splits the image into a
  band per CPU core and runs the bands concurrently (`DebayerCpu::process2Threaded()` in `debayer_cpu.cpp`); measured
  cost dropped to about 16-30 ms/frame (average ~24 ms), and some runs now hit the sensor's true native rate exactly
  (20.4 ms/frame, zero skipped frames) - something the single-threaded path could never do. It is not yet consistent:
  other runs still show 2x (41 ms) because creating and joining a fresh set of threads every single frame has real,
  variable OS overhead on this hardware, which sometimes eats the whole gain. A persistent thread pool (threads
  created once, reused every frame) would likely make the full-rate result consistent instead of occasional; not
  attempted here - safe to extend by someone with time for careful testing (see validation notes below). Stats
  (`SwStatsCpu::processLine0`) are shared across all bands through one mutex, so AE/AWB accuracy is unaffected.
  Auto-exposure mode uses the same mechanism and is not meaningfully different.
* **How this was validated before being trusted:** an intermittent crash during initial development (about 1 in 20
  runs) was taken seriously rather than patched over blind. It was re-tested under ThreadSanitizer (ARM64,
  preload `libtsan.so.2` before Python - needed to avoid a static-TLS loader error) across 36 runs (~900 threaded
  demosaic calls): zero races involving the new code (the only race TSan found is pre-existing in libcamera's own
  `Camera::acquire()`/`Semaphore` shutdown path, unrelated). The production build was then run 30 more times (25
  frames each, 750 more calls): zero crashes. Regression-tested against the standard V4L2/`a7zcam` paths (still,
  video, 20-cycle open/close soak) with no change in behaviour, since this only touches the libcamera software-ISP
  code path.
* **Fixed (previously crashed on camera stop/restart):** a second `start()`/`stop()` cycle in the same process used to
  abort or crash the kernel. Root causes: `IPASoftSimple::processStats` read sensor controls unconditionally even when
  none were available yet after a reset; `SimpleCameraData::imageBufferReady`/`conversionOutputDone` dereferenced a
  stale (freed) `Request` from a cancelled buffer of an *earlier* session; and a request whose software-ISP output was
  still in flight when `stop()` ran never got completed, which tripped libcamera's own `queuedRequests_.empty()`
  assertion on the *next* `stop()`. Verified with 11 process invocations (33 start/stop cycles total, 3 per invocation) across a reboot, zero crashes.
* Output widths are 4 px narrower than the sensor mode (software ISP border).
* Only tested with libcamera v0.5.2.

## What had to be changed (for reviewers / porters)
Kernel (`patches/vin_core_fixes.patch`, `driver/imx519_mipi.c`):
* sensor: standard `PIXEL_RATE`, `HBLANK` (read-only), `VBLANK` and `ANALOGUE_GAIN` controls with per-mode ranges;
  `EXPOSURE` now in lines (V4L2 standard; it was lines x 16, the Allwinner ISP convention).
* VIN `vidioc_querycap` no longer overwrites the V4L2 version with the BSP's own "1.1.0" (libcamera rejects < 5.0).
* VIN `ENUM_FRAMESIZES` returns one *discrete* size per sensor mode and `-EINVAL` (not `-1`) at the end of the list.

libcamera (`libcamera/0001-libcamera-v0.5.2-sunxi-vin-support.patch`, applies to tag v0.5.2):
* simple pipeline handler: matches the `sunxi-vin-media` media device with the software ISP; for it, only the sensor and
  the video node are configured (the VIN programs its own CSI/MIPI/ISP sub-devices), `VIDIOC_S_INPUT` 0 is issued, only the
  sensor's own Bayer format is offered (the VIN lists every Bayer order and depth), capture buffers are queued *before*
  `STREAMON` (the VIN dereferences the first queued buffer inside STREAMON and oopses on an empty queue), and frame-start
  events are not used.
* the capture-node search requires a real video device (the VIN's `sunxi_mipi` sub-device claims the I/O function).
* `V4L2VideoDevice`: `setInput()`; duplicate pixel formats (the VIN lists YUYV several times) are skipped instead of
  discarding the whole list.
* software ISP buffers come from the system DMA heap (the A733's CMA heap is only 16 MB).
* `CameraSensorHelper` for `imx519` / `imx519_mipi` (gain = 1024 / (1024 - code)).
