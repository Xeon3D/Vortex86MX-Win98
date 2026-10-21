"""emu_r6040.py - run R6040.SYS under unicorn against fake NDIS + a fake R6040.

There is no R6040 on the build PC, so this harness checks the driver's logic
before it goes to the eBOX: the import calling conventions, ring handling,
filter programming, the receive indication through the Win98 miniport block
offsets, TransferData, the RESOURCES/SendResourcesAvailable path, reset,
shutdown and halt (everything freed, nothing pending), the init failure path
and the bounded hardware waits.

Usage: python test/emu_r6040.py [path/to/r6040.sys]
Needs: pip install unicorn pefile
"""
import struct
import sys
import zlib

import pefile
from unicorn import (Uc, UcError, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE,
                     UC_HOOK_INSN)
from unicorn.x86_const import (UC_X86_REG_EAX, UC_X86_REG_ESP, UC_X86_REG_EIP,
                               UC_X86_INS_IN, UC_X86_INS_OUT, UC_X86_REG_EFLAGS)

IO = 0xE800
STUB_BASE = 0x00050000
RET_MAGIC = 0x00060000
STACK_TOP = 0x00280000
STACK2_TOP = 0x002C0000
HEAP_BASE = 0x00400000
SECT_BASE = 0x01000000
MB = 0x00380000           # fake NDIS_MINIPORT_BLOCK
FILTER_COOKIE = 0x0F117E50
MAC = bytes([0x00, 0x1B, 0xEB, 0x12, 0x34, 0x56])

S_OK = 0
S_FAIL = 0xC0000001
S_RES = 0xC000009A


def u32(b, o=0):
    return struct.unpack_from('<I', b, o)[0]


def bitrev32(x):
    return int('{:032b}'.format(x)[::-1], 2)


class Fail(Exception):
    pass


class R6040Model:
    """Just enough of the MAC for the driver's code paths."""

    def __init__(self, emu):
        self.emu = emu
        self.regs = {}
        self.pending = 0
        self.phy = {0: 0x3100, 1: 0x786D, 2: 0x0243, 3: 0x0C54, 4: 0x01E1, 5: 0x45E1}
        self.phy_addr = 1
        self.mdio_stuck = False
        self.reset_stuck = False
        self.tx_autocomplete = True
        self.sent = []
        self.tx_hw = None
        self.rx_hw = None
        for i, r in enumerate((0x68, 0x6A, 0x6C)):
            self.regs[r] = MAC[2 * i] | (MAC[2 * i + 1] << 8)
        self.regs[0xB6] = 0x0030
        self.regs[0x88] = 0x8100            # PHY status change, PHY addr 1
        self.cable = True                   # False: no link, AN never completes
        self.storm = False                  # MISR stuck at RX_DONE
        self.mier_at_misr = []              # MIER value at each MISR read
        self.phy_writes = []                # (addr, reg, value)
        self.counters = {}                  # 0x50..0x5E, reading clears
        self.vendor = {(30, 17): 0x2000}    # RDC internal PHY registers
        self.an_countdown = 0               # BMSR reads until AN completes
        self.tx_lazy = False                # TX_POLL lost while engine busy
        self.tx_engine_busy = False
        self.tx_batch = []
        self.polls = 0
        self.lpa_after_an = 0x45E1          # partner abilities after AN
        self.an_restarts = 0

    def rd(self, r):
        if r == 0x3C:
            self.mier_at_misr.append(self.regs.get(0x40, 0))
            if self.storm:
                return 0x0001
            v = self.pending
            self.pending = 0
            return v
        if r == 0x8A:                       # MAC's PHY poller result
            return 0x0007 if self.cable and self.regs.get(0x88, 0) & 0x8000 else 0
        if 0x50 <= r <= 0x5E:
            return self.counters.pop(r, 0)
        return self.regs.get(r, 0)

    def phy_value(self, addr, reg):
        if (addr, reg) in self.vendor:
            return self.vendor[(addr, reg)]
        if addr != self.phy_addr:
            return 0xFFFF
        v = self.phy.get(reg, 0)
        if reg == 1 and self.an_countdown:
            self.an_countdown -= 1
            v &= ~0x0024
            if self.an_countdown == 0 and self.cable:
                self.phy[5] = self.lpa_after_an
        if reg == 1 and not self.cable:
            v &= ~0x0024                    # no link, AN not complete
        if reg == 5 and not self.cable:
            v = 0
        return v

    def wr(self, r, v):
        if r == 0x04:
            if v & 1:
                self.regs[0x88] = 0             # MAC reset clears MPSCCR
            v = v if self.reset_stuck else v & ~1
            if (v & 1) == 0:
                self.tx_hw = self.rx_hw = None
        elif r == 0x20:
            if self.mdio_stuck:
                self.regs[r] = v
                return
            addr, reg = (v >> 8) & 31, v & 31
            if v & 0x2000:
                self.regs[0x24] = self.phy_value(addr, reg)
            elif v & 0x4000 and (addr, reg) in self.vendor:
                self.phy_writes.append((addr, reg, self.regs.get(0x28, 0)))
                self.vendor[(addr, reg)] = self.regs.get(0x28, 0)
            elif v & 0x4000 and addr != self.phy_addr:
                self.phy_writes.append((addr, reg, self.regs.get(0x28, 0)))
            elif v & 0x4000 and addr == self.phy_addr:
                d = self.regs.get(0x28, 0)
                self.phy_writes.append((addr, reg, d))
                if reg == 0 and d & 0x0200:
                    d &= ~0x0200
                    self.phy[1] = 0x786D
                    self.an_countdown = 6       # renegotiating
                    self.an_restarts += 1
                self.phy[reg] = d
            v &= ~0x6000
        elif r == 0x14 and v & 1:
            self.regs[r] = 0
            self.polls += 1
            if self.tx_lazy:
                # a poll while the engine is busy is lost; otherwise the
                # engine takes the frames queued right now (sent at finish)
                if not self.tx_engine_busy:
                    self.tx_engine_busy = True
                    self.tx_batch = self.tx_owned()
                return
            if self.tx_autocomplete:
                self.tx_run()
            return
        self.regs[r] = v

    def tx_owned(self):
        mem = self.emu.uc
        if self.tx_hw is None:
            self.tx_hw = self.ring(0x2C, 0x30)
        out, a = [], self.tx_hw
        for _ in range(64):
            st, ln, buf, nxt = struct.unpack('<HHII', mem.mem_read(a, 12))
            if not st & 0x8000:
                break
            out.append((a, ln, buf, nxt))
            a = nxt
        return out

    def tx_finish(self):
        """lazy engine: the batch taken at the poll goes out, engine idles"""
        mem = self.emu.uc
        for a, ln, buf, nxt in self.tx_batch:
            self.sent.append(bytes(mem.mem_read(buf, ln)))
            mem.mem_write(a, struct.pack('<H', 0x4000))
            self.tx_hw = nxt
            self.pending |= 0x0010
        self.tx_batch = []
        self.tx_engine_busy = False

    def ring(self, lo, hi):
        return self.regs.get(lo, 0) | (self.regs.get(hi, 0) << 16)

    def tx_run(self):
        mem = self.emu.uc
        if self.tx_hw is None:
            self.tx_hw = self.ring(0x2C, 0x30)
        while True:
            d = bytes(mem.mem_read(self.tx_hw, 16))
            st, ln, buf, nxt = struct.unpack_from('<HHII', d)
            if not st & 0x8000:
                break
            self.sent.append(bytes(mem.mem_read(buf, ln)))
            mem.mem_write(self.tx_hw, struct.pack('<H', 0x4000))
            self.pending |= 0x0010
            self.tx_hw = nxt

    def rx_inject(self, frame, status=0x4000):
        mem = self.emu.uc
        if self.rx_hw is None:
            self.rx_hw = self.ring(0x34, 0x38)
        d = bytes(mem.mem_read(self.rx_hw, 16))
        st, ln, buf, nxt = struct.unpack_from('<HHII', d)
        if not st & 0x8000:
            return False
        if ln < len(frame) + 4:
            raise Fail('RX buffer length %d too small' % ln)
        mem.mem_write(buf, frame + b'\xde\xad\xbe\xef')
        mem.mem_write(self.rx_hw, struct.pack('<HH', status, len(frame) + 4))
        self.pending |= 0x0001
        self.rx_hw = nxt
        return True

    def rx_skip(self):
        if self.rx_hw is None:
            self.rx_hw = self.ring(0x34, 0x38)
        self.rx_hw = struct.unpack('<I', self.emu.uc.mem_read(self.rx_hw + 8, 4))[0]

    def irq(self):
        return (self.pending & self.regs.get(0x40, 0)) != 0


class Emu:
    def __init__(self, path):
        self.pe = pefile.PE(path)
        self.uc = uc = Uc(UC_ARCH_X86, UC_MODE_32)
        # Load like Win98's VXDLDR: every section in its own memory block
        # (never at the preferred base), base relocations applied through an
        # RVA -> address map, imports bound by name through FirstThunk.
        pe = self.pe
        imgbase = pe.OPTIONAL_HEADER.ImageBase
        raw = open(path, 'rb').read()
        self.secmap = []
        for i, sec in enumerate(pe.sections):
            addr = SECT_BASE + i * 0x20000 + 0x1230      # scattered, unaligned
            n = max(sec.Misc_VirtualSize, sec.SizeOfRawData)
            data = raw[sec.PointerToRawData:sec.PointerToRawData + min(sec.SizeOfRawData, n)]
            self.secmap.append((sec.VirtualAddress, n, addr))
        uc.mem_map(SECT_BASE, 0x20000 * len(pe.sections) + 0x10000)
        for (va, n, addr), sec in zip(self.secmap, pe.sections):
            uc.mem_write(addr, bytes(n))
            uc.mem_write(addr, raw[sec.PointerToRawData:sec.PointerToRawData +
                                   min(sec.SizeOfRawData, n)])
        nrel = 0
        for b in getattr(pe, 'DIRECTORY_ENTRY_BASERELOC', []):
            for ent in b.entries:
                if ent.type == 0:
                    continue
                if ent.type != 3:
                    raise Fail('relocation type %d' % ent.type)
                loc = self.rva2addr(ent.rva, 4)
                val = self.r32(loc) - imgbase
                self.w32(loc, self.rva2addr(val, 1))
                nrel += 1
        self.nrel = nrel
        uc.mem_map(STUB_BASE, 0x10000)
        uc.mem_map(RET_MAGIC, 0x1000)
        uc.mem_map(0x00200000, 0x00200000)       # stacks, block, heap
        uc.mem_map(HEAP_BASE, 0x00400000)
        self.heap = HEAP_BASE
        self.allocs = {}
        self.calls = []
        self.dev = R6040Model(self)
        self.cmos = bytearray(128)
        self.p61 = 0
        self.config = {}
        self.indications = []
        self.rx_completes = 0
        self.nested = None
        self.stubs = {}
        self.pci = bytearray(256)
        struct.pack_into('<IHH', self.pci, 0, 0x604017F3, 0x0001, 0)
        self.resources_ok = True
        self.shutdown = None
        self.intr = None
        self.io_reg = None
        self.status = []
        self.written = {}
        self.param_types = {}

        n = 0
        for imp in self.pe.DIRECTORY_ENTRY_IMPORT:
            for f in imp.imports:
                addr = STUB_BASE + 16 * n
                n += 1
                name = f.name.decode()
                self.stubs[addr] = name
                uc.mem_write(addr, b'\xc3')           # patched below per call
                uc.mem_write(self.rva2addr(f.address - imgbase, 4), struct.pack('<I', addr))
        for extra in ('EthRxIndicate', 'EthRxComplete'):
            addr = STUB_BASE + 16 * n
            n += 1
            self.stubs[addr] = extra
        uc.mem_write(MB, b'\0' * 0x400)
        rev = {v: k for k, v in self.stubs.items()}
        uc.mem_write(MB + 0xD8, struct.pack('<I', FILTER_COOKIE))
        uc.mem_write(MB + 0x164, struct.pack('<I', rev['EthRxIndicate']))
        uc.mem_write(MB + 0x170, struct.pack('<I', rev['EthRxComplete']))

        uc.hook_add(UC_HOOK_CODE, self.on_stub, None, STUB_BASE, STUB_BASE + 0xFFFF)
        uc.hook_add(UC_HOOK_INSN, self.on_in, None, 1, 0, UC_X86_INS_IN)
        uc.hook_add(UC_HOOK_INSN, self.on_out, None, 1, 0, UC_X86_INS_OUT)
        self.entry = self.rva2addr(pe.OPTIONAL_HEADER.AddressOfEntryPoint, 1)

    # ---- memory helpers
    def rva2addr(self, rva, n):
        for va, size, addr in self.secmap:
            if va <= rva and rva + n <= va + size:
                return addr + rva - va
        raise Fail('RVA %#x outside every section (bad relocation?)' % rva)

    def alloc(self, n, tag):
        a = (self.heap + 15) & ~15
        self.heap = a + n
        self.uc.mem_write(a, b'\xcc' * n)
        self.allocs[a] = (n, tag)
        return a

    def r32(self, a):
        return u32(self.uc.mem_read(a, 4))

    def w32(self, a, v):
        self.uc.mem_write(a, struct.pack('<I', v & 0xFFFFFFFF))

    # ---- port I/O
    def on_in(self, uc, port, size, _):
        if port == 0x61:
            self.p61 += 1
            return 0x10 if (self.p61 // 3) & 1 else 0
        if port == 0x71:
            return self.cmos[self.cmos_idx & 0x7F]
        if IO <= port < IO + 0x100:
            return self.dev.rd(port - IO)
        raise Fail('IN from unexpected port %#x' % port)

    def on_out(self, uc, port, size, value, _):
        if port == 0x70:
            self.cmos_idx = value
            return
        if port == 0x71:
            self.cmos[self.cmos_idx & 0x7F] = value & 0xFF
            return
        if IO <= port < IO + 0x100:
            self.dev.wr(port - IO, value & 0xFFFF)
            return
        raise Fail('OUT to unexpected port %#x' % port)

    # ---- calling into the driver
    def call(self, fn, *args, top=STACK_TOP, count=40_000_000):
        uc = self.uc
        esp = top - 4 * (len(args) + 1)
        uc.mem_write(esp, struct.pack('<%dI' % (len(args) + 1), RET_MAGIC,
                                      *[a & 0xFFFFFFFF for a in args]))
        uc.reg_write(UC_X86_REG_ESP, esp)
        uc.reg_write(UC_X86_REG_EFLAGS, 0x202)
        try:
            uc.emu_start(fn, RET_MAGIC, count=count)
        except UcError as e:
            raise Fail('emulation fault in %#x: %s at eip=%#x' %
                       (fn, e, uc.reg_read(UC_X86_REG_EIP)))
        if uc.reg_read(UC_X86_REG_EIP) != RET_MAGIC:
            raise Fail('did not return (instruction budget exhausted: unbounded wait?)')
        if uc.reg_read(UC_X86_REG_ESP) != top:
            raise Fail('stack imbalance after %#x: esp=%#x' %
                       (fn, uc.reg_read(UC_X86_REG_ESP)))
        return uc.reg_read(UC_X86_REG_EAX)

    # ---- NDIS stubs
    def on_stub(self, uc, addr, size, _):
        name = self.stubs[addr]
        esp = uc.reg_read(UC_X86_REG_ESP)
        a = [self.r32(esp + 4 + 4 * i) for i in range(8)]
        nargs, ret = self.ndis(name, a)
        self.calls.append(name)
        uc.reg_write(UC_X86_REG_EAX, ret & 0xFFFFFFFF)
        uc.reg_write(UC_X86_REG_ESP, esp + 4 + 4 * nargs)
        uc.reg_write(UC_X86_REG_EIP, self.r32(esp))

    def ndis(self, name, a):
        if name == 'NdisInitializeWrapper':
            self.w32(a[0], 0x1234)
            return 4, 0
        if name == 'NdisTerminateWrapper':
            return 2, 0
        if name == 'NdisMRegisterMiniport':
            if a[2] != 0x3C:
                raise Fail('characteristics length %#x' % a[2])
            c = bytes(self.uc.mem_read(a[1], 0x3C))
            if c[0] != 3:
                raise Fail('NDIS major version %d' % c[0])
            names = ['CheckForHang', 'DisableInterrupt', 'EnableInterrupt', 'Halt',
                     'HandleInterrupt', 'Initialize', 'ISR', 'Query', 'Reconfigure',
                     'Reset', 'Send', 'Set', 'TransferData']
            self.h = {n: u32(c, 8 + 4 * i) for i, n in enumerate(names)}
            return 3, 0
        if name == 'NdisAllocateMemory':
            self.w32(a[0], self.alloc(a[1], 'mem'))
            return 5, 0
        if name == 'NdisFreeMemory':
            self.free(a[0], a[1], 'mem')
            return 3, 0
        if name == 'NdisOpenConfiguration':
            self.w32(a[0], 0)
            self.w32(a[1], 0x777)
            return 3, 0
        if name == 'NdisReadConfiguration':
            ln = struct.unpack('<H', self.uc.mem_read(a[3], 2))[0]
            kw = bytes(self.uc.mem_read(self.r32(a[3] + 4), ln)).decode('utf-16-le')
            if a[4] not in (0, 1):
                raise Fail('parameter type %d' % a[4])
            self.param_types[kw] = a[4]
            if kw in self.config:
                p = self.alloc(12, 'param')
                self.uc.mem_write(p, struct.pack('<III', 0, self.config[kw], 0))
                self.w32(a[1], p)
                self.w32(a[0], 0)
            else:
                self.w32(a[0], S_FAIL)
            self.read_keys.append(kw)
            return 5, 0
        if name == 'NdisWriteConfiguration':
            ln = struct.unpack('<H', self.uc.mem_read(a[2], 2))[0]
            kw = bytes(self.uc.mem_read(self.r32(a[2] + 4), ln)).decode('utf-16-le')
            typ = self.r32(a[3])
            if typ != 2:
                raise Fail('WriteConfiguration type %d' % typ)
            sl = struct.unpack('<H', self.uc.mem_read(a[3] + 4, 2))[0]
            val = bytes(self.uc.mem_read(self.r32(a[3] + 8), sl)).decode('utf-16-le')
            self.written[kw] = val
            self.w32(a[0], 0)
            return 4, 0
        if name == 'NdisCloseConfiguration':
            return 1, 0
        if name == 'NdisMSetAttributes':
            self.busmaster = a[2] & 0xFF
            return 4, 0
        if name == 'NdisMPciAssignResources':
            if not self.resources_ok:
                return 3, S_FAIL
            rl = self.alloc(8 + 32, 'res')
            self.uc.mem_write(rl, struct.pack('<HHI', 1, 1, 2) +
                              struct.pack('<BBHIiI', 1, 1, 1, IO, 0, 0x100) +
                              struct.pack('<BBHIII', 2, 3, 0, 11, 11, 1))
            self.w32(a[2], rl)
            return 3, 0
        if name == 'NdisMQueryAdapterResources':
            self.w32(a[0], S_FAIL)
            return 4, 0
        if name == 'NdisReadPciSlotInformation':
            self.uc.mem_write(a[3], bytes(self.pci[a[2]:a[2] + a[4]]))
            return 5, a[4]
        if name == 'NdisWritePciSlotInformation':
            self.pci[a[2]:a[2] + a[4]] = bytes(self.uc.mem_read(a[3], a[4]))
            return 5, a[4]
        if name == 'NdisMRegisterIoPortRange':
            self.w32(a[0], a[2])
            self.io_reg = (a[2], a[3])
            return 4, 0
        if name == 'NdisMDeregisterIoPortRange':
            self.io_reg = None
            return 4, 0
        if name == 'NdisMAllocateSharedMemory':
            p = self.alloc(a[1], 'shared')
            self.w32(a[3], p)
            self.w32(a[4], p)
            self.w32(a[4] + 4, 0)
            return 5, 0
        if name == 'NdisMFreeSharedMemory':
            if a[3] != a[4] or a[5] != 0:
                raise Fail('FreeSharedMemory with wrong physical address')
            self.free(a[3], a[1], 'shared')
            return 6, 0
        if name == 'NdisMRegisterInterrupt':
            self.intr = (a[0], a[2], a[3], a[4] & 0xFF, a[5] & 0xFF, a[6])
            return 7, 0
        if name == 'NdisMDeregisterInterrupt':
            self.intr = None
            return 1, 0
        if name == 'NdisMRegisterAdapterShutdownHandler':
            self.shutdown = (a[1], a[2])
            return 3, 0
        if name == 'NdisMDeregisterAdapterShutdownHandler':
            self.shutdown = None
            return 1, 0
        if name == 'NdisMIndicateStatus':
            if a[2] != 0 or a[3] != 0:
                raise Fail('status buffer for media indication')
            self.status.append(a[1])
            return 4, 0
        if name == 'NdisMIndicateStatusComplete':
            self.status.append('complete')
            return 1, 0
        if name == 'NdisMSendResourcesAvailable':
            self.send_res += 1
            return 1, 0
        if name == 'NdisGetFirstBufferFromPacket':
            head = self.r32(a[0] + 8)
            total = 0
            b = head
            while b:
                total += self.r32(b + 8)
                b = self.r32(b)
            self.w32(a[1], head)
            self.w32(a[2], self.r32(head + 4) if head else 0)
            self.w32(a[3], self.r32(head + 8) if head else 0)
            self.w32(a[4], total)
            return 5, 0
        if name == 'NdisQueryBuffer':
            self.w32(a[1], self.r32(a[0] + 4))
            self.w32(a[2], self.r32(a[0] + 8))
            return 3, 0
        if name == 'EthRxIndicate':
            if a[0] != FILTER_COOKIE:
                raise Fail('EthDB argument %#x' % a[0])
            hdr = bytes(self.uc.mem_read(a[3], a[4]))
            la = bytes(self.uc.mem_read(a[5], a[6]))
            if a[2] != a[3]:
                raise Fail('Address != HeaderBuffer')
            td = None
            if self.nested is not None:
                td = self.transfer_nested(a[1], *self.nested)
            self.indications.append((a[1], hdr, la, a[7], td))
            return 8, 0
        if name == 'EthRxComplete':
            if a[0] != FILTER_COOKIE:
                raise Fail('EthDB argument to complete')
            self.rx_completes += 1
            return 1, 0
        raise Fail('unexpected NDIS call %s' % name)

    def free(self, p, n, tag):
        if p not in self.allocs:
            raise Fail('free of unknown block %#x' % p)
        if self.allocs[p] != (n, tag):
            raise Fail('free size/type mismatch %#x: %r vs %r' % (p, self.allocs[p], (n, tag)))
        del self.allocs[p]

    def transfer_nested(self, rx_ctx, offset, count):
        """Call MiniportTransferData from inside the indication."""
        ctx = self.uc.context_save()
        pkt, bufs = self.make_packet([b'\0' * 8, b'\0' * 100])
        res = self.alloc(4, 'res')
        st = self.call(self.h['TransferData'], res, pkt, self.adapter, rx_ctx,
                       offset, count, top=STACK2_TOP)
        n = self.r32(res)
        data = b''.join(bytes(self.uc.mem_read(v, ln)) for v, ln in bufs)[:n]
        self.uc.context_restore(ctx)
        return st, n, data

    # ---- packets
    def make_packet(self, chunks):
        pkt = self.alloc(64, 'pkt')
        self.uc.mem_write(pkt, b'\0' * 64)
        prev = None
        bufs = []
        for c in chunks:
            b = self.alloc(16, 'buf')
            v = self.alloc(max(len(c), 1), 'data')
            self.uc.mem_write(v, c)
            self.uc.mem_write(b, struct.pack('<IIII', 0, v, len(c), 0))
            bufs.append((v, len(c)))
            if prev is None:
                self.w32(pkt + 8, b)
            else:
                self.w32(prev, b)
            prev = b
        return pkt, bufs

    def interrupt(self):
        """Deliver the IRQ if the model asserts it; returns True if handled."""
        if not self.dev.irq():
            return False
        rec = self.alloc(1, 'b')
        q = self.alloc(1, 'b')
        self.call(self.h['ISR'], rec, q, self.adapter)
        recognized = self.uc.mem_read(rec, 1)[0]
        queue = self.uc.mem_read(q, 1)[0]
        if recognized and queue:
            self.call(self.h['HandleInterrupt'], self.adapter)
        return bool(recognized)


def check(cond, msg):
    if not cond:
        raise Fail(msg)
    print('  ok  ' + msg)


def boot(path, config=None, mutate=None):
    e = Emu(path)
    e.config = config if config is not None else {}
    e.read_keys = []
    e.send_res = 0
    if mutate:
        mutate(e)
    check(e.nrel > 0, 'image relocated section by section (%d fix-ups)' % e.nrel)
    st = e.call(e.entry, 0xD0D0, 0xE0E0)
    check(st == 0, 'DriverEntry returns NDIS_STATUS_SUCCESS')
    check(e.cmos[1] == 0x12 and e.cmos[3] == 0x80, 'CMOS: DriverEntry checkpoint 12 (registered)')
    media = e.alloc(8, 'media')
    e.uc.mem_write(media, struct.pack('<II', 6, 0))       # 802.5, 802.3
    oe = e.alloc(4, 'oe')
    sel = e.alloc(4, 'sel')
    adapter_holder = []
    st = e.call(e.h['Initialize'], oe, sel, media, 2, MB, 0x5555)
    e.init_status = st
    if st == 0:
        e.selected = e.r32(sel)
        # adapter context = argument of NdisMSetAttributes (2nd) == ISR context
        e.adapter = [p for p, (n, t) in e.allocs.items() if t == 'mem'][0]
    return e


def query(e, oid, n=256):
    buf = e.alloc(n, 'q')
    w = e.alloc(4, 'q')
    nd = e.alloc(4, 'q')
    st = e.call(e.h['Query'], e.adapter, oid, buf, n, w, nd)
    return st, bytes(e.uc.mem_read(buf, e.r32(w)))


def setinfo(e, oid, data):
    buf = e.alloc(max(len(data), 1), 's')
    e.uc.mem_write(buf, data)
    r = e.alloc(4, 's')
    nd = e.alloc(4, 's')
    return e.call(e.h['Set'], e.adapter, oid, buf, len(data), r, nd)


def ether_crc_be_ref(mac):
    return bitrev32((~zlib.crc32(mac)) & 0xFFFFFFFF)


def main(path):
    print('== normal life cycle')
    e = boot(path, {'R6040Diag': 1, 'R6040TxKick': 0})
    check(e.init_status == 0, 'Initialize succeeds')
    check(e.selected == 1, 'selects NdisMedium802_3 from the medium array')
    check(e.busmaster == 1, 'NdisMSetAttributes(BusMaster=TRUE)')
    check(e.intr is not None and e.intr[1:5] == (11, 11, 1, 1),
          'NdisMRegisterInterrupt(vector 11, level 11, RequestIsr, Shared)')
    check(e.shutdown is not None, 'shutdown handler registered')
    check({'R6040Diag', 'R6040DiagCmos', 'R6040Shutdown', 'ConnectionType'} <= set(e.read_keys),
          'reads its registry parameters (UNICODE keywords)')
    d = e.dev.regs
    check(d.get(0x40) == 0x07, 'MIER: RX interrupts only (0x07); TX reclaimed in Send')
    check(d.get(0x08) == 0x012A and d.get(0x18) == 1536, 'MBCR and RX buffer size programmed')
    check(d.get(0x00, 0) & 0x1002 == 0x1002, 'MCR0 RX and TX enabled')
    check(d.get(0x00, 0) & 0x8000, 'full duplex from 100FD auto-negotiation')
    check([d[0x68], d[0x6A], d[0x6C]] == [0x1B00, 0x12EB, 0x5634], 'station address kept')
    check(e.cmos[1] == 0x2E and e.cmos[3] & 0x81 == 0x81, 'CMOS: checkpoint init-ok, events loaded+init-ok')
    check(e.pci[4] & 5 == 5, 'PCI I/O + bus master enabled')
    for p, (n, t) in e.allocs.items():
        if t == 'shared':
            check(p & 15 == 0, 'shared block %#x 16-byte aligned (%d bytes)' % (p, n))

    st, mac = query(e, 0x01010102)
    check(st == 0 and mac == MAC, 'OID_802_3_CURRENT_ADDRESS = chip MAC')
    st, v = query(e, 0x00010107)
    check(st == 0 and u32(v) == 1000000, 'OID_GEN_LINK_SPEED = 100 Mbps')
    st, v = query(e, 0x00010114)
    check(st == 0 and u32(v) == 0, 'OID_GEN_MEDIA_CONNECT_STATUS = connected')
    st, v = query(e, 0x00010101)
    check(st == 0 and len(v) % 4 == 0 and 0x01010103 in struct.unpack('<%dI' % (len(v) // 4), v),
          'OID_GEN_SUPPORTED_LIST')
    st, v = query(e, 0x00010101, 8)
    check(st == 0xC0010016, 'short buffer -> NDIS_STATUS_BUFFER_TOO_SHORT')
    st, v = query(e, 0x0DEAD)
    check(st == 0xC0010017, 'unknown OID -> NDIS_STATUS_INVALID_OID')

    check(setinfo(e, 0x0001010E, struct.pack('<I', 0x0B)) == 0, 'set filter directed+mc+broadcast')
    mc = bytes([0x01, 0x00, 0x5E, 0x00, 0x00, 0x01])
    check(setinfo(e, 0x01010103, mc) == 0, 'set multicast list')
    bit = ether_crc_be_ref(mc) >> 26
    hashes = [d.get(0x60 + 2 * i, 0) for i in range(4)]
    want = [0, 0, 0, 0]
    want[bit >> 4] |= 1 << (bit & 15)
    check(hashes == want and d[0] & 0x0100, 'multicast hash bit %d set, MCR0 hash enabled' % bit)
    check(setinfo(e, 0x0001010E, struct.pack('<I', 0x20)) == 0 and d[0] & 0x0020,
          'promiscuous sets MCR0_PROMISC')
    check(setinfo(e, 0x0001010E, struct.pack('<I', 0x0B)) == 0 and not d[0] & 0x0020,
          'promiscuous cleared again')
    check(setinfo(e, 0x0001010E, struct.pack('<I', 0x1000)) == 0xC00000BB,
          'unsupported filter bit refused')
    check(setinfo(e, 0x01010103, b'\x01' * 7) == 0xC0010014, 'bad multicast length refused')

    # --- send
    frame = bytes(range(14)) + b'ARP-PAYLOAD-' * 2
    pkt, _ = e.make_packet([frame[:14], frame[14:]])
    st = e.call(e.h['Send'], e.adapter, pkt, 0)
    check(st == 0, 'Send returns SUCCESS synchronously (nothing pending)')
    check(len(e.dev.sent) == 1 and e.dev.sent[0] == frame + b'\0' * (60 - len(frame)),
          'frame on the wire, padded to 60 bytes')
    e.interrupt()
    check('NdisMSendComplete' not in e.calls, 'NdisMSendComplete never called')

    e.dev.tx_autocomplete = False
    results = []
    for i in range(17):
        pkt, _ = e.make_packet([bytes([i]) * 100])
        results.append(e.call(e.h['Send'], e.adapter, pkt, 0))
    check(results[:16] == [0] * 16 and results[16] == S_RES,
          'TX ring full -> NDIS_STATUS_RESOURCES on the 17th send')
    check(d.get(0x40) == 0x17, 'ring full: TX_DONE interrupt enabled (MIER 0x17)')
    e.dev.tx_run()
    e.interrupt()
    check(e.send_res == 1, 'NdisMSendResourcesAvailable after descriptors freed')
    check(d.get(0x40) == 0x07, 'TX_DONE interrupt off again (MIER 0x07)')
    check(len(e.dev.sent) == 17 and e.dev.sent[-1] == b'\x0f' * 100, 'queued frames transmitted')
    e.dev.tx_autocomplete = True

    # --- receive
    rx = [bytes([0xFF] * 6) + MAC + b'\x08\x06' + bytes([i]) * (46 + 10 * i) for i in range(3)]
    e.nested = (10, 20)
    for f in rx:
        e.dev.rx_inject(f)
    e.dev.rx_inject(bytes(64), status=0x0040)                 # CRC error
    check(e.interrupt(), 'ISR recognizes RX interrupt')
    check(len(e.indications) == 3, 'three good frames indicated, CRC error dropped')
    ok = all(ind[1] == f[:14] and ind[2] == f[14:] and ind[3] == len(f) - 14
             for ind, f in zip(e.indications, rx))
    check(ok, 'header/lookahead/packet size correct (FCS stripped)')
    ok = e.nested is None or all(ind[4][0] == 0 and ind[4][2] == f[14 + 10:14 + 30] for ind, f in zip(e.indications, rx))
    check(ok, 'TransferData inside the indication copies the right bytes')
    check(e.rx_completes == 1, 'one NdisMEthIndicateReceiveComplete per batch')
    descs, a = [], e.dev.ring(0x34, 0x38)
    for i in range(32):
        s_, l_, b_, a = struct.unpack('<HHII', e.uc.mem_read(a, 12))
        descs.append((s_, l_))
    check(a == e.dev.ring(0x34, 0x38), 'RX ring is a closed loop of 32 descriptors')
    check(all(s == 0x8000 and ln == 1536 for s, ln in descs), 'all RX descriptors back to the MAC')
    e.nested = None
    st, v = query(e, 0x00020102)
    check(u32(v) == 3, 'OID_GEN_RCV_OK = 3')
    st, v = query(e, 0x00020104)
    check(u32(v) == 1, 'OID_GEN_RCV_ERROR = 1')

    # ring wrap: 70 frames in batches
    e.indications = []
    for i in range(70):
        if not e.dev.rx_inject(bytes([0x01, 0, 0x5E, 0, 0, 1]) + MAC + b'\x08\x00' + bytes(50)):
            e.interrupt()
            e.dev.rx_inject(bytes([0x01, 0, 0x5E, 0, 0, 1]) + MAC + b'\x08\x00' + bytes(50))
    e.interrupt()
    check(len(e.indications) == 70, 'RX ring wraps correctly (70 frames)')

    rec = e.alloc(1, 'b')
    q = e.alloc(1, 'b')
    e.call(e.h['ISR'], rec, q, e.adapter)
    check(e.uc.mem_read(rec, 1)[0] == 0, 'ISR does not claim a shared IRQ that is not ours')

    # --- hang detection / reset
    check(e.call(e.h['CheckForHang'], e.adapter) & 0xFF == 0, 'CheckForHang FALSE when idle')
    e.dev.tx_autocomplete = False
    pkt, _ = e.make_packet([bytes(60)])
    e.call(e.h['Send'], e.adapter, pkt, 0)
    hang = [e.call(e.h['CheckForHang'], e.adapter) & 0xFF for _ in range(4)]
    check(hang[-1] == 1, 'CheckForHang TRUE after TX makes no progress')
    ar = e.alloc(1, 'b')
    check(e.call(e.h['Reset'], ar, e.adapter) == 0, 'Reset completes synchronously')
    check(e.dev.regs.get(0x40) == 0x07 and e.dev.regs[0] & 0x1002 == 0x1002,
          'MAC running again after reset')
    e.dev.tx_autocomplete = True
    pkt, _ = e.make_packet([bytes(range(60))])
    check(e.call(e.h['Send'], e.adapter, pkt, 0) == 0 and e.dev.sent[-1] == bytes(range(60)),
          'send works after reset')

    # --- shutdown then halt
    n_calls = len(e.calls)
    e.call(e.shutdown[1], e.shutdown[0])
    check(len(e.calls) == n_calls, 'shutdown handler makes no NDIS calls')
    check(e.dev.regs.get(0x40) == 0 and not e.dev.regs.get(0, 0) & 0x1002,
          'shutdown: interrupts masked, RX/TX stopped')
    check(e.cmos[1] == 0x5E and e.cmos[3] & 0x0C == 0x0C, 'CMOS: shutdown entered + done')
    e.call(e.h['Halt'], e.adapter)
    check(e.intr is None and e.shutdown is None and e.io_reg is None,
          'halt: interrupt, shutdown handler and I/O range deregistered')
    left = [t for p, (n, t) in e.allocs.items() if t in ('mem', 'shared')]
    check(left == [], 'halt: adapter memory and all 3 shared blocks freed')
    check(e.cmos[1] == 0x4E and e.cmos[3] & 0x30 == 0x30 and e.cmos[5] == 0x40,
          'CMOS: halt done; only the provoked TX-hang warning')

    print('== version identification')
    import os
    import re
    here = os.path.dirname(os.path.abspath(__file__))
    ver = re.search(r'R6040_VERSION_STR\s+"([^"]+)"',
                    open(os.path.join(here, '..', 'version.h')).read()).group(1)
    inf = open(os.path.join(here, '..', 'R6040.INF')).read()
    check('R6040.DeviceDesc = "RDC R6040 Fast Ethernet (open R6040.SYS %s)"' % ver in inf,
          'R6040.INF device name carries the version (%s)' % ver)
    check(e.written.get('R6040Version') == ver, 'registry value R6040Version = "%s"' % ver)
    st, v = query(e, 0x0001010D)
    check(v.rstrip(b'\0').decode().endswith('R6040.SYS %s)' % ver),
          'OID_GEN_VENDOR_DESCRIPTION carries the version')

    print('== robustness (build 3)')
    e = boot(path, {'R6040TxKick': 0})
    check(all(m == 0 for m in e.dev.mier_at_misr), 'MISR is only read with MIER masked')
    check(e.status == [], 'no status indication during Initialize')
    # RX index out of step with the MAC
    e.indications = []
    e.dev.rx_skip()
    f = bytes([0xFF] * 6) + MAC + b'\x08\x06' + bytes(46)
    e.dev.rx_inject(f)
    e.dev.rx_inject(f)
    e.interrupt()
    check(len(e.indications) == 2, 'RX resync: frames after a skipped descriptor still indicated')
    for _ in range(3):
        e.dev.rx_skip()
        e.dev.rx_inject(f)
        e.interrupt()
    check(len(e.indications) == 5, 'RX resync works repeatedly (ring keeps wrapping)')
    # all descriptors used up before the DPC runs
    e.indications = []
    n = 0
    while e.dev.rx_inject(f):
        n += 1
    e.dev.pending |= 0x0006                 # no descriptor + FIFO full
    e.interrupt()
    check(n == 32 and len(e.indications) == 32, 'RX ring completely full: all 32 indicated')
    check(e.dev.rx_inject(f), 'descriptors re-armed after overflow')
    e.interrupt()
    check(len(e.indications) == 33, 'reception continues after overflow')
    e.dev.counters = {0x58: 0x0305, 0x54: 0x0200, 0x56: 0x0004}
    e.call(e.h['CheckForHang'], e.adapter)
    st, v = query(e, 0x00020105)
    check(u32(v) == 5, 'OID_GEN_RCV_NO_BUFFER from the MAC counter (5)')
    st, v = query(e, 0x01020203)
    check(u32(v) == 3, 'OID_802_3_RCV_OVERRUN from the MAC counter (3)')
    # interrupt storm: MISR stuck
    e.dev.storm = True
    e.dev.pending = 1
    check(e.interrupt(), 'storm: ISR recognizes')
    e.dev.storm = False
    check(e.dev.regs.get(0x40) == 0x07, 'storm: DPC returns (bounded) and restores MIER')

    print('== link state (build 3)')
    e.status = []
    e.dev.cable = False
    check(e.call(e.h['CheckForHang'], e.adapter) & 0xFF == 0, 'cable out: CheckForHang FALSE')
    check(e.status == [0x4001000C, 'complete'], 'NDIS_STATUS_MEDIA_DISCONNECT + complete')
    st, v = query(e, 0x00010114)
    check(u32(v) == 1, 'OID_GEN_MEDIA_CONNECT_STATUS = disconnected')
    e.dev.tx_autocomplete = False
    pkt, _ = e.make_packet([bytes(60)])
    e.call(e.h['Send'], e.adapter, pkt, 0)
    e.dev.phy_writes = []
    hang = [e.call(e.h['CheckForHang'], e.adapter) & 0xFF for _ in range(5)]
    check(hang == [0] * 5, 'no TX-hang reset while the link is down')
    check(any(r == 0 and val & 0x1200 == 0x1200 for ad, r, val in e.dev.phy_writes),
          'auto-negotiation restarted after ~10 s without link')
    e.dev.tx_run()
    e.dev.tx_autocomplete = True
    e.status = []
    e.dev.cable = True
    for _ in range(4):                      # AN after replug takes a few s
        e.call(e.h['CheckForHang'], e.adapter)
        if e.status:
            break
    check(e.status == [0x4001000B, 'complete'], 'NDIS_STATUS_MEDIA_CONNECT on replug')
    st, v = query(e, 0x00010114)
    check(u32(v) == 0, 'OID_GEN_MEDIA_CONNECT_STATUS = connected')
    e.call(e.h['CheckForHang'], e.adapter)
    check(e.status == [0x4001000B, 'complete'], 'no repeated indication without change')
    e.call(e.h['Halt'], e.adapter)
    check(e.cmos[1] == 0x4E, 'halt done after the robustness run')

    e = boot(path, {'R6040MediaSense': 0})
    e.dev.cable = False
    e.call(e.h['CheckForHang'], e.adapter)
    check(e.status == [], 'R6040MediaSense=0: no indications')

    e = boot(path, {'ConnectionType': 4})
    e.dev.cable = False
    e.dev.phy_writes = []
    for _ in range(6):
        e.call(e.h['CheckForHang'], e.adapter)
    check(not any(r == 0 for ad, r, val in e.dev.phy_writes),
          'forced 100 full: no auto-negotiation restarts')
    check(e.dev.phy[0] & 0x3100 == 0x2100, 'ConnectionType 4: BMCR 100 Mbps full, AN off')

    print('== boot-time link (build 4)')
    def stale(em):
        em.dev.phy[5] = 0x0021              # BIOS-era partner word: 10 half
    e = boot(path, {}, stale)
    check(e.dev.an_restarts == 1, 'auto-negotiation restarted at init')
    check(e.dev.regs[0] & 0x8000, 'after renegotiation: MCR0 full duplex')
    st, v = query(e, 0x00010107)
    check(u32(v) == 1000000, 'link speed 100 Mbps after renegotiation')
    check('lpa=0021' in e.written.get('R6040PhyBoot', ''),
          'R6040PhyBoot records the BIOS state: ' + e.written.get('R6040PhyBoot', ''))
    check('an_wait=' in e.written.get('R6040Link', '') and '-> 100 full' in e.written['R6040Link'],
          'R6040Link records the result: ' + e.written.get('R6040Link', ''))
    e = boot(path, {'R6040AnAtInit': 0}, stale)
    check(e.dev.an_restarts == 0 and not e.dev.regs[0] & 0x8000,
          'R6040AnAtInit=0: build 3 behaviour (BIOS link kept, half duplex)')
    def nolpa(em):
        em.dev.phy[5] = 0x0001              # AN complete, no abilities
    e = boot(path, {'R6040AnAtInit': 0}, nolpa)
    check(e.dev.regs[0] & 0x8000, 'no partner abilities: duplex from the MAC (MPSR)')
    def unplugged(em):
        em.dev.cable = False
    e = boot(path, {}, unplugged)
    check(e.init_status == 0 and '-> no link' in e.written.get('R6040Link', ''),
          'no cable at boot: bounded wait, init succeeds: ' + e.written.get('R6040Link', ''))
    e = boot(path, {'ConnectionType': 3})
    check(e.dev.an_restarts == 0 and '(forced)' in e.written.get('R6040Link', ''),
          'forced mode: no auto-negotiation, "(forced)" recorded')

    print('== MAC PHY poller (build 5)')
    e = boot(path, {})
    check(e.dev.regs.get(0x88) == 0x8100, 'MPSCCR keeps the BIOS value 8100 across the MAC reset')
    check('mpsccr=8100' in e.written.get('R6040RegsBoot', '') and
          'mpsr=0000' not in e.written.get('R6040Regs', ''),
          'R6040Regs: MPSR follows the link: ' + e.written.get('R6040Regs', ''))
    check('mpsr=0007' in e.written.get('R6040Link', ''), 'R6040Link: ' + e.written.get('R6040Link', ''))
    def nopoll(em):
        em.dev.regs[0x88] = 0
    e = boot(path, {}, nopoll)
    check(e.dev.regs.get(0x88) == 0x8107, 'BIOS left the poller off: armed for PHY 1 (8107)')
    e = boot(path, {'R6040PhyPoll': 0})
    check(e.dev.regs.get(0x88, 0) == 0 and 'mpsr=0000' in e.written.get('R6040Regs', ''),
          'R6040PhyPoll=0: build 4 behaviour (poller cleared by the reset)')

    print('== removed experiments (build 10)')
    e = boot(path, {'R6040Mbcr': 0x1F1A, 'R6040LateAn': 1, 'R6040MediaBounce': 1})
    check(e.dev.regs.get(0x08) == 0x012A, 'R6040Mbcr ignored: MBCR stays 012A')
    n0 = e.dev.an_restarts
    for _ in range(6):
        e.call(e.h['CheckForHang'], e.adapter)
    check(e.dev.an_restarts == n0, 'R6040LateAn ignored: no late renegotiation')
    check(e.status == [], 'R6040MediaBounce ignored: no indications without a cable change')
    check(not {'R6040Mbcr', 'R6040LateAn', 'R6040MediaBounce'} & set(e.read_keys),
          'the removed switches are not even read')
    e.dev.counters = {0x5C: 0x0300}
    e.call(e.h['CheckForHang'], e.adapter)
    st, v = query(e, 0x01020204)
    check(u32(v) == 3 and e.cmos[5] & 0x80, 'TX underruns counted, CMOS warning bit 7')
    check(e.dev.regs.get(0x40) == 0x17, 'release default: TX_DONE interrupt on (R6040TxKick=1)')

    print('== TX re-kick (build 8)')
    def lazy(em):
        em.dev.tx_lazy = True
    burst = [bytes([0x10 + i]) * 200 for i in range(4)]
    for kick in (0, 1):
        e = boot(path, {'R6040TxKick': kick}, lazy)
        check(e.dev.regs.get(0x40) == (0x17 if kick else 0x07),
              'TxKick=%d: MIER %s' % (kick, '0x17 (TX_DONE on)' if kick else '0x07'))
        for f in burst:
            pkt, _ = e.make_packet([f])
            e.call(e.h['Send'], e.adapter, pkt, 0)
        e.dev.tx_finish()                   # engine sent what it saw at poll 1
        e.interrupt()
        if e.dev.tx_engine_busy:
            e.dev.tx_finish()
            e.interrupt()
        got = [x[:200] for x in e.dev.sent]
        if kick:
            check(got == burst, 'TxKick=1: lost polls re-kicked, whole burst sent (%d)' % len(got))
        else:
            check(len(got) == 1, 'TxKick=0 on a lazy engine: %d of 4 frames sent, rest stuck '
                  '(the suspected eBOX upload stall)' % len(got))
    e = boot(path, {}, lazy)
    pkt, _ = e.make_packet([bytes(100)])
    e.call(e.h['Send'], e.adapter, pkt, 0)
    pkt, _ = e.make_packet([bytes(100)])
    e.call(e.h['Send'], e.adapter, pkt, 0)
    polls = e.dev.polls
    for _ in range(2):
        e.call(e.h['CheckForHang'], e.adapter)
    check(e.dev.polls > polls, 'CheckForHang re-kicks a TX ring that made no progress')
    r = e.dev.regs
    check(r.get(0x70, 0) & 1 and (r[0x70] >> 1) == 2, 'debug words: tick counter in MID1L (%04X)' % r.get(0x70, 0))
    check(r.get(0x82, 0) >= 1 and (r.get(0x80, 0) >> 1) >= 1,
          'debug words: stale-TX checks %d, re-kicks %d' % (r.get(0x82, 0), r.get(0x80, 0) >> 1))
    e.dev.tx_finish()
    e.interrupt()
    e.dev.cable = False
    e.call(e.h['CheckForHang'], e.adapter)
    check(r.get(0x74, 0) & 0xFF == 1 and (r.get(0x78, 0) >> 1) == 3,
          'debug words: link down counted at tick 3')
    e = boot(path, {'R6040DbgRegs': 0})
    check(e.dev.regs.get(0x70, 0) == 0, 'R6040DbgRegs=0: perfect filter slots zeroed')

    print('== RDC PHY tweak')
    e = boot(path, {})
    check(not any(ad in (0, 30) for ad, r, v in e.dev.phy_writes), 'R6040PhyTweak default: untouched')
    e = boot(path, {'R6040PhyTweak': 1})
    w = [(ad, r, v) for ad, r, v in e.dev.phy_writes if ad in (0, 30)]
    check(w == [(30, 17, 0x6000), (30, 17, 0x4000), (0, 19, 0), (0, 30, 0x01F0)],
          'R6040PhyTweak=1: RDC sequence written')

    print('== diagnostics off')
    e = boot(path, {'R6040Diag': 0})
    check(e.init_status == 0 and e.cmos[1:6] == bytearray(5),
          'R6040Diag=0: Initialize clears the DriverEntry record, CMOS stays 0')

    print('== no shutdown handler')
    e = boot(path, {'R6040Shutdown': 0})
    check(e.init_status == 0 and e.shutdown is None, 'R6040Shutdown=0 registers none')
    e.call(e.h['Halt'], e.adapter)
    check('NdisMDeregisterAdapterShutdownHandler' not in e.calls, 'and does not deregister one')

    print('== forced 10 Mbps half duplex')
    e = boot(path, {'ConnectionType': 1})
    check(e.dev.phy[0] & 0x3100 == 0, 'BMCR: autoneg off, 10 Mbps, half duplex')
    check(not e.dev.regs[0] & 0x8000, 'MCR0 half duplex')
    st, v = query(e, 0x00010107)
    check(u32(v) == 100000, 'link speed 10 Mbps')

    print('== stuck hardware: bounded waits')
    def stuck(em):
        em.dev.mdio_stuck = True
        em.dev.reset_stuck = True
    e = boot(path, {}, stuck)
    check(e.init_status == 0, 'Initialize still returns with MDIO and reset stuck')
    check(e.cmos[5] & 0x05 == 0x05, 'CMOS warnings: MAC-reset timeout + MDIO timeout')
    e.call(e.h['Halt'], e.adapter)
    check(e.cmos[1] == 0x4E, 'Halt completes with stuck hardware')

    print('== init failure path')
    def nores(em):
        em.resources_ok = False
    e = boot(path, {}, nores)
    check(e.init_status == 0xC0010006, 'no resources -> NDIS_STATUS_ADAPTER_NOT_FOUND')
    check([t for p, (n, t) in e.allocs.items() if t in ('mem', 'shared')] == [],
          'failure path frees everything')
    check(e.cmos[1] == 0x32 and e.cmos[3] & 0x02, 'CMOS: init failed at step 2')

    def wrongid(em):
        struct.pack_into('<I', em.pci, 0, 0x813910EC)
    e = boot(path, {}, wrongid)
    check(e.init_status == 0xC0010006, 'wrong PCI ID refused')
    print('ALL TESTS PASSED')


if __name__ == '__main__':
    try:
        main(sys.argv[1] if len(sys.argv) > 1 else 'r6040.sys')
    except Fail as ex:
        print('FAIL: %s' % ex)
        sys.exit(1)
