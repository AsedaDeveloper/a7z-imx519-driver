# Hardware video encoder (Allwinner video engine / "cedar") on the A7Z

Verified Oct 26 2026 on the vendor 5.15.147 kernel. The engine is exposed as `/dev/cedar_dev`; userspace API is `libvencoder` (`/usr/include/vencoder.h`).
This removes the preview bottleneck: raw 1080p is ~3 fps over the ~10 MB/s USB link, hardware H.264 at 6 Mbit/s is 1080p30 with room to spare.

## Measured (synthetic frames, `userspace/enctest.c`)
| codec | size | encode time | throughput |
|---|---|---|---|
| H.264 | 1280x720 | 3.3 ms/frame | ~223 fps |
| H.264 | 1920x1088 | 7.1 ms/frame | ~110 fps |
| H.265 | 1920x1088 | 5.8 ms/frame | ~127 fps |
| JPEG  | 1920x1088 | 9.8 ms/frame | ~80 fps (188 KB/frame at default quality) |

Live camera through the ISP (`a7zcap`, `A7Z_ENC=h264`): 300 frames of 1080p in 12.5 s including autofocus and start-up, 7.2 MB at 6 Mbit/s. Output validated with ffprobe/ffmpeg (h264 Main 1920x1080, 300 frames decoded).

## Limits found
* **Nothing above 1080p encodes.** 2320x1744 and 3840x2160 time out ("h264 encoder wait interrupt overtime"); 4656x3496 fails at `VideoEncInit`. The vendor's own `demoVencoder` fails identically, so this is the hardware/driver ceiling, not our code. Full-resolution stills therefore stay JPEG-per-frame at <=1080p or raw.
* **Height must be a multiple of 16** when driving the API directly: 1920x1080 times out, 1920x1088 works. The capture buffer is already 1088 rows, so 1080p costs nothing: encode 1088 and set `nDstHeight` to 1080 (`A7Z_ENC_H=1080`) so decoders crop the 8 padding rows.
* **A failed encode wedges the engine.** Every later encode then times out, including sizes that worked before. Recovery: `sudo rmmod sunxi_ve && sudo modprobe sunxi_ve`. (Relevant to checklist B5: a stall watchdog should do this automatically.)

## Three things that must be right or the engine looks broken
1. `CdcMemOpen(cfg.memops)` after `MemAdapterGetOpsS()`. Without it the engine faults on unmapped addresses — the kernel logs `Bug is in VE_ENC module, invalid address: ...` and every frame times out.
2. `bEncH264Nalu = 0` gives Annex-B with inline SPS/PPS. With `1` the output is length-prefixed and no decoder will open it.
3. H.264 SPS/PPS only exist **after** the first frame is encoded (`VENC_IndexParamH264SPSPPS` returns a 0xff-filled buffer before that). H.265 (`VENC_IndexParamH265Header`) is filled earlier, but fetching it after the first frame works for both.

## Use
```sh
# board: live H.264 to stdout, autofocus and exposure cap as usual
A7Z_ENC=h264 A7Z_ENC_H=1080 A7Z_ENC_KBPS=6000 A7Z_STDOUT=1 A7Z_AF=1 A7Z_FPS=30 A7Z_MAXEXP=30000 a7zcap 1920 1080 100000000 x
```
`A7Z_ENC` takes `h264`, `h265` or `jpeg`; `A7Z_ENC_KBPS` sets the bitrate (default 6000). `userspace/enctest.c` is the standalone encoder test (no camera).

## Latency
A raw H.264 pipe carries no timestamps, so a player guesses the frame rate. ffplay's guess paces slightly slow, it falls behind every second and the backlog grows
without bound: measured **~1950 KB queued (~2.6 s at 6 Mbit/s) after 20 s**, with no frames dropped. Giving it the true rate fixes it — steady-state queue 0 KB:

    ffplay -fflags nobuffer -flags low_delay -framedrop -sync ext -probesize 32 -analyzeduration 0 -framerate 30 -f h264 -

Frames leave the board at a steady 30.0 fps (median inter-frame gap 33.3 ms), so the link and the board add no growing delay. `A7Z_FRESH=1` additionally skips to the
newest queued capture buffer if the driver got ahead (measured 0 stale frames at 1080p30, so it is insurance, not a fix). `tools/lv_enc.sh` has all of it.
