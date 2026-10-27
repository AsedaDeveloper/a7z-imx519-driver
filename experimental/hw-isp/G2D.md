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

## Not done
Nothing in the pipeline uses it yet. The only CPU box downscale is the optional USB preview (`A7Z_STREAM_D`); using the G2D there means handing it the ISP output buffers as dma-buf fds, for a saving of ~2-3 ms CPU per frame.
