#!/usr/bin/env python3
"""Extract the ISP602 default parameter struct from YOUR OWN installed libisp_ini.so.

The Allwinner library is closed source and cannot be redistributed, so this repo ships no tuning data.
libisp reads /mnt/isp_param_config.bin = [u32 size][20B][50B note][param struct]; the param struct is four blocks that libisp_ini.so
exports as data symbols (the plain ov13850_mipi_isp_* set; it is what libisp falls back to without a param file):
    test(124) + 3a(2724) + tuning(99440) + iso(13996) = 116284 bytes

usage: extract_isp_defaults.py [/usr/lib/aarch64-linux-gnu/libisp_ini.so] OUT.bin
"""
import struct, sys

BLOCKS = [("ov13850_mipi_isp_test_settings", 124),
          ("ov13850_mipi_isp_3a_settings", 2724),
          ("ov13850_mipi_isp_tuning_settings", 99440),
          ("ov13850_mipi_isp_iso_settings", 13996)]

def main():
    args = sys.argv[1:]
    if not args or len(args) > 2:
        sys.exit(__doc__)
    lib = args[0] if len(args) == 2 else "/usr/lib/aarch64-linux-gnu/libisp_ini.so"
    out = args[-1]
    d = open(lib, "rb").read()
    if d[:4] != b"\x7fELF" or d[4] != 2:
        sys.exit("not a 64-bit ELF file: " + lib)
    shoff, = struct.unpack_from("<Q", d, 0x28)
    shentsize, shnum = struct.unpack_from("<HH", d, 0x3A)
    sec = [struct.unpack_from("<IIQQQQIIQQ", d, shoff + i * shentsize) for i in range(shnum)]
    # (name, type, flags, addr, offset, size, link, info, align, entsize)
    syms = {}
    for s in sec:
        if s[1] in (2, 11):                      # SHT_SYMTAB / SHT_DYNSYM
            strtab = sec[s[6]]
            for o in range(s[4], s[4] + s[5], 24):
                nm, info, other, shndx, val, size = struct.unpack_from("<IBBHQQ", d, o)
                e = d.index(b"\0", strtab[4] + nm)
                syms[d[strtab[4] + nm:e].decode()] = (shndx, val, size)
    blob = b""
    for name, want in BLOCKS:
        if name not in syms:
            sys.exit("symbol %s not found in %s (different libisp version?)" % (name, lib))
        shndx, val, size = syms[name]
        if size != want:
            sys.exit("symbol %s has size %d, expected %d (different libisp version?)" % (name, size, want))
        s = sec[shndx]
        blob += d[s[4] + (val - s[3]): s[4] + (val - s[3]) + size]
    open(out, "wb").write(blob)
    print("wrote %s (%d bytes) from %s" % (out, len(blob), lib))

main()
