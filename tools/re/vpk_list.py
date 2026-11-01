#!/usr/bin/env python3
"""List files in a Source VPK (v1/v2 directory file) whose path contains every given substring.
usage: vpk_list.py pak01_dir.vpk substr [substr ...]"""
import struct, sys

def entries(path):
    d = open(path, 'rb').read()
    sig, ver = struct.unpack_from('<II', d, 0)
    assert sig == 0x55AA1234, 'not a VPK'
    if ver == 2:
        tree_size = struct.unpack_from('<I', d, 8)[0]; pos = 28
    else:
        tree_size = struct.unpack_from('<I', d, 8)[0]; pos = 12
    end = pos + tree_size
    def cstr():
        nonlocal pos
        e = d.index(b'\0', pos); s = d[pos:e].decode('latin1'); pos = e + 1; return s
    while pos < end:
        ext = cstr()
        if not ext: break
        while True:
            p = cstr()
            if not p: break
            while True:
                n = cstr()
                if not n: break
                crc, pre, arch, off, ln, term = struct.unpack_from('<IHHIIH', d, pos); pre_pos = pos + 18; pos += 18 + pre
                yield f"{p}/{n}.{ext}".strip('/ '), arch, off, ln, pre, pre_pos

if __name__ == '__main__':
    subs = [s.lower() for s in sys.argv[2:]]
    for name, arch, off, ln, pre, _ in entries(sys.argv[1]):
        if all(s in name.lower() for s in subs):
            print(f"{name}\tarchive {arch}\toffset {off}\tlen {ln}")
