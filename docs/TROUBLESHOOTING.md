# Troubleshooting

| Symptom | Cause / fix |
|---|---|
| `dmesg`: "chip id register = 0x0000" / "not an target chip" | module not powered or cable reseated badly. Power off, reseat both ends. Check the overlay is active (`ls /boot/dtbo/*imx519*`). |
| no `/dev/video0` | `modprobe vin_v4l2` and read `dmesg`. "Unknown symbol sensor_get_clk": the patched `vin_v4l2` is installed without the matching patched `vin_io` (install both from the same build). |
| capture times out ("select timeout"), receiver sees no packets | wrong camera overlay (must be csi1/mipi1, TWI3) - see [HOW_IT_WORKS](HOW_IT_WORKS.md) trap 1 |
| frames ~50 % empty or sheared | I2C bus at 400 kHz (must be 100 kHz) or an old driver build (frame-length bug). Re-run `install.sh`. |
| kernel oops / board reboots when running `v4l2-ctl` | the VIN patch is not installed. Install it, or only use `/dev/v4l-subdev0` for exposure and gain. |
| `Device or resource busy` on `/dev/video0` | another program (live view, a stream) has the camera. Only one at a time. |
| `a7zcam`: "no frames from the camera" | the camera is busy, or the modules did not load: `lsmod`, `dmesg | grep -iE "vin|imx519"` |
| picture very dark in a lit room | exposure still at the default; run `a7zcam still` (auto exposure) or set `exposure` / `gain` |
| autofocus lands in the wrong place | needs a scene with detail and light; dark/featureless scenes have no contrast to measure |
| frame times differ between two runs | check that exposure and size were really the same (exposure changes apply 2 frames late); repeated measurements across reboots agree to 0.003 % |
| `v4l2-ctl --list-ctrls` prints "error 22 getting ctrl ... Visual Angle" | harmless: unsupported ISP-only controls |

## Collecting information for a bug report
```bash
bash scripts/collect-info.sh
```
Paste the whole output into the issue (it contains no passwords or Wi-Fi names). Other boards, kernels or modules: see [TESTED.md](TESTED.md).
