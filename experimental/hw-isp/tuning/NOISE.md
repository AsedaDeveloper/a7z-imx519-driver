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
* One scene, one session, one module.
* Not done for B14: lens shading, colour matrix per illuminant, the tone curve, and noise reduction strength per ISO (the dynamic per-ISO block is not decoded).

## Dark frames (lens covered, Oct 28, 30 ms)
| gain | x | mean Y | max-ish: min Y | temporal std |
|---|---|---|---|---|
| 16-48 | 1-3 | 0.0 | 0 | 0.00 |
| 64-256 | 4-16 | 0.0 | 0 | 0.03-0.36 (a few pixels flicker above 0) |
| 384 | 24 | **73.1** | 8.4 | 7.1 dark / 46.7 mid |
| 512 | 32 | **173.3** | 70.6 | 11.3 |

Up to 16x the ISP's black level sits at or above the sensor's dark level: the output is clamped to 0, so **no black-level figure can be read from the output** (the pedestal is hidden, not zero).
At 24x and 32x a covered lens gives a bright, noisy image. That is not light (16x is black), so the digital-gain path above 16x lifts a pedestal or fixed pattern far above the clamp, and the picture is unusable.
**Unconfirmed (Oct 28): two later reruns of 16x-24x could not reproduce a clean dark baseline (one was uncovered, one showed a mean of 24 at 16x, so light was reaching the sensor), and the 24x/32x pedestal above may have been light or the cover shifting during the run. Treat it as an open question until a run reads 0.0 at 16x and then continues up.** Cause not isolated: a missing black-level subtraction in the digital-gain range, or a gain-dependent offset in the param file, are both candidates. Dark frames at 20-23x would place the threshold.
