#!/bin/bash
# wb_deploy.sh : install the measured white-balance table (tuning_v5) as the live ISP parameter file.
# The only deletions are the ISP's saved-state files for the two modes (1080p and full resolution); libisp reloads them over a new table otherwise.
cp -n /mnt/isp_param_config.bin $HOME/isp_param_config.bin.before_v5
cp /tmp/p_new.bin $HOME/isp_param_config.bin.v5
sudo rm -f /mnt/isp0_2328_1748_30_0_imx519_mipi_ctx_saved.bin
sudo rm -f /mnt/isp1_4656_3496_9_0_imx519_mipi_ctx_saved.bin
sudo install -m644 $HOME/isp_param_config.bin.v5 /mnt/isp_param_config.bin
ls -l /mnt/isp_param_config.bin
echo "v5 table installed. Undo: sudo install -m644 ~/isp_param_config.bin.orig_wb /mnt/isp_param_config.bin  (and delete the saved-state files again)"
