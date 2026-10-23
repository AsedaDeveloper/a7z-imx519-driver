#!/bin/bash
# Live view of the full-resolution 2-in-1 capture, with autofocus. The sensor and ISP run at full size; the preview sent over the link is decimated.
# usage: A7Z_HOST=radxa@board [A7Z_SSH_OPTS="-p PORT"] ./liveview_full.sh [full|uhd] [D] [skip]
#   full : 4656x3496 (16.3 MP) at 15 fps, preview 1/4 -> 1164x874   (default)
#   uhd  : 3840x2160 at 18 fps, preview 1/3 -> 1280x720
# needs a7zdual in ~/bin on the board, and ffplay on this computer. If it stutters, use a larger D (6) or skip=2.
# The first ~10 s are frozen while autofocus scans the lens. Stop with Ctrl-C.
HOST=${A7Z_HOST:?set A7Z_HOST=user@board (ssh target)}
MODE=${1:-full}
case $MODE in
  uhd)  SW=3840; SH=2160; FPS=18; D=${2:-3} ;;
  *)    SW=4656; SH=3496; FPS=15; D=${2:-4} ;;
esac
SKIP=${3:-1}; W=$((SW/D/2*2)); H=$((SH/D/2*2))
# shellcheck disable=SC2086
ssh $A7Z_SSH_OPTS "$HOST" "A7Z_MERGE=1 A7Z_FPS=$FPS A7Z_AF=1 A7Z_STREAM=$D A7Z_STREAM_SKIP=$SKIP ~/bin/a7zdual $SW $SH 100000000 x 2>/dev/null" \
 | ffplay -loglevel error -fflags nobuffer -flags low_delay -f rawvideo -pixel_format yuv420p -video_size ${W}x${H} -framerate $FPS -window_title "IMX519 $MODE (${SW}x${SH}) preview 1/$D, $FPS fps, AF" -
