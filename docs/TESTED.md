# Tested setups

Only the first row has been tested by the author. Everything else is unknown until someone reports it. If you try
this on another setup, please open an issue with the **"Tested setup report"** template (it takes two minutes) and
paste the output of `scripts/collect-info.sh`.

| Board | OS / kernel | Camera module | Result | Reported by |
|---|---|---|---|---|
| Radxa Cubie A7Z (2 GB) | Radxa Debian 13 "trixie" CLI (rsdk-t6), kernel 6.6.98-5-aw2511, BSP tag `cubie-aiot-v1.5.0` | Waveshare IMX519-78 16 MP AF, Radxa cable AC008-270 on connector J5 | **Works**: all five modes up to 4656x3496, exposure to 15 s measured (hardware to ~118 s), gain to 64x, autofocus, DNG, live view; 60/60 open-stream-close cycles | author |
| Radxa Cubie A7A / A7S | - | IMX519 | **Not tested.** The overlay is very likely close (the J5-style csi1/mipi1 wiring came from the A7A overlays) but the connector pins may differ | - |
| Radxa Cubie A7Z | other kernels / newer Radxa images | IMX519 | **Not tested.** `install.sh` refuses other kernels unless you pass `--force` | - |
| Radxa Cubie A7Z | any | Arducam IMX519 (non-Waveshare, different oscillator / lens motor) | **Not tested.** The lens motor driver assumes an AK7375 at I2C 0x0c | - |
| any | any | IMX586 | **Not supported** (no driver) | - |

## What "works" means here
Frames of the correct size and content reach user space as RAW10 RGGB, exposure/gain/focus controls act on the
image, no kernel errors in `dmesg`, and `a7zcam still ... --af` returns a sharp picture of a lit scene.
Colour is not calibrated against a chart; there is no hardware ISP.
