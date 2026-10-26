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
