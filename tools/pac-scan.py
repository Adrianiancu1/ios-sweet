#!/usr/bin/env python3
"""Count ARMv8.3+ instructions in the executable mappings of a dyld shared cache.

  pac-scan.py DIR_OR_FILES...

Takes the main cache file and its subcaches (dyld_shared_cache_arm64e,
.01, .02, ...; .symbols is skipped). Only mappings with execute
permission are scanned. Galaxy S20+ cores (Cortex-A55/A76, Exynos M5)
are ARMv8.2: "hint" PAC instructions run as NOPs there, everything in
the other groups raises an undefined-instruction exception.
"""
import os
import struct
import sys

import numpy as np

# (name, mask, value, runs on ARMv8.2 as NOP?)
PATTERNS = [
    ("PACIASP/PACIBSP/AUTIASP/AUTIBSP/..Z/1716 (hint)", 0xfffffd1f, 0xd503211f, True),
    ("XPACLRI (hint)", 0xffffffff, 0xd50320ff, True),
    ("RETAA/RETAB", 0xfffffbff, 0xd65f0bff, False),
    ("BRAAZ/BRABZ", 0xfffff81f, 0xd61f081f, False),
    ("BLRAAZ/BLRABZ", 0xfffff81f, 0xd63f081f, False),
    ("BRAA/BRAB", 0xfffff800, 0xd71f0800, False),
    ("BLRAA/BLRAB", 0xfffff800, 0xd73f0800, False),
    ("LDRAA/LDRAB", 0xff200400, 0xf8200400, False),
    ("PACIA/PACIB/PACDA/PACDB/AUT* (+Z)", 0xffffc000, 0xdac10000, False),
    ("XPACI/XPACD", 0xfffffbe0, 0xdac143e0, False),
    ("PACGA", 0xffe0fc00, 0x9ac03000, False),
    ("FJCVTZS (v8.3 JSCVT)", 0xfffffc00, 0x1e7e0000, False),
    ("FCMLA vector (v8.3 FCMA)", 0xbf20e400, 0x2e00c400, False),
]


def exec_ranges(path):
    with open(path, "rb") as f:
        hdr = f.read(0x20)
    if not hdr.startswith(b"dyld_v1"):
        return []
    moff, mcount = struct.unpack_from("<II", hdr, 0x10)
    with open(path, "rb") as f:
        f.seek(moff)
        raw = f.read(32 * mcount)
    out = []
    for i in range(mcount):
        _addr, size, foff, _maxp, initp = struct.unpack_from("<QQQII", raw, 32 * i)
        if initp & 4 and size:
            out.append((foff, size))
    return out


def files(args):
    for a in args:
        if os.path.isdir(a):
            for n in sorted(os.listdir(a)):
                if n.startswith("dyld_shared_cache_") and not n.endswith((".symbols", ".map", ".atlas")):
                    yield os.path.join(a, n)
        else:
            yield a


def main():
    counts = {p[0]: 0 for p in PATTERNS}
    total = 0
    for path in files(sys.argv[1:]):
        ranges = exec_ranges(path)
        with open(path, "rb") as f:
            for foff, size in ranges:
                f.seek(foff)
                remaining = size & ~3
                while remaining:
                    chunk = f.read(min(remaining, 256 << 20))
                    if not chunk:
                        break
                    remaining -= len(chunk)
                    w = np.frombuffer(chunk[: len(chunk) & ~3], dtype="<u4")
                    total += w.size
                    for name, mask, val, _hint in PATTERNS:
                        counts[name] += int(np.count_nonzero((w & np.uint32(mask)) == np.uint32(val)))
        print(f"# {os.path.basename(path)}: {len(ranges)} exec mapping(s)", file=sys.stderr)
    print(f"instructions scanned: {total}")
    bad = 0
    for name, _m, _v, hint in PATTERNS:
        print(f"{counts[name]:>10}  {'nop ' if hint else 'TRAP'}  {name}")
        if not hint:
            bad += counts[name]
    print(f"{bad:>10}  TRAP  total that must be rewritten or emulated")


if __name__ == "__main__":
    main()
