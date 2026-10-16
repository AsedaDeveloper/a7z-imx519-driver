#!/usr/bin/env bash
# Prints everything that is useful for a bug report. Paste the output into the issue.
# Safe to run as a normal user; it changes nothing and does not print passwords or Wi-Fi names.
echo "### board";  cat /proc/device-tree/model /proc/device-tree/compatible 2>/dev/null | tr "\0" " "; echo
echo "### os";     . /etc/os-release 2>/dev/null && echo "$PRETTY_NAME"; uname -a
echo "### repo";   git -C "$(dirname "$0")/.." log --oneline -1 2>/dev/null
echo "### modules"; lsmod | grep -E "imx519|ak7375|vin" ; modinfo -n vin_v4l2 vin_io imx519_mipi ak7375_act 2>&1
echo "### overlay"; ls /boot/dtbo 2>/dev/null | grep -E "imx519|camera|cam2"
grep -E "fdtoverlays|overlay" /boot/extlinux/extlinux.conf 2>/dev/null | head -5
echo "### i2c bus 3 (camera 0x1a, lens 0x0c)"; i2cdetect -y -r 3 2>&1 | head -12
echo "### video devices"; ls -l /dev/video* /dev/v4l-subdev* /dev/media* 2>&1
v4l2-ctl -d /dev/video0 --list-formats 2>&1 | head -8
echo "### dmesg"; dmesg 2>/dev/null | grep -iE "imx519|ak7375|sunxi:vin|vin_v4l2|Oops|BUG:" | tail -60
echo "### in use?"; fuser -v /dev/video0 2>&1 | head -5
echo "### memory/disk"; free -m | sed -n 1,2p; df -h / | tail -1
