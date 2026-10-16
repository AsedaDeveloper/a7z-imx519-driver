# How it works - and the traps (read before porting to another sensor)

This is the knowledge that took the longest to find. Each item cost hours; each one looks like "the camera
doesn't work" with no error message.

## The stack

```
IMX519 module ──15-pin FFC──> AC008 cable ──31-pin──> J5 ──MIPI CSI-2 (2 lanes)──> MCSIB pads
                                    I2C (TWI3, PE3/PE4), PE8 = enable/standby, PE9 = reset
SoC:  combo-PHY → mipi1 → csi1 → TDM/ISP(bypassed) → VIPP → DMA → /dev/video0   (Allwinner "VIN", not V4L2 core)
Kernel modules:   imx519_mipi (sensor)   ak7375_act (lens)   vin_io + vin_v4l2 (Allwinner VIN, patched)
```

## Trap 1 - the wrong receiver instance (frames: none at all)
The Radxa stock overlay `cubie-a7z-radxa-camera-8m-219-cam2` selects `csi0/mipi0` and I2C bus TWI4. Those are for a
different physical port. The A7Z's single 31-pin connector `J5` is wired to **MCSIB** pads: use the **A7A/A7S style**
overlay: `csi1`, `mipi1`, `vinc*_csi_sel = vinc*_mipi_sel = 1`, **TWI3 (PE3/PE4)**, PE8 (power-down / enable),
PE9 (reset), `mclk_id = 1`. With the wrong instance the receiver counts zero packets and nothing in `dmesg` says why.
(`overlay/imx519-j5.dts` is correct.) The MIPI "PHY offset" and termination parameters make no difference.

## Trap 2 - I2C must be 100 kHz (frames half empty, scrambled)
The module silently **drops register writes at 400 kHz** (the BSP default) while still ACKing them, and the VIN's CCI
layer reports success. The sensor then stays in whatever mode it was in: wrong line length, frames that are ~50 %
empty or sheared. 100 kHz (what the Raspberry Pi uses) works. It is set in the overlay (`clock-frequency = <0x186a0>`).
Debug trick that found it: read the CSI-2 packet header from the receiver (see below) to see the real line length.

## Trap 3 - the module's I2C does not read back
Every register except the chip ID reads **0** (even while streaming; the Raspberry Pi shows the same). The ID itself
is only returned for a 2-byte read at exactly 0x0016. You cannot verify writes by reading them back - verify by
effect (frame period = exposure, brightness = gain, packet length = mode).

## Trap 4 - the lens motor needs ONE 3-byte write
AK7375 at I2C 0x0c: wake it with `[0x02, 0x00]`, then write the position as **one transaction**
`[0x00, pos>>4, (pos&0xf)<<4]` (12-bit position). Two separate 2-byte writes are silently ignored. The Allwinner
actuator framework never calls `ACT_INIT`, so the driver wakes the motor itself before the first position write.
0 = far (infinity), 4095 = nearest. The VIN's `focus_absolute` control is 0..127; the driver maps it linearly.

## Trap 5 - the first frame after a mode change was truncated (frame length from the previous mode)
Bug in early versions of this driver: the frame-length variable was updated *after* the exposure registers were
written, so the first session in a new mode ran the old mode's `VTS` and the sensor output only ~half the lines.
Fixed; the symptom was "52 % of the frame is zero".

## Trap 6 - standard V4L2 tools crash the vendor kernel (fixed by `patches/`)
* `v4l2-ctl --list-ctrls` on `/dev/video0` *before* `--set-input` dereferences a NULL sensor pointer → kernel oops.
* `v4l2-ctl --set-ctrl=exposure=…` (or `gain`) crashes: the BSP marks every RAW sensor as ISP-driven and routes these
  controls to an ISP sub-device that does not exist when the ISP is unused.
* The BSP never re-applies controls set before `S_INPUT` (the call is commented out), and `v4l2-ctl` sets controls
  before selecting the input.
The patch fixes all three. It must be applied to **both** `vin_io` and `vin_v4l2` (the shipped binaries come from a
slightly different source revision: the patched `vin_v4l2` needs a symbol the shipped `vin_io` lacks).
Without the patch use only the sensor sub-device node (`/dev/v4l-subdev0`) for exposure and gain.

## Trap 7 - the VIN eats the first frame of every stream
Frame addresses are programmed one frame ahead, so the first completed frame is lost (delivered sequence numbers
start at 2). A one-shot capture costs ~2 exposures; use a persistent stream (`tools/a7zcam.py`, `Camera`).

## Trap 8 - exposure latency
Frame length applies at once, but the coarse integration time reaches the output **two frames later**. Only the
very first exposure (written before streaming starts, which the driver does last) is immediate.

## Trap 9 - the VIN refuses frames wider than 4224 px (fixed by the patch)
The Allwinner ISP and scaler sub-devices clamp the width to a hard-coded 4224 (`MAX_IN_WIDTH` in `sunxi_isp.c` and
`sunxi_scaler.c`), so a request for the sensor's full 4656 columns silently becomes 4224 and the right 432 columns are
lost. With the ISP bypassed the hardware has no such limit: the patch raises the clamps to 4800 and full-width raw
frames work (verified with the sensor's colour-bar test pattern: eight bars across all 4656 columns, no overflow).

## Useful debugging recipes
* **Real line length the sensor sends** (shows mode / I2C problems): the combo CSI port's packet-header register,
  `devmem` / `/dev/mem` at physical `0x5810000 + 0x151c` while streaming (bits 31:16 = word count in bytes, bits 5:0 =
  data type; RAW10 line = pixels × 1.25 bytes).
* **Clock lane present**: PHY frequency counter, `0x5810000 + 0x234`, bits 31:16 non-zero ⇒ the sensor drives its MIPI
  clock (bit rate = 1000 × VIN top clock(MHz) × 8 / count).
* Frame period = exposure: `v4l2-ctl --stream-count=N --verbose` prints `delta:` per frame.
* The sensor test pattern: write `0x0601 = 0x02` (colour bars) over I2C while streaming.

## Sensor facts
2 lanes, RAW10, 24 MHz module oscillator (the module has its own), link 408 MHz (816 Mbit/s per lane), pixel rate
426.667 Mpix/s, black level 64, full scale 1023, line time = `hts / 426.667 MHz` (14.13 µs at 1080p).
Gain: analog `code = 1024 - 1024/G` up to 16× (code 960), then digital 8.8 fixed point. Long exposure: register
`0x3100` shift (0-7) multiplies the exposure and frame length (15 s uses shift 5).
