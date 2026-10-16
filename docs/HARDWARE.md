# Hardware

## Parts
| Part | Notes |
|---|---|
| Radxa Cubie A7Z | uses the 31-pin 0.3 mm camera connector `J5` |
| IMX519 module | Waveshare "IMX519-78 16MP AF Camera" (tested), Arducam IMX519 AF; **15-pin 1.0 mm** FFC socket |
| Radxa camera cable **AC008** | 31-pin 0.3 mm → 15-pin 1.0 mm, *same side*. A Raspberry Pi Zero (22-pin) cable does **not** fit. |

## Orientation (important)
Insert/latch with the board **powered off**. A reversed or mis-seated cable can short the supply pins: the author's
board refused to power up until the cable was removed. If the sensor ID is not found after a reseat, power off, reseat
both ends fully (contacts toward the board at the A7Z end), power on.

## Signals (from the A7Z v1.11 schematic, sheet 10)
| J5 pin | Net | Use here |
|---|---|---|
| 24 | CM-PDN0 = PE8 | module enable (high = power on). Drives the module's regulators; 1.8 V level works. |
| 28 | RESET = PE9 | not needed by this module |
| 25 / 26 | SCL / SDA = TWI3 (PE3 / PE4) | I2C, **must run at 100 kHz** |
| 20 | MCSI-MCLK = PE5 | camera clock (the module has its own oscillator; harmless) |
| 27, 23 | PDN1, CAM1-CLK | not connected on the A7Z |
| 15-16 / 12-13 / 18-19 | MCSIB D0 / D1 / clock | MIPI CSI-2, 2 lanes used (pins 2-6 carry the unused lanes D2/D3 for 4-lane cameras) |
| 29-30 / 31-32 | 3V3 / 5V | module supply (3V3_CAM1 via DCDC1) |

The 15-pin module side follows the Raspberry Pi 15-pin pinout (pin 11 = CAM_GPIO = enable, 12 = CAM_IO1, 13 = SCL,
14 = SDA, 15 = 3V3); Radxa's cable reverses the order so that pin 11 lands on PDN0.

I2C devices on the module (bus TWI3): sensor `0x1a`, lens motor `0x0c` (AK7375). Both ACK only while PE8 is high.
