#!/bin/bash
# Full 1080p30 live view: the board's hardware video engine encodes H.264, so the whole frame fits down the ~10 MB/s USB link (raw 1080p would be ~3 fps).
# Latency note: a raw H.264 pipe carries no timestamps, so ffplay guesses the rate, paces too slowly and the backlog grows without bound (~2.6 s after 20 s).
# -framerate 30 tells it the true rate; -sync ext + -framedrop keep it honest if a frame is late. Steady-state queue is then 0 KB.
# usage: lv_enc.sh [kbps]   board log in /tmp/enc1080.log
K=${1:-6000}
ssh -q -p 2222 radxa@127.0.0.1 "sudo rmmod sunxi_ve 2>/dev/null; sudo modprobe sunxi_ve; sleep 1; sudo env A7Z_FRESH=1 A7Z_ENC=h264 A7Z_ENC_H=1080 A7Z_ENC_KBPS=$K A7Z_STDOUT=1 A7Z_AF=1 A7Z_AF_DEBUG=1 A7Z_SKIP=1 A7Z_FPS=30 A7Z_MAXEXP=30000 ~/bin/a7zcap.enc 1920 1080 100000000 x 2>/tmp/enc1080.log" \
 | ffplay -loglevel error -fflags nobuffer -flags low_delay -framedrop -sync ext -probesize 32 -analyzeduration 0 -framerate 30 \
   -f h264 -x 1280 -y 720 -window_title "IMX519 1080p30, hardware H.264 ($K kbps), low latency" -
