#!/bin/bash
# wb_batch.sh LABEL TAG... : for each tag install the light table /tmp/p_TAG.bin with a fresh ISP state and capture the grey card.
# The only deletion is the ISP's saved-state file for the 1080p mode, which libisp otherwise reloads over the new table.
L=$1; shift
mkdir -p /tmp/cap
for t in "$@"; do
  sudo rm -f /mnt/isp0_2328_1748_30_0_imx519_mipi_ctx_saved.bin
  sudo install -m644 /tmp/p_$t.bin /mnt/isp_param_config.bin
  rm -f /tmp/cap/wb_${L}_${t}_*.yuv
  sudo env A7Z_FOCUS=1850 A7Z_EVERY=40 $HOME/bin/a7zcap.sweep 1920 1080 40 /tmp/cap/wb_${L}_$t >/dev/null 2>&1
done
sudo rm -f /mnt/isp0_2328_1748_30_0_imx519_mipi_ctx_saved.bin
sudo install -m644 $HOME/isp_param_config.bin.orig_wb /mnt/isp_param_config.bin
echo done $L: $#  tables, original table restored
