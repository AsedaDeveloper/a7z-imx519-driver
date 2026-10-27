# Per-frame metadata (checklist B18)

libcamera hands every frame its exposure, gain and lens position. A plain V4L2 node cannot carry those, so the feeder publishes them on the side and a consumer
matches them to a frame **by timestamp**.

## What is recorded
One record per dequeued frame (`a7zcap`, `seq_track`):

| field | meaning |
|---|---|
| `ts_ns` | `CLOCK_MONOTONIC` when the frame was dequeued; the loopback stamps each buffer with the same clock when it is written, ~7 ms later (max 9 ms measured) |
| `seq` | ISP frame sequence number |
| `exp_us` | sensor exposure that actually applied **to this frame** |
| `gain` | sensor gain, 16 = 1x (`gain / 16` for the multiple) |
| `lens_code`, `lens_dpt` | lens position that applied to this frame (273 codes per dioptre) |
| `r_gain`, `b_gain` | the ISP's white-balance gains as sent to the driver |
| `flags` | bit 0 autofocus sweeping, bit 1 lens held (control file) |

"Applied to this frame" matters: a command issued after frame *s* shows up later. **Exposure, gain and colour gains take effect 4 frames later** (`A7Z_EXPLAT`, default 4);
the lens uses `A7Z_AF_LAT` (default 1). The 4 was measured at 1080p30 by alternating the exposure between full and half on every libisp command (`A7Z_EXPSTEP=1`) and fitting
log-luma against the commanded log(exposure x gain) at each lag: **0.999 at lag 4, between -0.5 and +0.3 at every other lag**. Checked the other way round on the finished
records: correlation with frame brightness is **1.000 at offset 0** and -0.76 to +0.08 at every other offset.

## Where it goes
* `A7Z_METARING=/run/a7zcam.meta` (the service sets it): a 1024-slot ring (~34 s at 30 fps), 40-byte records, world-readable. Header: magic `A7ZM`, version, slots, record size; u64 head count at byte 16.
* `A7Z_META=file.jsonl`: every record as one JSON line (for recordings).

## Using it
```python
from a7z_v4l2 import Camera
with Camera() as cam:
    y, u, v, meta = cam.read_with_meta()      # meta = None if no record is within 16 ms
    print(meta["exp_us"], meta["gain"] / 16, meta["lens_dpt"])
```
`a7zstatus` shows the newest record (exposure, gain, lens) and warns if it is more than a second old.

## Verified
Live service, as `radxa`: 90 of 90 frames matched. Test feeder across an autofocus sweep: 298 of 300 matched (the two misses are the first frames, before the ring has a record), 19
distinct lens positions covering 0 to 12 dpt, flags 0/1/2 all seen.

## Caveats
* The lens latency is the autofocus code's own measurement (1 frame at a steady frame rate), not re-measured at 30 fps; during sweeps the lens position may be off by a frame.
* `gain` is the sensor gain only; the ISP's digital gain is not in it.
* The 4-frame exposure latency was measured at 1080p30; it is a pipeline depth, so it is expected to hold at other rates but was not checked.
* Frames the stale-frame filter drops (`A7Z_FRESH`) also get records; they are never matched because a published frame's own record is always nearer.

## Does libisp's auto exposure cope with the latency? (B8)
`tools/ae_check.py`: static scene, 85 s, exposure x gain never moved. Torch at the lens switched on then off (one trial each way): on, luma 130 -> 214 and back to 133 in ~1.5 s with no overshoot; off, luma fell to 38, AE raised exposure x gain 10x in 6 frames, overshot to luma 168 (+30%) and settled to 131 in ~0.3 s. So it does not hunt; the one transient overshoot on dimming is what a loop that only partly allows for the 4-frame delay would show, but one trial cannot separate that from deliberate fast-attack tuning.
