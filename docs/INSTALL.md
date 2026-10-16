# Install

## Requirements
* Radxa Cubie A7Z running Radxa's Debian 13 image, kernel **6.6.98-5-aw2511** (`uname -r`). Other kernels may need source changes.
* Network access (the installer downloads Radxa's BSP source, ~15 MB: `drivers/vin` and `include` only).
* `sudo apt install git build-essential device-tree-compiler patch xz-utils binutils python3-numpy python3-opencv linux-headers-$(uname -r)`
* IMX519 module + Radxa AC008 cable, plugged in **with the board powered off** (see [HARDWARE.md](HARDWARE.md)).

## Install
```bash
git clone https://github.com/AsedaDeveloper/a7z-imx519-driver.git
cd a7z-imx519-driver
sudo ./scripts/install.sh            # add --no-vin-patch to skip the VIN patch (not recommended)
sudo reboot
```
Options: `--work-dir DIR` (build area, default `/opt/a7z-imx519-driver`), `--bsp-dir DIR` (reuse an existing BSP checkout),
`--no-overlay`, `--no-tools`, `--dry-run`.

What it installs: `imx519_mipi.ko` + `ak7375_act.ko` in `/lib/modules/<kernel>/extra/`, patched `vin_io.ko` +
`vin_v4l2.ko` in `/lib/modules/<kernel>/updates/` (they take precedence; the originals stay in `kernel/`), the overlay
`/boot/dtbo/cubie-a7z-imx519-j5.dtbo`, a udev rule, `/etc/modules-load.d/ak7375_act.conf` (the lens driver must load
**before** the VIN core) and the tools in `/opt/a7zcam` (`/usr/local/bin/a7zcam`).

## Check
```bash
lsmod | grep -E "imx519|ak7375|vin"            # imx519_mipi, ak7375_act, vin_io, vin_v4l2
dmesg | grep imx519                            # "find the sony IMX519", chip id 0x0519
a7zcam still /tmp/test.jpg --size 1080p --af
```

## Remove / roll back
```bash
sudo ./scripts/uninstall.sh && sudo reboot
```
This restores the stock Radxa camera stack. The boot menu always keeps a rescue entry **without** overlays: if a
bad overlay ever stops the board booting, pick that entry in the boot menu.

## Updating the kernel
After a kernel update re-run `sudo ./scripts/install.sh` (modules are built against the running kernel's headers).
