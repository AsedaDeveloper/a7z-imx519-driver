#!/usr/bin/env bash
# Builds libcamera v0.5.2 with Allwinner sunxi-vin support (software ISP) into /opt/libcamera-a7z.
# Does not touch the Debian libcamera packages. Takes about 30-40 minutes on the A7Z.
#
#   sudo ./scripts/build-libcamera.sh [--src DIR] [--prefix DIR] [--jobs N]
#
# Afterwards run libcamera programs through the wrapper:   lc-a7z cam -l   /   lc-a7z gst-launch-1.0 libcamerasrc ! ...
set -euo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC=/opt/a7z-imx519-driver/libcamera-src; PREFIX=/opt/libcamera-a7z; JOBS=4; TAG=v0.5.2
while [ $# -gt 0 ]; do case "$1" in
  --src) SRC="$2"; shift ;; --prefix) PREFIX="$2"; shift ;; --jobs) JOBS="$2"; shift ;;
  -h|--help) sed -n '2,8p' "$0"; exit 0 ;; *) echo "unknown option $1" >&2; exit 2 ;; esac; shift; done
[ "$(id -u)" = 0 ] || { echo "run as root: sudo $0" >&2; exit 1; }

echo "== dependencies"
DEBIAN_FRONTEND=noninteractive apt-get install -y meson ninja-build pkg-config python3-yaml python3-ply python3-jinja2 \
  libgnutls28-dev libyaml-dev libudev-dev libevent-dev python3-dev pybind11-dev libssl-dev openssl libdw-dev libunwind-dev \
  libglib2.0-dev libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev libtiff-dev libjpeg-dev libdrm-dev git >/dev/null

echo "== source ($TAG) -> $SRC"
if [ ! -d "$SRC/.git" ]; then
  git clone --depth 1 --branch "$TAG" https://git.libcamera.org/libcamera/libcamera.git "$SRC"
fi
cd "$SRC"
if git apply --check "$REPO/libcamera/0001-libcamera-v0.5.2-sunxi-vin-support.patch" 2>/dev/null; then
  git apply "$REPO/libcamera/0001-libcamera-v0.5.2-sunxi-vin-support.patch"; echo "patch applied"
elif git apply --reverse --check "$REPO/libcamera/0001-libcamera-v0.5.2-sunxi-vin-support.patch" 2>/dev/null; then
  echo "patch already applied"
else echo "patch does not apply to this tree" >&2; exit 1; fi

echo "== configure + build (-j$JOBS)"
[ -d build ] || meson setup build --prefix="$PREFIX" --buildtype=release -Dpipelines=simple -Dipas=simple -Dcam=enabled \
  -Dqcam=disabled -Dgstreamer=enabled -Dpycamera=enabled -Dtest=false -Ddocumentation=disabled -Dlc-compliance=disabled -Dv4l2=true
ninja -C build -j"$JOBS"
ninja -C build install
install -m 644 "$REPO/libcamera/imx519_mipi.yaml" "$PREFIX/share/libcamera/ipa/simple/imx519_mipi.yaml"

PY=$(dirname "$(dirname "$(find "$PREFIX" -name '_libcamera*.so' | head -1)")")
cat > /usr/local/bin/lc-a7z <<W
#!/bin/sh
# run a program with the A7Z libcamera build ($PREFIX)
export LD_LIBRARY_PATH="$PREFIX/lib/aarch64-linux-gnu\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}"
export LIBCAMERA_IPA_MODULE_PATH="$PREFIX/lib/aarch64-linux-gnu/libcamera/ipa"
export GST_PLUGIN_PATH="$PREFIX/lib/aarch64-linux-gnu/gstreamer-1.0\${GST_PLUGIN_PATH:+:\$GST_PLUGIN_PATH}"
export PYTHONPATH="$PY\${PYTHONPATH:+:\$PYTHONPATH}"
export PATH="$PREFIX/bin:\$PATH"
export LIBCAMERA_LOG_LEVELS="\${LIBCAMERA_LOG_LEVELS:-*:3}"
exec "\$@"
W
chmod 755 /usr/local/bin/lc-a7z
echo "done. Try:  lc-a7z cam -l"
