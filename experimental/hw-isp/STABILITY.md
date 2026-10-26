# Keeping the stream alive (checklist B5 / B7)

`tools/a7zstream.sh` supervises `a7zcap` and restarts it when the pipeline fails. `a7zcap` exits with a code that says what went wrong, so the supervisor can
act instead of blindly retrying:

| exit | meaning | supervisor action |
|---|---|---|
| 3 | encoder wedged (5 consecutive `VideoEncodeOneFrame` failures) | reload `sunxi_ve`, restart |
| 4 | no frame for 3 s — capture stalled | restart |
| other non-zero | crash, kill, device busy | restart after a short wait |

`A7Z_HEALTH=1` makes `a7zcap` print one line a second: frame count, measured fps, **driver sequence gaps** (frames the VIN dropped) and frames skipped by
`A7Z_FRESH`. That is what the soak test parses.

## Verified
* **Process killed mid-stream** (`kill -9`): exactly one `RESTART ... rc=137`, capture running again and back at 30.0 fps within ~15 s.
* **Device busy**: with a stale instance holding `/dev/video0` the supervisor retried every second and recovered automatically the moment the device freed up.
* **Encoder wedge is per-process.** Wedging the engine from a *separate* process (an oversized encode) did not disturb a healthy running stream — it kept 30.0 fps
  throughout. The wedge seen while developing only affects the process that caused it, so exit 3 plus a module reload is the right scope.

## Two traps that cost time here
* `pkill -f a7zcap` **matches the shell running the command**, so a cleanup one-liner kills its own ssh session (exit 255) and leaves the real process alive.
  Use `pkill -x a7zcap.enc`. The supervisor does this on start.
* Two supervisors both grab `/dev/video0` and fill the log with `Device or resource busy`. The supervisor now takes an `flock` and refuses to start twice.

## Soak test
`tools/soak.sh MINUTES` runs the supervised stream and reports fps (min/median/max), driver frame drops, stale frames, autofocus runs and restarts.

### Result: 9.7 min at 1080p30 (Oct 26 2026)
| | |
|---|---|
| frames | 17621 |
| fps | median 30.0, p10 30.0, 576 of 581 one-second samples at 30 fps |
| restarts / error lines | 0 / 0 |
| stale frames skipped (`A7Z_FRESH`) | 0 |
| driver frame drops (VIN sequence gaps) | 228 |

**All 228 dropped frames are autofocus.** They fall in exactly 5 bursts of ~45 frames, each at an autofocus run (6 runs, 1.00 s each), and the fps sample for that
second drops to 2.4-8.6. Nothing is dropped at any other time. The cause is structural: `autofocus()` takes over the capture loop to drive the lens and score
frames, so for ~1.5 s nothing is encoded or streamed and the driver's queued buffers age out. Not a leak or an overrun — the stream is otherwise exactly 30.0 fps
for 9.7 minutes.

**To fix (not done):** encode and stream from inside the autofocus frame loop (`grab_sharp` already dequeues every frame — it could hand each one to the encoder),
so a refocus costs sharpness, not a 1.5 s hole in the video.
