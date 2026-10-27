# What a frame's timestamp is worth (checklist C9)

For the balloon and observatory logs the question is: given a frame, when did it happen, and can that be lined up with other instruments?
`tools/clock_check.py` answers it on this board.

## The clock
Frame timestamps are **`CLOCK_MONOTONIC`**, on both the sensor node and the loopback, and the per-frame metadata ring (METADATA.md) uses the same clock — so a
frame, its exposure/gain/lens record and anything else logged with `clock_gettime(CLOCK_MONOTONIC)` share one timebase with no conversion.

Converting to UTC is one subtraction: read both clocks back to back once per session and add the offset. Measured spread of that offset over a 300-frame run: **6.3 ms**.

```python
import time
off = time.clock_gettime_ns(time.CLOCK_REALTIME) - time.clock_gettime_ns(time.CLOCK_MONOTONIC)
utc_ns = frame_ts_ns + off
```

## How steady the frames are (1080p30)
| measured at | median interval | 90% of intervals | shortest | longest | gaps > 1.5x |
|---|---|---|---|---|---|
| capture, `/dev/video0` (metadata ring, 395 frames) | 33.342 ms (29.99 fps) | **+/- 1.1 ms** | 28.3 ms | 37.4 ms | 0 |
| loopback, `/dev/video9` (300 frames) | 33.393 ms (29.95 fps) | +/- 2.6 ms | **1.2 ms** | 37.4 ms | 0 |

The capture side is the good one. **The loopback roughly doubles the jitter and can deliver two frames almost back to back** (1.2 ms apart), because its timestamp
is when the feeder wrote the frame, not when the sensor produced it. For anything that cares about timing, take the time from the metadata ring (it is recorded at
dequeue from the sensor node), not from the loopback buffer.

## The catch: this board's wall clock is not anchored to anything
`timedatectl` reports **NTP not synchronised**, the board has no network of its own, and during this work its clock read **2026-10-22** while the real date was
2026-10-27 — five days out. An RTC device exists but is clearly not holding correct time. So:

* `CLOCK_MONOTONIC` is trustworthy for intervals and for lining up instruments on this board.
* `CLOCK_REALTIME` is an arbitrary number until something sets it. Any log stamped with it is unusable for correlating with the outside world.
* For the balloon and observatory: log the monotonic timestamp with every frame, plus **one externally-sourced anchor per session** (a GPS fix, a ground-station
  time message) recorded with its own monotonic timestamp. Everything else can then be placed in UTC afterwards from that single pair.

No anchored time source is wired up here — that belongs to the balloon's GPS, not to the camera. What the camera side now guarantees is a single, measured,
shared monotonic timebase and a known conversion.
