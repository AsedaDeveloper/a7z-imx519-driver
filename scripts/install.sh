#!/usr/bin/env bash
# Installer for the IMX519 (Waveshare / Arducam 16 MP autofocus module) on the Radxa Cubie A7Z.
#
#   sudo ./scripts/install.sh [--no-vin-patch] [--bsp-dir DIR] [--work-dir DIR] [--no-overlay] [--no-tools] [--dry-run] [--force]
#
# What it does (each step is idempotent, it is safe to run it again):
#   1. checks the system (Radxa Debian, kernel headers, build tools)
#   2. gets the Radxa/Allwinner BSP source (only drivers/vin + include, a few MB)
#   3. builds + installs the sensor driver (imx519_mipi) and the lens motor driver (ak7375_act)
#   4. builds + installs a patched VIN core (vin_io + vin_v4l2) into /lib/modules/<kernel>/updates/  (can be skipped)
#   5. compiles + installs the device-tree overlay for the camera connector and refreshes the boot menu
#   6. installs a udev rule (exposure/gain parameters writable by the video group) and autoloads the lens driver
#   7. installs the tools (a7zcam, live view) into /opt/a7zcam
# Then REBOOT. Undo everything with scripts/uninstall.sh.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KVER="$(uname -r)"
KBUILD="/lib/modules/${KVER}/build"
BSP_TAG="cubie-aiot-v1.5.0"
BSP_URL="https://github.com/radxa/allwinner-bsp.git"
WORK="/opt/a7z-imx519-driver"
BSP="${WORK}/bsp"
PATCH_VIN=1; DO_OVERLAY=1; DO_TOOLS=1; DRY=0; FORCE=0; BSP_GIVEN=0

while [ $# -gt 0 ]; do
  case "$1" in
    --no-vin-patch) PATCH_VIN=0 ;;
    --no-overlay)   DO_OVERLAY=0 ;;
    --no-tools)     DO_TOOLS=0 ;;
    --bsp-dir)      BSP="$2"; BSP_GIVEN=1; shift ;;
    --work-dir)     WORK="$2"; [ "$BSP_GIVEN" = 1 ] || BSP="${WORK}/bsp"; shift ;;
    --dry-run)      DRY=1 ;;
    --force)        FORCE=1 ;;   # continue even if the board / kernel is not the tested one
    -h|--help)      sed -n '2,17p' "$0"; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
  shift
done

say()  { printf '\n\033[1m== %s\033[0m\n' "$*"; }
warn() { printf '\033[33mWARNING: %s\033[0m\n' "$*"; }
die()  { printf '\033[31mERROR: %s\033[0m\n' "$*" >&2; exit 1; }
run()  { if [ "$DRY" = 1 ]; then echo "[dry-run] $*"; else "$@"; fi; }

[ "$(id -u)" = 0 ] || die "run as root: sudo $0"

# ------------------------------------------------------------------------------------------------ 1. checks
say "1/7 checking the system"
MODEL="$(cat /proc/device-tree/model 2>/dev/null | tr -d '\0' || true)"
COMPAT="$(cat /proc/device-tree/compatible 2>/dev/null | tr '\0' ' ' || true)"
if ! echo "$MODEL $COMPAT" | grep -qi "a7z"; then
  [ "$FORCE" = 1 ] && warn "this does not look like a Radxa Cubie A7Z (model: '${MODEL:-unknown}'), continuing because of --force" \
    || die "this does not look like a Radxa Cubie A7Z (model: '${MODEL:-unknown}'). The overlay is specific to connector J5 of the A7Z. Use --force to override."
fi
if [ "$KVER" != "6.6.98-5-aw2511" ]; then
  [ "$FORCE" = 1 ] && warn "tested only with kernel 6.6.98-5-aw2511 (running $KVER), continuing because of --force" \
    || die "tested only with kernel 6.6.98-5-aw2511 (running $KVER). Modules built for another kernel may not load or may crash. Use --force to try anyway and please report the result (docs/TESTED.md)."
fi
[ -d "$KBUILD" ] || die "kernel headers missing: apt install linux-headers-${KVER}  (or linux-headers-radxa-a733)"
for t in git make gcc dtc patch python3 xz strip; do command -v "$t" >/dev/null || die "missing tool '$t' (apt install git build-essential device-tree-compiler patch python3 xz-utils binutils)"; done
[ -d /boot/dtbo ] || [ "$DO_OVERLAY" = 0 ] || die "/boot/dtbo not found: this image does not use u-boot overlays, so install.sh cannot enable the camera (use --no-overlay and add the overlay by hand)"
FREE_MB=$(df -Pm "$(dirname "$WORK")" 2>/dev/null | awk 'NR==2{print $4}')
[ -z "${FREE_MB:-}" ] || [ "$FREE_MB" -ge 1500 ] || die "only ${FREE_MB} MB free where $WORK will live; about 1500 MB are needed for the BSP source and the build"
if [ ! -d "$BSP/drivers/vin" ]; then
  git ls-remote --exit-code --tags "$BSP_URL" "refs/tags/$BSP_TAG" >/dev/null 2>&1 \
    || die "cannot reach $BSP_URL (tag $BSP_TAG). Check the network, or pass --bsp-dir with an existing checkout."
fi
if [ -e /dev/video0 ] && fuser /dev/video0 >/dev/null 2>&1; then warn "/dev/video0 is in use (live view?). Stop it before rebooting; the install itself is not affected."; fi
mkdir -p "$WORK"

# ------------------------------------------------------------------------------------------------ 2. BSP source
say "2/7 BSP source (drivers/vin, include) -> $BSP"
if [ -d "$BSP/drivers/vin" ]; then
  echo "found existing BSP tree, using it"
else
  run git clone --depth 1 --branch "$BSP_TAG" --filter=blob:none --sparse "$BSP_URL" "$BSP"
  run git -C "$BSP" sparse-checkout set drivers/vin include
fi
[ "$DRY" = 1 ] || [ -f "$BSP/drivers/vin/vin.c" ] || die "BSP tree incomplete ($BSP)"
if [ "$PATCH_VIN" = 1 ] && [ -f "$BSP/drivers/vin/vin.c" ]; then
  # fail early (before any build) if the VIN patch does not fit this BSP revision
  patch -p1 --dry-run -s -d "$BSP" < "$REPO/patches/vin_core_fixes.patch" >/dev/null 2>&1 \
    && echo "VIN patch applies cleanly to this BSP tree" \
    || { [ "$FORCE" = 1 ] && warn "VIN patch does not apply to this BSP tree (continuing: --force)" \
         || die "VIN patch does not apply to this BSP revision ($BSP). Re-run with --no-vin-patch, or use the tag $BSP_TAG."; }
fi

VINCFLAGS() { # include paths shared by all builds
  local d="$1"
  echo "-I$d/include -I$d/drivers/vin -I$d/drivers/vin/utility -I$d/drivers/vin/vin-cci -I$d/drivers/vin/platform \
-I$d/drivers/vin/vin-video -I$d/drivers/vin/vin-mipi -I$d/drivers/vin/vin-csi -I$d/drivers/vin/vin-isp \
-I$d/drivers/vin/vin-vipp -I$d/drivers/vin/vin-stat -I$d/drivers/vin/vin-tdm -I$d/drivers/vin/modules/sensor \
-I$d/drivers/vin/modules/actuator -I$d/drivers/vin/modules/flash -DDYNAMIC_DEBUG_MODULE -Wno-error"
}

EXTRA="/lib/modules/${KVER}/extra"; UPD="/lib/modules/${KVER}/updates"
run mkdir -p "$EXTRA" "$UPD"

# ------------------------------------------------------------------------------------------------ 3. sensor + lens drivers
say "3/7 building the sensor driver (imx519_mipi) and the lens motor driver (ak7375_act)"
SDIR="$BSP/drivers/vin/modules/sensor"; ADIR="$BSP/drivers/vin/modules/actuator"
if [ "$DRY" = 0 ]; then
  [ -f "$SDIR/Makefile.orig" ] || cp "$SDIR/Makefile" "$SDIR/Makefile.orig"
  [ -f "$ADIR/Makefile.orig" ] || cp "$ADIR/Makefile" "$ADIR/Makefile.orig"
  cp "$REPO/driver/imx519_mipi.c" "$SDIR/imx519_mipi.c"
  cp "$REPO/driver/ak7375_act.c"  "$ADIR/ak7375_act.c"
  echo 'obj-$(CONFIG_SENSOR_IMX519_MIPI) += imx519_mipi.o' > "$SDIR/Makefile"
  echo 'obj-$(CONFIG_ACTUATOR_AK7375) += ak7375_act.o'     > "$ADIR/Makefile"
  ( cd "$SDIR" && rm -f ./*.o ./*.ko .*.cmd Module.symvers modules.order
    KCFLAGS="$(VINCFLAGS "$BSP") -Wall" make -C "$KBUILD" M="$SDIR" CONFIG_SENSOR_IMX519_MIPI=m KBUILD_MODPOST_WARN=1 modules >"$WORK/build_sensor.log" 2>&1 ) \
    || { tail -20 "$WORK/build_sensor.log"; die "sensor driver build failed (log: $WORK/build_sensor.log)"; }
  ( cd "$ADIR" && rm -f ./*.o ./*.ko .*.cmd Module.symvers modules.order
    KCFLAGS="$(VINCFLAGS "$BSP") -DCONFIG_ACTUATOR_MODULE=1 -Wall" make -C "$KBUILD" M="$ADIR" CONFIG_ACTUATOR_AK7375=m KBUILD_MODPOST_WARN=1 modules >"$WORK/build_actuator.log" 2>&1 ) \
    || { tail -20 "$WORK/build_actuator.log"; die "lens driver build failed (log: $WORK/build_actuator.log)"; }
  install -m 644 "$SDIR/imx519_mipi.ko" "$EXTRA/imx519_mipi.ko"
  install -m 644 "$ADIR/ak7375_act.ko"  "$EXTRA/ak7375_act.ko"
  echo "installed imx519_mipi.ko and ak7375_act.ko into $EXTRA"
else echo "[dry-run] would build + install both drivers"; fi

# ------------------------------------------------------------------------------------------------ 4. VIN core patch
if [ "$PATCH_VIN" = 1 ]; then
  say "4/7 building the patched VIN core (fixes kernel crashes of standard V4L2 tools)"
  VW="$WORK/bsp-patched"
  if [ "$DRY" = 0 ]; then
    rm -rf "$VW"; mkdir -p "$VW/drivers"
    cp -a "$BSP/include" "$VW/include"; cp -a "$BSP/drivers/vin" "$VW/drivers/vin"
    # the sensor/actuator subdirectories must not be rebuilt here
    echo '# (intentionally empty: built separately)' > "$VW/drivers/vin/modules/sensor/Makefile"
    echo '# (intentionally empty: built separately)' > "$VW/drivers/vin/modules/actuator/Makefile"
    ( cd "$VW" && patch -p1 < "$REPO/patches/vin_core_fixes.patch" ) || die "VIN patch does not apply to this BSP revision"
    ( cd "$VW/drivers/vin" && KCFLAGS="$(VINCFLAGS "$VW")" make -C "$KBUILD" M="$VW/drivers/vin" KBUILD_MODPOST_WARN=1 modules >"$WORK/build_vin.log" 2>&1 ) \
      || { tail -20 "$WORK/build_vin.log"; die "VIN build failed (log: $WORK/build_vin.log)"; }
    for m in vin_io vin_v4l2; do
      install -m 644 "$VW/drivers/vin/$m.ko" "$UPD/$m.ko"; strip --strip-debug "$UPD/$m.ko"
    done
    echo "installed patched vin_io.ko + vin_v4l2.ko into $UPD (the originals under kernel/ are untouched)"
  else echo "[dry-run] would patch + build + install vin_io / vin_v4l2"; fi
else
  say "4/7 VIN patch skipped (--no-vin-patch): listing controls before selecting the input, or setting exposure/gain on /dev/video0, will crash the kernel"
fi

# ------------------------------------------------------------------------------------------------ 5. overlay
if [ "$DO_OVERLAY" = 1 ]; then
  say "5/7 device-tree overlay for the camera connector"
  if [ "$DRY" = 0 ]; then
    dtc -I dts -O dtb -o /tmp/cubie-a7z-imx519-j5.dtbo "$REPO/overlay/imx519-j5.dts" 2>/dev/null || die "overlay did not compile"
    # dry-run the overlay against the running device tree first
    if command -v fdtoverlay >/dev/null && [ -r /sys/firmware/fdt ]; then
      fdtoverlay -i /sys/firmware/fdt -o /tmp/overlay-test.dtb /tmp/cubie-a7z-imx519-j5.dtbo \
        || die "overlay does not apply to the running device tree - NOT installing it"
    fi
    install -m 644 /tmp/cubie-a7z-imx519-j5.dtbo /boot/dtbo/cubie-a7z-imx519-j5.dtbo
    # only one camera overlay may be active at a time
    for f in /boot/dtbo/*camera*.dtbo /boot/dtbo/*cam2*.dtbo; do [ -e "$f" ] && mv -f "$f" "$f.disabled" || true; done
    if command -v u-boot-update >/dev/null; then u-boot-update >/dev/null 2>&1 || warn "u-boot-update failed; run it by hand"; else warn "u-boot-update not found; enable the overlay with rsetup"; fi
    echo "installed /boot/dtbo/cubie-a7z-imx519-j5.dtbo (the boot menu keeps a rescue entry without overlays)"
  else echo "[dry-run] would compile + install the overlay"; fi
else say "5/7 overlay skipped (--no-overlay)"; fi

# ------------------------------------------------------------------------------------------------ 6. udev + autoload
say "6/7 udev rule and module autoload"
run install -m 644 "$REPO/scripts/62-imx519-params.rules" /etc/udev/rules.d/62-imx519-params.rules
run install -m 644 "$REPO/scripts/63-cedar-ve.rules" /etc/udev/rules.d/63-cedar-ve.rules   # hardware H.264 encoder for the video group
# the lens driver must be loaded BEFORE the VIN core so that the framework finds it
run sh -c "echo ak7375_act > /etc/modules-load.d/ak7375_act.conf"
run depmod -a
run udevadm control --reload || true

# ------------------------------------------------------------------------------------------------ 7. tools
if [ "$DO_TOOLS" = 1 ]; then
  say "7/7 tools -> /opt/a7zcam"
  run mkdir -p /opt/a7zcam
  run install -m 755 "$REPO/tools/a7zcam.py"   /opt/a7zcam/a7zcam.py
  run install -m 644 "$REPO/tools/liveview.py" /opt/a7zcam/liveview.py
  run install -m 644 "$REPO/tools/imx519_color.json" /opt/a7zcam/imx519_color.json
  run ln -sf /opt/a7zcam/a7zcam.py /usr/local/bin/a7zcam
  if ! command -v gst-inspect-1.0 >/dev/null || ! gst-inspect-1.0 omxh264videoenc >/dev/null 2>&1; then
    warn "hardware H.264 encoding needs GStreamer + the Allwinner OMX plugin: apt install gstreamer1.0-tools gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-plugins-bad gstreamer1.0-omx-allwinner (video falls back to software x264 without it)"
  fi
  if ! python3 -c "import numpy, cv2" 2>/dev/null; then
    warn "python3 numpy/opencv missing: apt install python3-numpy python3-opencv"
  fi
  run usermod -aG video,i2c "${SUDO_USER:-radxa}" 2>/dev/null || true
else say "7/7 tools skipped"; fi

say "done"
echo "Reboot now:   sudo reboot"
echo "Then test:    a7zcam still /tmp/test.jpg --size 1080p --af      (and: a7zcam live  -> http://<board>:8080)"
echo "Undo:         sudo ./scripts/uninstall.sh"
