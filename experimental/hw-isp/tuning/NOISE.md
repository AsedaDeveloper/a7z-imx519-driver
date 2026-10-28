# Noise against gain (checklist B14, partial)

`tools/noise_profile.py` pins the exposure and steps the **sensor gain** (`gain N` in the control file, 16 = 1x), holds the lens, and measures the ISP's output over a static scene:
40 frames per gain, per-pixel temporal standard deviation on a 3000-pixel grid, over pixels whose mean is in the mid-tones (20..230).

The control-file verbs are new: `gain N` and `exp N` (microseconds) pin what `a7zcap` sends to the sensor whatever libisp asks for, and `free` gives it back (CONTROLS.md). The file's modification
time has one-second resolution, so leave at least a second between writes. The param file's own `ae_forced` / `isp_test_gain` fields were tried first and do nothing.

## Measured (Oct 28, 30 ms exposure, dim indoor scene, lens held)
| gain | x | mean Y | temporal std, mid-tones |
|---|---|---|---|
| 16 | 1.0 | 22.5 | 0.40 |
| 32 | 2.0 | 44.5 | 0.44 |
| 64 | 4.0 | 60.2 | 0.50 |
| 128 | 8.0 | 106.2 | 0.50 |
| 192 | 12.0 | 135.4 | 0.49 |
| 256 | 16.0 | 155.5 | 0.45 |
| 384 | 24.0 | 183.4 | **1.61** |
| 512 | 32.0 | 201.0 | **3.21** |

Up to 16x the temporal noise that survives the ISP is under 0.7 of 255; at 24x and 32x it jumps 3.5x and 7x. The sensor's analogue gain ends at 16x and anything above it is digital gain, which is where it knees.
In practice: stay at or below 16x (gain code 256) unless the frame is worth a noisy picture.

## What this does not show
* **It is temporal noise, on a static scene, with the ISP's 3D noise reduction on.** That stage averages over time, so the low-gain end looks better than the spatial noise of a single frame (`ae_check`-style
  high-pass metrics are the other view). Do not read 0.4 as the sensor's read noise.
* The mean luma rises with gain because the exposure is fixed; the rows are not at equal brightness.
* One scene, one session, one module. No dark-frame (lens covered) run yet, so **no black-level figure per gain**; the dark column is empty for the same reason.
* Not done for B14: lens shading, colour matrix per illuminant, the tone curve, and noise reduction strength per ISO (the dynamic per-ISO block is not decoded).
