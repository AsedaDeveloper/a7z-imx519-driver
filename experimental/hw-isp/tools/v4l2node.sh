#!/bin/bash
# v4l2node.sh [start|stop|status] — expose the IMX519 through the hardware ISP as a standard V4L2 device.
#
# Why a loopback: libisp cannot run in a different process from the one that streams. A second process asking the vendor
# driver for frames gets S_FMT EBUSY and STREAMON EPERM, so /dev/video0 cannot be shared. a7zcap owns the ISP and
# republishes each frame into a v4l2loopback node, which ordinary V4L2 apps open normally.
NODE=${NODE:-9}
W=${W:-1920} H=${H:-1080} FPS=${FPS:-30}
BIN=${BIN:-/home/radxa/bin/a7zcap.enc}
CTX=/mnt/isp0_2328_1748_30_0_imx519_mipi_ctx_saved.bin

case "${1:-start}" in
start)
  if ! lsmod | grep -q '^v4l2loopback'; then
    modprobe v4l2loopback devices=1 video_nr=$NODE card_label="A7Z IMX519" exclusive_caps=1 max_buffers=4 \
      || insmod /lib/modules/$(uname -r)/extra/v4l2loopback.ko devices=1 video_nr=$NODE card_label="A7Z IMX519" exclusive_caps=1 max_buffers=4
    sleep 1
  fi
  pkill -x a7zcap.enc 2>/dev/null; sleep 1
  rm -f "$CTX"                               # libisp reloads stale ISP state over the tuning table otherwise
  setsid env A7Z_LOOPBACK=/dev/video$NODE A7Z_FRESH=1 A7Z_HEALTH=1 A7Z_AF=1 A7Z_FPS=$FPS A7Z_MAXEXP=30000 \
      A7Z_EVERY=100000 "$BIN" "$W" "$H" 100000000 /tmp/cap/lb >/tmp/v4l2node.log 2>&1 &
  sleep 12
  grep -qE "loopback: /dev/video$NODE" /tmp/v4l2node.log \
    && echo "/dev/video$NODE serving ${W}x${H} YU12 at ${FPS} fps (ISP, autofocus, exposure cap)" \
    || { echo "failed to start — see /tmp/v4l2node.log"; tail -3 /tmp/v4l2node.log; exit 1; }
  ;;
stop)
  pkill -x a7zcap.enc 2>/dev/null
  sleep 1
  rmmod v4l2loopback 2>/dev/null
  echo stopped
  ;;
status)
  echo "feeder : $(pgrep -cx a7zcap.enc) process(es)"
  echo "module : $(lsmod | grep -c '^v4l2loopback') loaded"
  ls -l /dev/video$NODE 2>/dev/null
  grep HEALTH /tmp/v4l2node.log 2>/dev/null | tail -1
  ;;
esac
