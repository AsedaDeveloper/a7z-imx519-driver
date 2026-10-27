#!/usr/bin/env python3
"""Hold the camera at full load and watch the temperatures, the clocks and the delivered frame rate (checklist C8).

usage (on the board, camera service stopped):
    thermal_soak.py [SECONDS] [--mode WxH@FPS] [--idle]
    BIN=/home/radxa/bin/a7zcap.enc thermal_soak.py 900 --mode 4656x3496@15

Samples every 5 s: every thermal zone, the big- and little-core clocks, the cooling devices' states, and the feeder's own HEALTH line. Prints a row per sample and a
summary saying whether anything throttled (a clock below its maximum, or a cooling device above state 0) and whether the frame rate held.

--idle runs the same sampling with no capture, to get the room-temperature baseline the loaded run is compared against.
"""
import os, re, subprocess, sys, time

BIN = os.environ.get("BIN", os.path.expanduser("~/bin/a7zcap.enc"))
ZONES = "/sys/class/thermal"
CPU = "/sys/devices/system/cpu"


def zones():
    out = {}
    for z in sorted(os.listdir(ZONES)):
        if not z.startswith("thermal_zone"):
            continue
        try:
            name = open("%s/%s/type" % (ZONES, z)).read().strip().replace("_thermal_zone", "").replace("_zone", "")
            out[name] = int(open("%s/%s/temp" % (ZONES, z)).read()) / 1000
        except OSError:
            pass
    return out


def freqs():
    f = []
    for i in range(16):
        p = "%s/cpu%d/cpufreq/scaling_cur_freq" % (CPU, i)
        if os.path.exists(p):
            try:
                f.append(int(open(p).read()) // 1000)
            except OSError:
                pass
    return f


def maxfreqs():
    f = []
    for i in range(16):
        p = "%s/cpu%d/cpufreq/cpuinfo_max_freq" % (CPU, i)
        if os.path.exists(p):
            try:
                f.append(int(open(p).read()) // 1000)
            except OSError:
                pass
    return f


def cooling():
    st = {}
    base = ZONES
    for c in sorted(os.listdir(base)):
        if not c.startswith("cooling_device"):
            continue
        try:
            st[open("%s/%s/type" % (base, c)).read().strip()] = int(open("%s/%s/cur_state" % (base, c)).read())
        except OSError:
            pass
    return st


def main():
    secs = next((int(a) for a in sys.argv[1:] if a.isdigit()), 600)
    mode = next((a.split("=", 1)[1] if "=" in a else sys.argv[sys.argv.index(a) + 1] for a in sys.argv if a.startswith("--mode")), "4656x3496@15")
    idle = "--idle" in sys.argv
    wh, fps = mode.split("@")
    w, h = (int(x) for x in wh.split("x"))
    p = None
    if not idle:
        env = dict(os.environ, A7Z_FPS=fps, A7Z_HEALTH="1", A7Z_AF="1", A7Z_EVERY="100000", A7Z_NBUF="4")
        if w > 3264:
            env["A7Z_MERGE"] = "1"
        logf = open("/tmp/soak_feeder.log", "w")
        for _ in range(4):
            p = subprocess.Popen([BIN, str(w), str(h), str(int(fps) * secs + 600), "/tmp/cap/ts"], env=env, stdout=subprocess.DEVNULL, stderr=logf)
            time.sleep(12)
            if p.poll() is None:
                break                                   # a 2-in-1 start can come up with the left half empty (exit 6)
        print("load: %dx%d @%s fps" % (w, h, fps))
    else:
        print("idle baseline (no capture)")

    mx = max(maxfreqs() or [0])
    hot, throttled, rates, topclk = {}, [], [], 0
    cap0 = [int(open("%s/cpu%d/cpufreq/scaling_max_freq" % (CPU, i)).read()) // 1000 for i in (0, 6) if os.path.exists("%s/cpu%d/cpufreq/scaling_max_freq" % (CPU, i))]
    t0 = time.time()
    print("%6s  %-44s  %-18s  %s" % ("t", "temperatures C", "cpu MHz", "fps"))
    while time.time() - t0 < secs:
        z, f, c = zones(), freqs(), cooling()
        topclk = max(topclk, max(f or [0]))
        for k, v in z.items():
            hot[k] = max(hot.get(k, 0), v)
        act = {k: v for k, v in c.items() if v}
        if act:
            throttled.append((round(time.time() - t0), act))
        fr = None
        if p:
            try:
                tail = subprocess.run(["tail", "-n", "40", "/tmp/soak_feeder.log"], capture_output=True, text=True).stdout
                m = re.findall(r"HEALTH .*fps=([\d.]+)", tail)
                fr = float(m[-1]) if m else None
            except OSError:
                pass
        if fr:
            rates.append(fr)
        print("%5ds  %-44s  %-18s  %s" % (round(time.time() - t0), " ".join("%s %.0f" % kv for kv in z.items()),
              "/".join(map(str, f)), "-" if fr is None else "%.1f" % fr), flush=True)
        time.sleep(5)

    if p and p.poll() is None:
        p.terminate()
    print("\nsummary over %ds%s" % (secs, "" if not p else " at %s" % mode))
    print("  peak temperatures: " + ", ".join("%s %.0f C" % kv for kv in sorted(hot.items(), key=lambda kv: -kv[1])))
    capn = [int(open("%s/cpu%d/cpufreq/scaling_max_freq" % (CPU, i)).read()) // 1000 for i in (0, 6) if os.path.exists("%s/cpu%d/cpufreq/scaling_max_freq" % (CPU, i))]
    print("  peak core clock during the run: %d MHz of %d MHz rated" % (topclk, mx))
    print("  cpufreq ceiling (cpu0/cpu6): %s at the start, %s at the end  <- a drop here is real thermal capping" % (cap0, capn))
    print("  cooling devices active: %s" % (throttled[:3] if throttled else "never (no throttling)"))
    if rates:
        rates = rates[1:] or rates
        print("  frame rate: first %.1f, last %.1f, min %.1f, max %.1f" % (rates[0], rates[-1], min(rates), max(rates)))


if __name__ == "__main__":
    main()
