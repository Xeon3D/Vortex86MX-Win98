"""nefix.py <driver.drv>

Post-link fix for the 16-bit display driver built with Open Watcom 2.0:
the linker writes NE flags 0x8301 and marks the DATA segment MOVEABLE even
though the link script asks for FIXED. The official VMDisp9x binary (which is
known to load on the eBOX) has flags 0x8201 and a FIXED data segment, so make
ours identical.
"""
import struct, sys

NE_FLAGS_WANTED = 0x8201
SEG_MOVEABLE = 0x0010
SEG_DATA = 0x0001

def fix(path):
    d = bytearray(open(path, 'rb').read())
    ne = struct.unpack_from('<I', d, 0x3c)[0]
    assert d[ne:ne+2] == b'NE', "not an NE file"
    flags = struct.unpack_from('<H', d, ne + 0x0c)[0]
    nseg = struct.unpack_from('<H', d, ne + 0x1c)[0]
    segoff = struct.unpack_from('<H', d, ne + 0x22)[0]
    changed = []
    if flags != NE_FLAGS_WANTED:
        struct.pack_into('<H', d, ne + 0x0c, NE_FLAGS_WANTED)
        changed.append("flags %04X -> %04X" % (flags, NE_FLAGS_WANTED))
    for i in range(nseg):
        o = ne + segoff + i * 8
        sflags = struct.unpack_from('<H', d, o + 4)[0]
        if (sflags & SEG_DATA) and (sflags & SEG_MOVEABLE):
            struct.pack_into('<H', d, o + 4, sflags & ~SEG_MOVEABLE)
            changed.append("seg%d flags %04X -> %04X" % (i + 1, sflags, sflags & ~SEG_MOVEABLE))
    open(path, 'wb').write(d)
    print("nefix %s: %s" % (path, ", ".join(changed) if changed else "nothing to do"))

for p in sys.argv[1:]:
    fix(p)
