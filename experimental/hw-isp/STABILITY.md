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

**Fixed Oct 26.** `grab_sharp` now hands every frame it dequeues to the encoder (`stream_frame`), so the video continues through a sweep; the capture queue went
from 4 to 8 buffers (`A7Z_NBUF`) because scoring plus encoding costs ~25 ms a frame and a shallow queue turns any overrun into a drop.

**Correction to the measurement above.** Part of the original 228 was an artifact: drop accounting only tracked buffers the *main* loop dequeued, so frames consumed
by autofocus looked like sequence gaps. `seq_track()` is now called from both loops and the health counter reports every frame actually encoded.

### Re-run after the fix: 9.6 min at 1080p30
| | before | after |
|---|---|---|
| frames | 17621 | 17900 |
| fps (min / median) | 2.4 / 30.0 | **30.0 / 30.0** |
| one-second samples below 29 fps | 5 of 581 | **0 of 578** |
| driver frame drops | 228 | **0** |
| autofocus runs | 6 | 8 |
| restarts / errors | 0 / 0 | 0 / 0 |

Eight refocuses, no drop bursts, no dip below 30.0 fps.

## One status command, and a trap it found (checklist C11, partial)
`a7zstatus` (install to `/usr/local/bin`, needs no sudo) prints service state and restarts, the feeder's rate / dropped / stale counts, autofocus runs and lens position,
a live 30-frame read of `/dev/video9` (rate, mean luma, clipped %, black %, R/G and B/G of the whole frame), temperatures, CPU clocks, libisp error counts and the
supervisor's recent events. `--json` gives the same for scripts, `--no-probe` skips the frame read. Exit 0 healthy, 1 warning or still starting (first 30 s), 2 failed.

**Exposure, gain and lens position** now come from the per-frame metadata ring (B18, METADATA.md); the sensor subdev's own controls only ever show their defaults. **Not covered yet:**
MIPI link errors (needs B6), which is why C11 is not ticked.

**The trap.** While testing the failure path, the service looked healthy (`active`, no restarts) but published nothing: `a7zcap` logged `loopback S_FMT: Invalid argument`
and carried on with the loopback fd set to -1. Cause: a reader that opens `/dev/video9` while the feeder is not running (a camera app at boot, or the status probe itself)
leaves the node refusing the feeder's `S_FMT` until the module is reloaded. Fixed three ways, all verified by replaying the exact sequence (stop, reader touches the
idle node, start): `a7zcap` now exits **5** if the loopback cannot be set up, `a7zstream.sh` reloads `v4l2loopback` on exit 5 and once at supervisor start, and
`a7zstatus` reports the failure and never probes a node the feeder is not publishing to.
