#!/usr/bin/env python3
"""Which MIPI packet types and virtual channels does the receiver see while streaming? (checklist B17)

usage (on the board, as root, while a stream runs):  sudo mipi_types.py [SECONDS] [BASE]
Samples the combo-CSI channel-0 packet-header register (0x11c) of mipi1 as fast as /dev/mem allows and prints, per (data type, virtual channel), the share of samples
and the commonest word counts. Short packets show as dt 0x00 frame start / 0x01 frame end; pixel lines as 0x2b RAW10; embedded data would be 0x12, vendor data 0x30-0x37.
"""
import collections, mmap, os, struct, sys, time
secs = float(sys.argv[1]) if len(sys.argv) > 1 else 8
base = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0x5811400
m = mmap.mmap(os.open("/dev/mem", os.O_RDONLY | os.O_SYNC), 0x1000, mmap.MAP_SHARED, mmap.PROT_READ, offset=base & ~0xfff)
o = (base & 0xfff) + 0x11c
c, wc, n, t = collections.Counter(), collections.defaultdict(collections.Counter), 0, time.time()
while time.time() - t < secs:
    v = struct.unpack_from("<I", m, o)[0]
    k = (v & 0x3f, (v >> 6) & 3)
    c[k] += 1
    n += 1
    if k[0] >= 0x10:
        wc[k][v >> 16] += 1
print("samples", n)
for k, v in sorted(c.items()):
    print("dt=0x%02x vc=%d  %.3f%%  %s" % (k[0], k[1], 100 * v / n, ("word counts %s" % dict(wc[k].most_common(3))) if k in wc else ""))
