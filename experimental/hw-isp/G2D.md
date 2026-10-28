# 2D engine (G2D), checklist B11

`/dev/g2d` (5440000.g2d, driver `g2d_sunxi`, "rcq" variant, hardware version 0x10112114) works from userspace on dma-buf fds from `/dev/dma_heap/system`.
The `reserved` heap needs root. Header: `/usr/include/bsp/linux/sunxi-g2d.h` (include `<stdbool.h>` first).

## Why every earlier attempt "wrote nothing"
`g2d_image_enh.bbuff` must be **1** on both layers. With 0 the layer is the constant `color` value, not the buffer: the source read as 0, so the blit returned 0 (success) and wrote black.
A solid `G2D_CMD_FILLRECT_H` always worked, which is what showed the hardware, IOMMU and dma-buf path were fine. Setting `align[0]` to a byte stride makes the engine fault (`G2D irq pending flag timeout`, invalid address); leave `align` at 0.

## Measured (`userspace/g2dprobe.c`, `g2dnv12.c`, `g2dtest.c`)
| operation | result |
|---|---|
| ARGB 1920x1080 -> 640x360 `G2D_CMD_BITBLT_H` | smooth gradient: mean error 0.3, max 1 (of 255); 9.6 ms/call wall, ~1.5 ms/call CPU |
| ARGB 3840x2160 -> 1920x1080 | 35 ms/call: time follows source pixels (~210-250 Mpx/s), not bytes |
| Y8 1920x1080 -> 640x360 | writes every pixel; 8.5 ms/call |
| NV12 -> ARGB, 1920x1080 -> 640x360 (colour conversion + scale) | centre pixel 0xe3583b vs 0xe5593d expected (BT.601 full range), within 2 levels; format `G2D_FORMAT_YUV420UVC_V1U1V0U0` is NV12 (U first in memory) |
| 90 degree rotation, `flag_h = G2D_ROT_90`, ARGB 640x360 -> 360x640 | the four corners of a gradient land exactly where a clockwise turn puts them |
| NV12 scaled to 640x360 and rotated in one call | partial garbage (172980 of 230400 pixels written); do rotation as a separate step |
| `G2D_CMD_CREATE_TASK` / `TASK_APPLY` / `MIXER_TASK` | not working with the guessed argument shapes ("Null pointer" / "Fail to find mixer task" in dmesg); the legacy `BITBLT_H` path is enough |

## In use: the preview downscale (`A7Z_G2D=1`, Oct 28)
`a7zcap` can now do the `A7Z_STREAM_D=n` preview downscale on the G2D. The three planes of each ISP buffer are exported with `VIDIOC_EXPBUF` (the vin driver supports it) and each is scaled as an 8-bit plane
(`G2D_FORMAT_Y8`, 1920x1080 -> 640x360 for Y, 960x540 -> 320x180 for U and V) into dma-heap buffers that go straight to the output, so the CPU never reads the uncached capture memory.
Off by default, and not used with `A7Z_SWAWB` (that edits the planes on the CPU). Falls back to the CPU box filter if `/dev/g2d` or the export is unavailable.

Measured on the board at 1080p30, `A7Z_STREAM_D=3`, 600-700 frames (`A7Z_G2D_STAT=1` times both paths on the same buffers, every 50th frame for the CPU one):
| | CPU per frame | wall per frame |
|---|---|---|
| G2D, three planes | **2.0 ms** | 13.1 ms (the ioctl blocks until the engine is done) |
| CPU box filter | **12.8 ms** | about the same |

So the saving is about **10.8 ms of CPU per frame**, four times the 2-3 ms estimated before it was built (that figure only counted the scaling, not the copy out of uncached memory). Wall time does not improve.
Y plane against the CPU box filter: mean absolute difference 0.65 of 255, maximum 41 (at sharp edges; the engine's filter is not a box average). A frame from the G2D output has the right colours and geometry.
Frame pacing: **700 frames at 30 fps with 0 sequence drops** (one run). The same run with the `A7Z_G2D_STAT` sampler on stalled four times (one for about a second, 68 drops) because the sampler itself reads uncached memory on the CPU; do not use the stats flag in service.

## Not done
Rotation and colour conversion on the G2D are measured above but nothing uses them. The service does not enable `A7Z_G2D` (the 1080p30 loopback path never downscales).
