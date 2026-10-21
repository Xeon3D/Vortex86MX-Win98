"""pefix.py - post-link fix-ups for R6040.SYS (Open Watcom wlink output).

 * .reloc VirtualSize = size of the base relocation directory (wlink leaves 0)
 * subsystem/OS version 4.0, DLL characteristics 0
 * file characteristics 0x010E (executable, stripped, 32-bit) like RDCPCI5.SYS
 * PE checksum recomputed (the standard ImageHlp algorithm)

The base relocations are rebuilt: with objalign=512, wlink writes 4 KB
windows that start at the section-aligned address (0x200/0x1200/0x2200)
while each 12-bit offset is relative to the 4 KB page below, so every fix-up
hit the wrong bytes once Windows 98's PE
loader (VXDLDR, which always relocates: sections live in separate heap
blocks) applied it.  The entries are regrouped by real 4 KB page and every
target is checked to lie inside a section's initialised data.

Usage: python pefix.py file.sys
"""
import struct
import sys


def checksum(data, csum_off):
    total = 0
    n = len(data)
    for i in range(0, n - n % 2, 2):
        if i in (csum_off, csum_off + 2):
            continue
        total += data[i] | (data[i + 1] << 8)
        total = (total & 0xFFFF) + (total >> 16)
    if n % 2:
        total += data[-1]
        total = (total & 0xFFFF) + (total >> 16)
    total = (total & 0xFFFF) + (total >> 16)
    return (total + n) & 0xFFFFFFFF


def main(path):
    d = bytearray(open(path, 'rb').read())
    pe = struct.unpack_from('<I', d, 0x3C)[0]
    assert d[pe:pe + 4] == b'PE\0\0'
    nsec = struct.unpack_from('<H', d, pe + 6)[0]
    opt = pe + 24
    optsize = struct.unpack_from('<H', d, pe + 20)[0]
    assert struct.unpack_from('<H', d, opt)[0] == 0x10B
    struct.pack_into('<HH', d, opt + 40, 4, 0)      # OS version
    struct.pack_into('<HH', d, opt + 48, 4, 0)      # subsystem version
    struct.pack_into('<H', d, opt + 70, 0)          # DllCharacteristics
    struct.pack_into('<H', d, pe + 22, 0x010E)      # file characteristics
    base = struct.unpack_from('<I', d, opt + 28)[0]
    reloc_rva, reloc_size = struct.unpack_from('<II', d, opt + 96 + 5 * 8)
    sec = opt + optsize
    secs = []
    for i in range(nsec):
        s = sec + 40 * i
        vsize, va, rawsize, rawptr = struct.unpack_from('<IIII', d, s + 8)
        secs.append((s, va, vsize, rawsize, rawptr))

    def file_off(rva, n=4):
        for s, va, vsize, rawsize, rawptr in secs:
            if va <= rva and rva + n <= va + min(vsize or rawsize, rawsize):
                return rawptr + rva - va
        return None

    rsec = [x for x in secs if x[1] == reloc_rva]
    assert rsec, 'no .reloc section'
    rs, rva_, rvs, rraw, rptr = rsec[0]
    # collect the real fix-up RVAs
    fixups = []
    p, end = rptr, rptr + reloc_size
    while p + 8 <= end:
        page, size = struct.unpack_from('<II', d, p)
        if size < 8:
            break
        for k in range((size - 8) // 2):
            e = struct.unpack_from('<H', d, p + 8 + 2 * k)[0]
            typ, off = e >> 12, e & 0xFFF
            if typ == 0:
                continue
            assert typ == 3, 'relocation type %d' % typ
            # wlink's window is [page, page + 4K) but the 12-bit offset is
            # relative to the 4 KB page below 'page'; entries from the upper
            # part of the window wrap around
            rva = (page & ~0xFFF) + off
            if rva < page:
                rva += 0x1000
            fixups.append(rva)
        p += size
    fixups = sorted(set(fixups))
    for f in fixups:
        fo = file_off(f)
        assert fo is not None, 'fix-up %#x outside sections' % f
        target = struct.unpack_from('<I', d, fo)[0] - base
        assert file_off(target, 1) is not None or any(
            va <= target < va + max(vsize, rawsize) for s, va, vsize, rawsize, rawptr in secs),             'fix-up at %#x points outside the image (%#x)' % (f, target)
    # rebuild the blocks
    out = bytearray()
    pages = sorted(set(f & ~0xFFF for f in fixups))
    for pg in pages:
        ents = [0x3000 | (f - pg) for f in fixups if f & ~0xFFF == pg]
        if len(ents) % 2:
            ents.append(0)
        out += struct.pack('<II', pg, 8 + 2 * len(ents))
        out += struct.pack('<%dH' % len(ents), *ents)
    assert len(out) <= rraw, '.reloc grew beyond its raw size'
    d[rptr:rptr + rraw] = bytes(out) + bytes(rraw - len(out))
    struct.pack_into('<I', d, opt + 96 + 5 * 8 + 4, len(out))
    struct.pack_into('<I', d, rs + 8, len(out))     # .reloc VirtualSize
    print('pefix: %d fix-ups in %d pages' % (len(fixups), len(pages)))
    struct.pack_into('<I', d, opt + 64, 0)
    struct.pack_into('<I', d, opt + 64, checksum(d, opt + 64))
    open(path, 'wb').write(d)


if __name__ == '__main__':
    main(sys.argv[1])
