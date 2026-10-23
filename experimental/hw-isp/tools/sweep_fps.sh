#!/bin/bash
# Try one set of tunable full-res sensor parameters on the board and print one result line.
# usage (on the board): sweep_fps.sh TAG IVT IOP HTS VTS      e.g.  sweep_fps.sh t1 480 432 11900 3572
# Needs a7zdual in ~/bin and passwordless sudo. The captured frame is kept as /tmp/cap/sw_TAG.yuv; ALWAYS score it with
# tools/corruption_check.py: frames keep arriving (and the frame counter looks fine) well past the point where the picture is damaged.
P=/sys/module/imx519_mipi/parameters
echo $2 | sudo tee $P/fast_ivt >/dev/null; echo $3 | sudo tee $P/fast_iop >/dev/null
echo $4 | sudo tee $P/fast_hts >/dev/null; echo $5 | sudo tee $P/fast_vts >/dev/null
mkdir -p /tmp/cap; sudo dmesg -C; rm -f /tmp/cap/sw_$1.yuv /tmp/cap/d_*
out=$(A7Z_MERGE=1 A7Z_FPS=15 timeout 60 ~/bin/a7zdual 4656 3496 40 /tmp/cap/d 2>&1)
fr=$(echo "$out" | grep -o "n0=[0-9]*" | cut -d= -f2); fps=$(echo "$out" | grep -o "measured [0-9.]* fps" | head -1)
[ -f /tmp/cap/d_n0_40_4656x3496.yuv ] && mv /tmp/cap/d_n0_40_4656x3496.yuv /tmp/cap/sw_$1.yuv
d=$(sudo dmesg)
echo "$1 ivt=$2 iop=$3 hts=$4 vts=$5 -> pclk $(echo "$2*4/3" | bc)MHz lane $(echo "$3*3" | bc)Mbps theory $(echo "scale=2; $2*4000000/3/($4*$5)" | bc)fps | frames=${fr:-0} ${fps:-no-fps} | bufferfull=$(echo "$d" | grep -c 'buffer full') framelost=$(echo "$d" | grep -c 'frame lost')"
