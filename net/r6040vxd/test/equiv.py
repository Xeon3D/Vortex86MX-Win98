"""equiv.py - run one scenario on two driver binaries and diff what they do.

Records every port write (port, value) and every NDIS call (name) while
both builds go through the same life cycle: init, a send burst, a TX ring
overflow, received frames, a cable unplug/replug, CheckForHang ticks,
reset, shutdown and halt.  Prints the differences.

Usage: python test/equiv.py old.sys new.sys
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import emu_r6040 as T

T.check = lambda c, m: None


def run(path):
    trace = []
    orig_out = T.Emu.on_out

    def on_out(self, uc, port, size, value, _):
        trace.append(('out', port, value & 0xFFFF))
        return orig_out(self, uc, port, size, value, _)

    T.Emu.on_out = on_out
    try:
        e = T.boot(path, {})
        mac = T.MAC
        for i in range(20):
            pkt, _ = e.make_packet([bytes([i]) * 300])
            e.call(e.h['Send'], e.adapter, pkt, 0)
            if i % 4 == 3:
                e.interrupt()
        e.dev.tx_autocomplete = False
        for i in range(18):
            pkt, _ = e.make_packet([bytes([i]) * 100])
            e.call(e.h['Send'], e.adapter, pkt, 0)
        e.dev.tx_run()
        e.dev.tx_autocomplete = True
        e.interrupt()
        for i in range(5):
            e.dev.rx_inject(bytes([0xFF] * 6) + mac + b'\x08\x06' + bytes(46 + i))
        e.interrupt()
        for _ in range(3):
            e.call(e.h['CheckForHang'], e.adapter)
        e.dev.cable = False
        for _ in range(6):
            e.call(e.h['CheckForHang'], e.adapter)
        e.dev.cable = True
        for _ in range(5):
            e.call(e.h['CheckForHang'], e.adapter)
        ar = e.alloc(1, 'b')
        e.call(e.h['Reset'], ar, e.adapter)
        e.call(e.shutdown[1], e.shutdown[0])
        e.call(e.h['Halt'], e.adapter)
        calls = [c for c in e.calls]
        return trace, calls, dict(e.written)
    finally:
        T.Emu.on_out = orig_out


def main(a, b):
    ta, ca, wa = run(a)
    tb, cb, wb = run(b)
    print('port writes: %d vs %d, NDIS calls: %d vs %d' % (len(ta), len(tb), len(ca), len(cb)))
    print('NDIS call sequence identical:', ca == cb)
    if ca != cb:
        for i, (x, y) in enumerate(zip(ca, cb)):
            if x != y:
                print('  first call difference at %d: %s vs %s' % (i, x, y))
                break
    diffs = [(i, x, y) for i, (x, y) in enumerate(zip(ta, tb)) if x != y]
    print('port write sequence identical:', ta == tb, '(%d differing positions)' % len(diffs))
    for i, x, y in diffs[:12]:
        print('  #%d %s vs %s' % (i, x, y))
    for k in sorted(set(wa) | set(wb)):
        if wa.get(k) != wb.get(k):
            print('  registry %s: %r vs %r' % (k, wa.get(k), wb.get(k)))


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
