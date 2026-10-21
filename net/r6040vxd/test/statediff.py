"""Compare MAC registers + adapter state: fresh init vs after unplug/replug."""
import sys
sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
import emu_r6040 as T

SYS = sys.argv[1] if len(sys.argv) > 1 else 'r6040.sys'
T.check = lambda c, m: None


def send(e, n):
    for i in range(n):
        pkt, _ = e.make_packet([bytes([i & 255]) * 600])
        e.call(e.h['Send'], e.adapter, pkt, 0)


def state(e):
    regs = {k: v for k, v in e.dev.regs.items() if k not in (0x14, 0x24, 0x28, 0x20)}
    raw = bytes(e.uc.mem_read(e.adapter, 0x900))
    return regs, raw


# fresh boot, some traffic, a few CheckForHang ticks
a = T.boot(SYS, {})
send(a, 40)
a.interrupt()
for _ in range(3):
    a.call(a.h['CheckForHang'], a.adapter)
ra, sa = state(a)

# same, then: unplug (MAC holds frames without link), traffic, ~16 s, replug
b = T.boot(SYS, {})
send(b, 40)
b.interrupt()
b.dev.cable = False
b.dev.tx_autocomplete = False
for _ in range(8):
    send(b, 3)
    b.call(b.h['CheckForHang'], b.adapter)
b.dev.cable = True
b.dev.tx_autocomplete = True
b.dev.tx_run()
b.interrupt()
for _ in range(6):
    b.call(b.h['CheckForHang'], b.adapter)
send(b, 40)
b.interrupt()
for _ in range(3):
    b.call(b.h['CheckForHang'], b.adapter)
rb, sb = state(b)

print('MAC register differences:')
for k in sorted(set(ra) | set(rb)):
    if ra.get(k) != rb.get(k):
        print('  reg %02X: fresh=%04X replug=%04X' % (k, ra.get(k, 0), rb.get(k, 0)))
print('MIER fresh=%04X replug=%04X' % (ra.get(0x40, 0), rb.get(0x40, 0)))
diff = [i for i in range(len(sa)) if sa[i] != sb[i]]
print('adapter struct bytes differing: %d at offsets %s' % (len(diff), [hex(x) for x in diff[:40]]))
print('RESOURCES returned during unplug:', b.send_res, 'SendResourcesAvailable calls')
