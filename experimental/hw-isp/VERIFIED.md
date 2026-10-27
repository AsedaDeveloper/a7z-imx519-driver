# What is verified, and what is not

One page for the question "can I rely on this?". Every row is something measured on the board; the detail and the caveats live in the linked document. Anything
not listed here has not been measured. The running tally is in [CHECKLIST.md](CHECKLIST.md) (`tools/progress.sh`).

## Relied on day to day
| Claim | Evidence | Where |
|---|---|---|
| The camera is an ordinary V4L2 device | `/dev/video9`, YU12 1080p30; `v4l2-ctl`, ffmpeg, GStreamer, OpenCV and a dependency-free Python reader all read it | [V4L2_NODE.md](V4L2_NODE.md) |
| It comes up by itself after a reboot | two reboots, service active, OpenCV 15/15 frames, nothing typed | [BOOT.md](BOOT.md) |
| It keeps running | 9.6 min at 1080p30: 17,900 frames, median and minimum 30.0 fps, 0 dropped across 8 refocuses; watchdog recovers a wedged encoder or a stalled stream | [STABILITY.md](STABILITY.md) |
| No sudo needed | reading the node, `a7zfocus` and a hand-run `a7zcap` all verified as an ordinary user | [BOOT.md](BOOT.md) |
| Autofocus holds a moving subject | settle-wait and futile-refocus guards; a 1.5 dioptre knock is recovered in 1.8-2.0 s, 7/7 at full resolution | [CHECKLIST.md](CHECKLIST.md) A3, A4 |
| Every frame carries its exposure, gain and lens position | 90/90 frames matched; recorded exposure correlates 1.000 with frame brightness | [METADATA.md](METADATA.md) |
| One command says whether it is healthy | `a7zstatus`: service, drops, rate, autofocus, image, sensor, MIPI link, temperatures | [STABILITY.md](STABILITY.md) |

## Measured, with the limits stated
| Claim | Measured | Limit |
|---|---|---|
| Hardware H.264/H.265/JPEG encoding | 1088p at ~110 / ~127 / ~80 fps | nothing above 1080p encodes, on the vendor demo either — [ENCODER.md](ENCODER.md) |
| All seven sensor modes work at their rated rate, switchable without a reload | 720p 84.7, 1080p 60.0, 16 MP 15.1; 5/5 switches | the fast modes need `A7Z_MAXEXP`, or auto exposure pins them to ~33 fps — [MODES.md](MODES.md) |
| The MIPI link has a margin, and the receiver reports it | `ecc_warn` latches exactly where the picture degrades; the 1296 default already latches `chksum_err` | one module, one cable; no driver-side handling — [LINK.md](LINK.md) |
| Frame timestamps are usable for logs | one `CLOCK_MONOTONIC` timebase; capture jitter +/-1.1 ms | the board's wall clock is not anchored — [TIMING.md](TIMING.md) |
| Nothing overheats at full load | 15 min at 16 MP: 51 C peak, rate flat, no camera throttling | open air with the fan at full; no closed case, no power figure — [THERMAL.md](THERMAL.md) |
| White balance | grey card in daylight: R/G 0.986, B/G 0.985 | warm and cool light not yet measured — CHECKLIST A2 |
| Highlights | ISP stage flag 24 cuts clipped pixels ~32% for free | not real highlight protection — [HIGHLIGHTS.md](HIGHLIGHTS.md) |

## Known not to work, or not done
| Thing | State |
|---|---|
| Raw Bayer capture on this path | the ISP path lists Bayer formats but streams none; raw lives in the main repo's bypass driver, which cannot run at the same time |
| Lens as a standard `FOCUS_ABSOLUTE` control | the actuator subdev exposes no controls; use `a7zfocus` — [CONTROLS.md](CONTROLS.md) |
| The ISP's other stages (tone mapping, DRC, noise reduction) | 32 stage flags swept; one helps and is deployed, the rest are worse or blank the frame. Four routes to naming them all failed, and the V4L2 user controls are inert — [HIGHLIGHTS.md](HIGHLIGHTS.md) |
| 2D engine (G2D) | `/dev/g2d` accepts a scale and writes nothing, in four variants |
| NPU | device node present, no userspace runtime installed |
| Translation of the ISP tuning to another module | everything here is one IMX519 on one board |
| Dual-camera sync | impossible on this board: one camera connector (J5), and the 16 MP mode already uses both ISP instances |

## The one flaw to know about
Roughly **one full-resolution (2-in-1) start in three comes up with the left ISP half empty** for the whole run, and auto exposure then blows out the right half.
`a7zcap` checks the first frames and exits with code **6** so the caller can start again; the cause is not found. 1080p is unaffected.
