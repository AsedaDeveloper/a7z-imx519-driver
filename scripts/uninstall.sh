#!/usr/bin/env bash
# Removes everything scripts/install.sh installed and restores the stock Radxa camera stack.
#   sudo ./scripts/uninstall.sh [--keep-work]
set -euo pipefail
KVER="$(uname -r)"
[ "$(id -u)" = 0 ] || { echo "run as root: sudo $0" >&2; exit 1; }
KEEP=0; [ "${1:-}" = "--keep-work" ] && KEEP=1

echo "removing kernel modules (the original Radxa VIN modules under kernel/ were never touched)"
rm -f "/lib/modules/${KVER}/extra/imx519_mipi.ko" "/lib/modules/${KVER}/extra/ak7375_act.ko" \
      "/lib/modules/${KVER}/updates/vin_io.ko" "/lib/modules/${KVER}/updates/vin_v4l2.ko"
rmdir --ignore-fail-on-non-empty "/lib/modules/${KVER}/updates" 2>/dev/null || true
rm -f /etc/modprobe.d/a7zcam-v4l2loopback.conf /etc/modules-load.d/v4l2loopback.conf /etc/modules-load.d/ak7375_act.conf /etc/udev/rules.d/62-imx519-params.rules /etc/udev/rules.d/63-cedar-ve.rules
echo "removing the device-tree overlay"
rm -f /boot/dtbo/cubie-a7z-imx519-j5.dtbo
command -v u-boot-update >/dev/null && u-boot-update >/dev/null 2>&1 || true
echo "removing tools"
for n in still vid hello; do for pfx in rpicam libcamera; do [ "$(readlink "/usr/local/bin/$pfx-$n" 2>/dev/null)" = /opt/a7zcam/rpicam.py ] && rm -f "/usr/local/bin/$pfx-$n"; done; done
rm -rf /opt/a7zcam; rm -f /usr/local/bin/a7zcam
[ "$KEEP" = 1 ] || rm -rf /opt/a7z-imx519-driver
depmod -a
udevadm control --reload || true
echo "done - reboot to unload the modules"
