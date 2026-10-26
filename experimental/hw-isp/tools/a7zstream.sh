#!/bin/bash
# a7zstream.sh — supervised camera pipeline. Restarts a7zcap when it fails and reloads the video engine when the encoder wedges.
# Two modes:
#   A7Z_NODE=/dev/videoN   publish to that v4l2loopback node (what the systemd service uses)
#   unset                  write H.264 to stdout (for piping to a player)
# a7zcap exits 3 = encoder wedged (needs a sunxi_ve reload), 4 = no frames for 3 s, other = crash/kill/device busy.
W=${1:-1920} H=${2:-1080} FPS=${3:-30} KBPS=${4:-6000}
NODE=${A7Z_NODE:-}
LOG=${A7Z_LOG:-/tmp/a7zstream.log}
BIN=${BIN:-/home/radxa/bin/a7zcap.enc}
CTX=/mnt/isp0_2328_1748_30_0_imx519_mipi_ctx_saved.bin
tries=0

exec 9>/tmp/a7zstream.lock
flock -n 9 || { echo "another supervisor is already running" >&2; exit 1; }   # two supervisors fight over /dev/video0
pkill -9 -x a7zcap.enc 2>/dev/null; sleep 1        # -x (exact name): a -f pattern also matches the shell running this script

reload_ve() { rmmod sunxi_ve 2>/dev/null; modprobe sunxi_ve; sleep 1; }
echo "$(date -Is) supervisor start ${W}x${H} ${FPS}fps ${KBPS}kbps node=${NODE:-stdout}" >> "$LOG"
reload_ve
while :; do
  rm -f "$CTX"                                     # libisp reloads stale ISP state over the tuning table otherwise
  if [ -n "$NODE" ]; then
    env A7Z_LOOPBACK="$NODE" A7Z_FRESH=1 A7Z_HEALTH=1 A7Z_AF=1 A7Z_FPS=$FPS A7Z_MAXEXP=30000 \
        A7Z_EVERY=100000 "$BIN" "$W" "$H" 100000000 /tmp/cap/lb >>"$LOG" 2>&1
  else
    env A7Z_FRESH=1 A7Z_HEALTH=1 A7Z_ENC=h264 A7Z_ENC_H=$H A7Z_ENC_KBPS=$KBPS A7Z_STDOUT=1 A7Z_AF=1 A7Z_SKIP=1 \
        A7Z_FPS=$FPS A7Z_MAXEXP=30000 "$BIN" "$W" "$H" 100000000 x 2>>"$LOG"
  fi
  rc=$?
  tries=$((tries+1))
  case $rc in
    0)  echo "$(date -Is) clean exit" >> "$LOG"; break;;
    3)  echo "$(date -Is) RESTART $tries: encoder wedged (rc=3), reloading sunxi_ve" >> "$LOG"; reload_ve;;
    4)  echo "$(date -Is) RESTART $tries: stream stalled (rc=4)" >> "$LOG";;
    *)  echo "$(date -Is) RESTART $tries: exit rc=$rc" >> "$LOG"; sleep 2;;
  esac
  sleep 1
done
