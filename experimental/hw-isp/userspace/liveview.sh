#!/bin/bash
# Live view of the hardware-ISP picture on your computer (needs ffplay, camera-overlay boot, a7zcap on the board).
# usage: A7Z_HOST=radxa@<board> ./liveview.sh [frame_skip=2]       stop with Ctrl-C
HOST=${A7Z_HOST:?set A7Z_HOST=user@board (ssh target)}
SKIP=${1:-2}
ssh -o ConnectTimeout=8 "$HOST" "A7Z_AF=1 A7Z_STDOUT=1 A7Z_SKIP=$SKIP ~/bin/a7zcap 640 480 100000000 x 2>/dev/null" \
 | ffplay -loglevel error -fflags nobuffer -flags low_delay -f rawvideo -pixel_format yuv420p -video_size 640x480 -framerate 15 -window_title "A733 hardware ISP (IMX519)" -
