#!/usr/bin/env python3
"""What a frame's timestamp means, and what it is worth for a shared log (checklist C9).

usage (on the board):  clock_check.py [N]        default 300 frames from /dev/video9

Answers four things a balloon or observatory log needs:
  1. which clock the V4L2 buffer timestamp is on (CLOCK_MONOTONIC or CLOCK_REALTIME),
  2. how steady the frame interval is (jitter, and the worst gap),
  3. how to turn a frame timestamp into UTC, and how much that conversion can be trusted,
  4. whether this board's wall clock is anchored to anything at all.
"""
import os, struct, subprocess, sys, time
sys.path.insert(0, "/usr/local/lib")
from a7z_v4l2 import Camera


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 300
    cam = Camera().open()
    rows = []
    for _ in range(n):
        cam.read()
        rows.append((cam.last_ts_ns, time.clock_gettime_ns(time.CLOCK_MONOTONIC), time.clock_gettime_ns(time.CLOCK_REALTIME), cam.last_seq))
    cam.close()

    dm = [(b - a) / 1e6 for a, b, _, _ in rows]
    dr = [(c - a) / 1e6 for a, _, c, _ in rows]
    print("1. which clock")
    print("   buffer ts vs CLOCK_MONOTONIC read just after: %+.2f ms median  -> %s" % (sorted(dm)[len(dm) // 2],
          "CLOCK_MONOTONIC" if abs(sorted(dm)[len(dm) // 2]) < 1000 else "not monotonic"))
    print("   buffer ts vs CLOCK_REALTIME read just after:  %+.0f ms median" % sorted(dr)[len(dr) // 2])

    iv = sorted((rows[i + 1][0] - rows[i][0]) / 1e6 for i in range(len(rows) - 1))
    med = iv[len(iv) // 2]
    print("\n2. frame interval (%d frames)" % len(rows))
    print("   median %.3f ms (= %.2f fps), min %.3f, max %.3f" % (med, 1000 / med, iv[0], iv[-1]))
    print("   jitter: 90%% of intervals within %.3f..%.3f ms of the median" % (iv[len(iv) // 20] - med, iv[-len(iv) // 20] - med))
    gaps = [x for x in iv if x > 1.5 * med]
    print("   gaps longer than 1.5x the median: %d%s" % (len(gaps), (" (worst %.1f ms)" % max(gaps)) if gaps else ""))

    off = [(c - a) for a, _, c, _ in rows]
    off_ms = [x / 1e6 for x in off]
    print("\n3. monotonic -> UTC")
    print("   offset CLOCK_REALTIME - frame ts: %.3f s, spread over the run %.1f ms" % (sum(off) / len(off) / 1e9, max(off_ms) - min(off_ms)))
    print("   so: utc_ns = frame_ts_ns + offset, with the offset read once per session (clock_gettime of both clocks back to back).")

    print("\n4. is the wall clock anchored?")
    rtc = os.path.exists("/dev/rtc0") or os.path.exists("/dev/rtc")
    try:
        ntp = subprocess.run(["timedatectl", "show", "-p", "NTPSynchronized", "-p", "LocalRTC", "--value"], capture_output=True, text=True).stdout.split()
    except OSError:
        ntp = []
    print("   RTC device present: %s" % rtc)
    print("   NTP synchronised:   %s" % (ntp[0] if ntp else "unknown"))
    print("   board wall clock:   %s" % time.strftime("%Y-%m-%d %H:%M:%S %Z"))
    print("   -> if NTP says no and there is no network, this is an arbitrary value: log the monotonic timestamp and one")
    print("      externally-sourced anchor (GPS fix, ground-station time) per session rather than trusting the wall clock.")


if __name__ == "__main__":
    main()
