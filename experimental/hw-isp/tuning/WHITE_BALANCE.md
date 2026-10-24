# White-balance light table (tuning_v5.json)

libisp picks its white balance from 9 reference lights (`awb_light_info`: r, g, b, ..., colour temperature, ...). The ISP's own units for r and b are not
ratio*256: with all 9 lights set to one value (r, b), the colour of a grey target moves roughly as r^-2.5 and b^-3 (measured, see below), so the neutral
values have to be measured, not computed from a colour-ratio table (e.g. Raspberry Pi's ct_curve; the shape agrees with it on the warm side).

## How the table was measured
`tools/wb_batch.sh` installs a table with all 9 lights equal, captures, and `tools/wb_read.py` reads the output colour. The saved ISP state
(`/mnt/isp0_<mode>_ctx_saved.bin`) must be removed before each capture or libisp reloads the old table over yours. The iPad screen (flat tints, dim) was used as a
set of white points because the real light could not be changed; a screen is narrow-band, so it is not a real illuminant, only a white point.

Neutral (r, b) per tint, in ISP units: 3537 K (208, 67), 4503 K (170, 87), 6504 K white = window daylight at the time (137, 113), 8625 K (128, 125),
13837 K (113, 138), 33645 K (96, 150). `tuning_v5.json` interpolates these in log space over the nine light colour temperatures and extrapolates the warm end.

## Result (auto white balance, colour error = distance of R/G and B/G from 1 in logs, 0 = neutral)
| scene | v4 table | v5 table |
|---|---|---|
| warm tint (~3000 K) | 1.31 | 0.34 |
| 3537 K | 0.64 | 0.16 |
| 4503 K | 0.22 | 0.14 |
| 6504 K white | 0.18 | 0.06 |
| 8625 K | 0.44 | 0.35 |
| 13837 K | 0.84 | 0.67 |
| 33645 K | 1.29 | 1.19 |

Mean over the cards: 0.70 -> 0.42. Converged by frame 39 in every run.

## Limits
* Above ~8000 K the picture stays blue: relabelling the coolest light to 12000 K with a matching (r, b) changed nothing, so some other AWB limit (gray-region bounds or
  light tolerance fields) stops libisp from choosing it. Not found yet.
* The CCMs (2800/4000/6500 K) were not touched.
* Measured with a screen and one window-daylight reference, not with real lamps; the window light drifted during the session (the grey patch read blue, then orange).
* The 1080p mode only; other modes have their own saved-state file.

## v6 (Oct 24 2026): global trim
Measured on an iPad colour chart that filled the frame (greys only, patch grid re-checked on every capture), v5 left a mild, steady cool cast: grey R/G 0.96, B/G 1.07-1.13.
`tuning_v6.json` scales every light_rb entry (r x0.88, b x1.13): grey R/G 0.98-1.00, B/G 1.00-1.04. Red responds weakly to the table, blue strongly; more than ~x0.80 / x1.15 overshoots.
A neutral-preserving 3x3 CCM fitted on top of v6 is ~identity (diag 1.02/1.0/1.03), so the CCMs are left alone.
Dead ends: the r_gain/b_gain fields of the EXP_GAIN ioctl do not change the image; 3a ints 625/626 are not a final gain trim; the software preview AWB (`A7Z_SWAWB=1` in a7zcap) locks onto a large coloured wall, so it is off by default.
Measured with an uncalibrated screen under one kind of light; check under daylight/lamps before trusting it elsewhere.
