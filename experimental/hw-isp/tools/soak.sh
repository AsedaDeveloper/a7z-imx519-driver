#!/bin/bash
# soak.sh MINUTES — run the supervised stream for a while and report the numbers the checklist (B7) asks for.
M=${1:-10}; LOG=/tmp/soak_$(date +%H%M%S).log
export A7Z_LOG=$LOG
timeout "${M}m" bash /tmp/a7zstream.sh 1920 1080 30 6000 > /tmp/soak_out.h264
echo "=== soak $M min ==="
python3 - "$LOG" <<'PY'
import sys, re
L = open(sys.argv[1], errors="replace").read().splitlines()
fps = [float(m.group(1)) for l in L if (m := re.search(r"fps=([\d.]+)", l))]
drops = [int(m.group(1)) for l in L if (m := re.search(r"seqdrops=(\d+)", l))]
stale = [int(m.group(1)) for l in L if (m := re.search(r"stale=(\d+)", l))]
af = [float(m.group(1)) for l in L if (m := re.search(r"sweep: \d+ frames, ([\d.]+) s", l))]
res = [l for l in L if "RESTART" in l]
print(f"health samples   {len(fps)}")
if fps: print(f"fps              min {min(fps):.1f}  median {sorted(fps)[len(fps)//2]:.1f}  max {max(fps):.1f}")
print(f"driver frame drops (seq gaps)  {max(drops) if drops else 0}")
print(f"stale frames skipped           {max(stale) if stale else 0}")
print(f"autofocus runs   {len(af)}" + (f"  times {min(af):.2f}-{max(af):.2f} s" if af else ""))
print(f"restarts         {len(res)}")
for r in res[:10]: print("   ", r)
PY
ls -l /tmp/soak_out.h264 | awk '{printf "bitstream        %.1f MB\n", $5/1048576}'
