#!/bin/sh
# deploy_param.sh FILE        copy a param file to the board as /mnt/isp_param_config.bin
# deploy_param.sh --remove    go back to libisp's built-in defaults
# needs A7Z_HOST=user@board and sudo on the board
HOST=${A7Z_HOST:?set A7Z_HOST=user@board (ssh target)}
if [ "$1" = "--remove" ]; then ssh -o ConnectTimeout=8 "$HOST" 'sudo rm -f /mnt/isp_param_config.bin /mnt/isp0_*_ctx_saved.bin'; exit; fi
scp -q "$1" "$HOST:/tmp/isp_param_config.bin"
ssh -o ConnectTimeout=8 "$HOST" 'sudo rm -f /mnt/isp0_*_ctx_saved.bin; sudo install -m644 /tmp/isp_param_config.bin /mnt/isp_param_config.bin; ls -l /mnt'
