#!/bin/bash
# a7zstream.sh [W H FPS KBPS] — supervised camera stream on stdout. Restarts the pipeline when it stalls and reloads the video engine when the encoder wedges.
# a7zcap exits 3 = encoder wedged (needs sunxi_ve reload), 4 = no frames for 3 s (stream stalled). Any other non-zero exit is retried with backoff.
W=${1:-1920} H=${2:-1080} FPS=${3:-30} KBPS=${4:-6000}
LOG=${A7Z_LOG:-/tmp/a7zstream.log}
tries=0
exec 9>/tmp/a7zstream.lock
flock -n 9 || { echo "another supervisor is already running" >&2; exit 1; }   # two supervisors fight over /dev/video0 and both log "Device or resource busy"
pkill -9 -x a7zcap.enc 2>/dev/null; sleep 1        # -x (exact name): a -f pattern also matches the shell running this script

reload_ve() { rmmod sunxi_ve 2>/dev/null; modprobe sunxi_ve; sleep 1; }
echo "$(date -Is) supervisor start ${W}x${H} ${FPS}fps ${KBPS}kbps" >> "$LOG"
reload_ve
while :; do
  env A7Z_FRESH=1 A7Z_HEALTH=1 A7Z_ENC=h264 A7Z_ENC_H=$H A7Z_ENC_KBPS=$KBPS A7Z_STDOUT=1 A7Z_AF=1 A7Z_SKIP=1 \
      A7Z_FPS=$FPS A7Z_MAXEXP=30000 /home/radxa/bin/a7zcap.enc "$W" "$H" 100000000 x 2>>"$LOG"
  rc=$?
  tries=$((tries+1))
  case $rc in
    0)  echo "$(date -Is) clean exit" >> "$LOG"; break;;
    3)  echo "$(date -Is) RESTART $tries: encoder wedged (rc=3), reloading sunxi_ve" >> "$LOG"; reload_ve;;
    4)  echo "$(date -Is) RESTART $tries: stream stalled (rc=4)" >> "$LOG";;
    *)  echo "$(date -Is) RESTART $tries: exit rc=$rc" >> "$LOG"; sleep 2;;
  esac
  rm -f /mnt/isp0_2328_1748_30_0_imx519_mipi_ctx_saved.bin     # stale ISP state would be reloaded over the tuning table
  sleep 1
done
