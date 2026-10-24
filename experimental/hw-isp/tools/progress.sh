#!/bin/sh
# progress.sh : percentage of ticked boxes in CHECKLIST.md, overall and per section
F="$(dirname "$0")/../CHECKLIST.md"
awk '
/^## [ABC]\./ { sec=$0; sub(/^## /,"",sec); order[++n]=sec }
/^- \[[ x]\] / { tot[sec]++; T++; if ($0 ~ /^- \[x\]/) { done[sec]++; D++ } }
END {
  for (i=1;i<=n;i++) { s=order[i]; printf "%-62s %2d/%2d  %3d%%\n", s, done[s]+0, tot[s], tot[s] ? 100*done[s]/tot[s] : 0 }
  printf "\nOVERALL %d/%d  %d%%\n", D, T, T ? 100*D/T : 0
}' "$F"
