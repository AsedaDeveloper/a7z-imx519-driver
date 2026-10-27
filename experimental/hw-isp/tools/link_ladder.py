#!/usr/bin/env python3
"""Step the MIPI lane rate and read the receiver's own error bits alongside the damage visible in the picture (checklist B6).

usage (on the board, as root, camera service stopped):
    link_ladder.py IOP [IOP ...]          lane rate = 3 x IOP Mbit/s, e.g.  link_ladder.py 432 448 456 464
    BIN=/home/radxa/bin/a7zcap.enc link_ladder.py ...      (sudo does not expand ~)

The register at 0x5811400 + 0x118 is the combo-CSI port's **channel-0 interrupt-pending** word (`CMB_PORT_MIPI_CH0_INT_PD`, mask `MIPI_INT_ALL = 0x1fff`), not the
"ECC error count" the neighbouring header suggests. The driver never enables or clears these interrupts, so the bits are **sticky**: they reset when the block is
powered up at stream start and then accumulate for the whole run. That makes the word a per-run "did the link ever go wrong" record, not an error rate.
"""
import mmap, os, re, struct, subprocess, sys, time

PD = 0x5811400 + 0x118
P = "/sys/module/imx519_mipi/parameters/fast_iop"
BIN = os.environ.get("BIN", os.path.expanduser("~/bin/a7zcap.enc"))
here = os.path.dirname(os.path.abspath(__file__))
CHK = next((p for p in (os.path.join(here, "corruption_check.py"), os.path.expanduser("~/corruption_check.py")) if os.path.exists(p)), None)
BITS = {0: "ph_sync", 1: "pf_sync", 2: "frame_start", 3: "frame_end", 4: "line_start", 5: "line_end", 6: "embed",
        7: "frame_sync_ERR", 8: "line_sync_ERR", 9: "ecc_WARN", 10: "ecc_ERR", 11: "chksum_ERR", 12: "eot_ERR"}
ERRBITS = (7, 8, 9, 10, 11, 12)


def decode(v):
    return ",".join(n for b, n in BITS.items() if v >> b & 1 and b in ERRBITS) or "-"


def run(iop, n=90):
    open(P, "w").write(str(iop))
    os.makedirs("/tmp/cap", exist_ok=True)
    for f in os.listdir("/tmp/cap"):
        if f.startswith("ll_"): os.remove("/tmp/cap/" + f)
    fd = os.open("/dev/mem", os.O_RDONLY | os.O_SYNC)
    m = mmap.mmap(fd, 0x1000, mmap.MAP_SHARED, mmap.PROT_READ, offset=PD & ~0xfff)
    rd = lambda: struct.unpack_from("<I", m, PD & 0xfff)[0]
    for _ in range(4):                                  # a 2-in-1 start can come up with the left ISP half empty (exit 6); just start again
        env = dict(os.environ, A7Z_MERGE="1", A7Z_FPS="15", A7Z_EVERY="100000", A7Z_NBUF="4")
        p = subprocess.Popen([BIN, "4656", "3496", str(n), "/tmp/cap/ll"], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        acc, t0 = 0, time.time()
        while p.poll() is None and time.time() - t0 < 90:
            acc |= rd(); time.sleep(0.05)
        if p.returncode != 6: break
    m.close(); os.close(fd)
    fr = sorted(f for f in os.listdir("/tmp/cap") if f.startswith("ll_"))
    score = None
    if fr and CHK:
        out = subprocess.run([sys.executable, CHK, "/tmp/cap/" + fr[-1]], capture_output=True, text=True).stdout
        mm = re.search(r"outliers=([\d.]+)/Mpx", out)
        score = float(mm[1]) if mm else None
    return acc, score, bool(fr), p.returncode


def main():
    iops = [int(a) for a in sys.argv[1:]] or [432]
    orig = open(P).read().strip()
    print("%-10s %-7s %-10s %-8s %s" % ("lane Mb/s", "frames", "outl/Mpx", "int_pd", "error bits that latched during the run"))
    try:
        for iop in iops:
            acc, score, ok, rc = run(iop)
            print("%-10d %-7s %-10s %-8s %s" % (iop * 3, "yes" if ok else "NONE", "-" if score is None else "%.1f" % score, "0x%03x" % acc, decode(acc)), flush=True)
    finally:
        open(P, "w").write(orig)


if __name__ == "__main__":
    main()
