# Sensor modes through the hardware ISP (checklist B16)

The driver declares seven modes. They were exercised on the raw/bypass path; this is what they do through the **hardware ISP**, which is the path this folder uses.
A mode is addressed as (width, height, frame rate): `S_PARM`'s denominator picks the sensor mode whose `fps_fixed` matches, and the VIPP scaler then resizes to
the `S_FMT` size. Widths above 3264 need the 2-in-1 path (`A7Z_MERGE=1`).

`tools/mode_matrix.py` produces the table; `--switch` adds the mode-change test.

| asked | sensor mode | rated | with libisp's auto exposure | with `A7Z_MAXEXP=8000` |
|---|---|---|---|---|
| 1280x720 @85 | 1280x720 | 85 | **32.8 fps** | **84.7 fps** |
| 1920x1080 @60 | 1920x1080 | 60 | **32.9 fps** | **60.0 fps** |
| 2328x1748 @30 | 2328x1748 (binned) | 30 | 30.0 fps | 30.0 fps |
| 3264x2448 @13 | 3264x2448 crop | 13 | 13.4 fps | 13.4 fps |
| 3840x2160 @18 | 3840x2160 crop | 18 | 18.0 fps | 18.0 fps |
| 4656x3496 @9 | full, stock clocks | 9 | 9.4 fps | 9.4 fps |
| 4656x3496 @15 | full, 2-in-1 | 15 | 15.1 fps | 15.1 fps |

**All seven reach their rated rate.** No new mode was added here: the set already covers binned and high-frame-rate operation, and what was missing was evidence
that each one works through the ISP.

## The trap: exposure, not the mode, caps the fast modes
A frame cannot be shorter than its exposure. In ordinary indoor light libisp asks for about 30 ms, which pins **any** mode above ~33 fps to ~33 fps — so 720p85 and
1080p60 look like modes that cannot reach their rated rate. They can: cap the exposure (`A7Z_MAXEXP=8000`, 8 ms) and both jump to 84.7 and 60.0 fps. Nothing in the
mode or the link is at fault, and the cap trades exposure for gain, so a dim scene gets noisier rather than slower.

## Switching modes needs no driver reload
Five transitions back to back, one process per mode, nothing unloaded in between, including in and out of the 16 MP 2-in-1 mode:

```
1280x720 @85  ->  4656x3496 @15  ->  1920x1080 @60  ->  2328x1748 @30  ->  1280x720 @85
```
Every one delivered its rated rate. A switch still means stopping the stream and starting it again (`STREAMOFF`, `S_FMT`, `STREAMON`); there is no way to change
mode mid-stream, and the 2-in-1 starts carry their usual one-in-three empty-left-half risk (exit 6, just restart).

## What the service actually runs
`a7z-camera.service` asks for 1920x1080 at 30 fps. That selects the **2328x1748 binned mode**, and VIPP scales it to 1080p — confirmed in the kernel log
(`mode regs 2328x1748`). Asking for 1920x1080 at 60 instead would use the sensor's own 1080p mode. Which is better was not measured; binning collects more light
per output pixel, the 1080p mode gives a different field of view.

## On the per-mode image scores
`mode_matrix.py` also prints isolated outliers per megapixel. They vary widely by mode (4 at 3840x2160, ~2000 at 720p), but this is **not** evidence of damage:
the counts are repeatable run to run (720p 2095 then 2203; 1080p 954 then 1365; 3264 8973 then 8747), and no MIPI error bit latches in any of these modes
(`int_pd 0x04f`, see LINK.md). Random link corruption would not repeat. Each mode is a different crop of the array at whatever focus the lens happens to hold, so
the frames contain different amounts of fine detail, which is what the measure responds to. Compare scores only between captures of the same scene in the same mode.
