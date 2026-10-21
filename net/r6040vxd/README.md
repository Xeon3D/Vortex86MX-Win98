# R6040.SYS: open NDIS miniport for the RDC R6040 (eBOX-3300MX, Windows 98 SE)

**Status: release 2.0 (build 10, test ID `N10`).** It behaves like build 8,
which the eBOX confirmed on 2026-10-20: after a fresh boot, a 32 MB round trip
runs in 9.0 s down / 6.1 s up (builds 3-7: about 10.9 / 12.8 s), with the data
byte-identical. Shutdown and restart are clean (since build 2). See
"Changelog" below. The one restart freeze left after build 2 was NUSB's
USB 2.0 (EHCI) driver, not this one.

Our own driver for the Vortex86MX Ethernet MAC (PCI `17F3:6040`). It replaces
RDC's binary `RDCPCI5.SYS` 8.01.08, which makes Windows 98 hang at "Windows is
shutting down" (it still hangs with both of RDC's halt loops bounded, with no
protocol bound and with the cable unplugged).

| File | What |
| --- | --- |
| `r6040.c`, `r6040hw.h`, `ndis98.h`, `version.h` | the driver |
| `makefile`, `pefix.py` | Open Watcom build plus a PE post-link fix: **rebuilds `.reloc`** (wlink with objalign=512 writes block pages 0x200 too high, which crashed build 1 at boot), checksum, version 4.0 |
| `r6040dbg.c` | `R6040DBG.EXE`, a DOS reader for the CMOS progress bytes |
| `R6040.INF` | Win9x INF for a clean install (hardware ID `PCI\VEN_17F3&DEV_6040`) |
| `OWNNET.BAT`, `NETSEND.BAT` | lab install over FTP and result upload (`OWNNET DIAG` adds the AUTOEXEC log line) |
| `INSTALL.BAT`, `UNDIAG.BAT` | offline install (swap under RDC's entry, or `INSTALL CLEAN` for a Device Manager install); remove the AUTOEXEC log line |
| `test/emu_r6040.py` | runs `r6040.sys` under unicorn against a fake NDIS and a fake R6040 |

## Why a `.SYS` and not a VxD

The plan was an NDIS 3.1 miniport VxD. The Win98 NDIS.VXD in the DDK
(`DDK\DBG_SYM\DEBUG\WINDOWS\SYSTEM`, debug build with symbols) shows that the
miniport API is not offered to VxDs:

- NDIS.VXD's 161 VxD services (device 0028h) contain **no `NdisM*` service**.
  The NT miniport API exists only in the PE export table that NDIS registers
  as `NDIS.SYS` (261 names, `_PELDR_AddExportTable` in `NdisPnpDeviceInit`).
- `LoadDynamicDriversForNode` reads `DeviceVxDs`. A `.SYS` name goes to
  `NtKernWin9XLoadDriver` + `IoCreateDevice`, anything else to `VXDLDR_LoadDevice`.
- `NdisMRegisterMiniport` writes the IRP dispatch table into the DRIVER_OBJECT
  passed to `NdisInitializeWrapper`, with no NULL check, and
  `ndisMInitializeAdapter` asserts on a DEVICE_OBJECT. Only the NTKERN (`.SYS`)
  load path creates both.

A VxD would have to fake both objects for a code path Microsoft never meant
for VxDs. So R6040.SYS is an NT-style NDIS 3.0 miniport, the format Win98's own
NIC drivers and RDC use, built with Open Watcom. It imports **only**
`NDIS.SYS` (24 functions; RDC's XP/W2K builds also import
`KeStallExecutionProcessor`/`NdisMGetDmaAlignment`, a possible reason they fail
on Win98). The NDIS internals it relies on are listed under "Win98 ABI
details" below.

## Design: nothing can be pending at halt or shutdown

- **Send** copies the frame into a pre-allocated DMA bounce buffer (16-entry
  ring, 1536 bytes each), pads it to 60 bytes, hands the descriptor to the MAC
  and returns `NDIS_STATUS_SUCCESS`. No send ever pends, so the driver never
  calls `NdisMSendComplete`. When the ring is full it returns
  `NDIS_STATUS_RESOURCES` and calls `NdisMSendResourcesAvailable` when
  descriptors free up.
- **Receive** (32-entry ring) indicates each good frame with
  `NdisMEthIndicateReceive`, using the whole frame as lookahead (copy
  semantics), then one `...ReceiveComplete` per batch. Only after that do the
  descriptors go back to the MAC, so `TransferData` stays valid during the
  indication. No receive packet is ever owned by a protocol, so there is
  nothing to wait for or return at halt.
- **Every hardware wait is a bounded poll**: MAC reset ≤ 2 ms, then a manual
  state-machine reset as the vendor recommends; MAC stop ≤ 10 ms; MDIO
  ≤ 10 ms. Delays use the port 61h refresh bit plus a read-count cap. The
  driver never calls `NdisMSleep` or any other blocking service.
- **Halt**: mask interrupts → stop and reset the MAC (DMA idle) →
  `NdisMDeregisterInterrupt` → deregister the shutdown handler → free the shared
  memory, the I/O range and the adapter. A CMOS checkpoint is written between
  steps. Note: `NdisMDeregisterInterrupt` itself waits on a kernel event while
  an interrupt DPC is outstanding. The driver masks the chip first and records
  a `DPC-pending-at-halt` warning if one was queued.
- **Shutdown handler** (optional, `R6040Shutdown`, default on): mask
  interrupts, stop and reset the MAC. It frees nothing, takes no lock and calls
  no NDIS function.
- **No spin locks**. NDIS serializes a 3.0 miniport. The ISR (reading MISR
  acknowledges it) masks MIER, ORs the status into a shared word with
  `lock or` and requests the DPC. `HandleInterrupt` takes that word with
  `xchg`, processes RX/TX and re-enables MIER.
- **Hardware**: resources come from `NdisMPciAssignResources` (Win98 ignores
  the slot and uses the devnode), with a fallback to `NdisMQueryAdapterResources`.
  The driver checks the PCI ID with `NdisReadPciSlotInformation` and turns on
  I/O and bus mastering. Rings and buffers use `NdisMAllocateSharedMemory`,
  which on Win98 is `HEAPCONTIG|HEAPLOCKEDLOW` memory; 32-byte descriptors are
  16-byte aligned. The MAC address is read from MID0 as the BIOS programmed
  it, before any reset, and written back after each reset (a locally
  administered fallback is used if it is invalid). The PHY address comes from
  MPSCCR if the BIOS set it, otherwise from a scan. Auto-negotiation
  advertises 10/100 half/full and is only restarted if not already set up, so
  the BIOS link stays up for DHCP. Duplex is resolved from ANAR&ANLPAR into
  MCR0. Multicast uses the 64-bit hash (top 6 bits of the big-endian CRC-32).
  ALL_MULTICAST and PROMISCUOUS are supported. Flow control stays off (the
  vendor and FreeBSD report TX pause storms).
- `CheckForHang` (every ~2 s) polls the link and reports a hang after ~6 s
  without TX progress. `Reset` then re-initializes the chip synchronously.

### Win98 ABI details the driver depends on (checked in NDIS.VXD)

- NDIS 3.0 characteristics length 0x3C (4.0 = 0x48, 5.0 = 0x60).
- `NdisMEthIndicateReceive[Complete]` are macros through the miniport block:
  EthDB at +0xD8, EthRxIndicateHandler at +0x164, EthRxCompleteHandler at
  +0x170. These offsets match the public NDIS 5 layout. RDC's driver uses its
  neighbours +0xE8/+0xEC on the box.
- The miniport `NdisOpenConfiguration` sets the Unicode flag, so keywords are
  `UNICODE_STRING`.
- Win98 `NDIS_BUFFER` and the NT MDL both have `Next` at offset 0. The driver
  walks chains with `NdisGetFirstBufferFromPacket` + `Next` + `NdisQueryBuffer`.

## Release 2.0 (build 10)

Build 8 plus clean-up. Kept, with their defaults:
- `R6040TxKick` = 1, the build 8 fix: the TX_DONE interrupt is always on,
  and TX_POLL is re-kicked while frames are queued, both after each
  completion and from `CheckForHang`. It's now in the Advanced tab.
- `R6040AnAtInit`, `R6040PhyPoll`, `R6040MediaSense`, `R6040AnRestart`,
  `R6040Shutdown`, `R6040Diag` (CMOS checkpoints), `R6040DbgRegs` (live
  counters) and `R6040PhyTweak` (off).
- The registry status strings `R6040Version`, `R6040PhyBoot`, `R6040Link`,
  `R6040RegsBoot` and `R6040Regs`.

Removed, because none of them changed the result on the eBOX:
- `R6040LateAn` (build 6), `R6040Mbcr` (build 6; MBCR is fixed at 012A
  again) and `R6040MediaBounce` (build 7).
- Build 9's `R6040LinkDrop` was never part of the build 8 line.

The removed values are no longer even read, so leftovers in the registry do
nothing.

**N10 vs N8 behaviour**: `test/equiv.py` runs the same scenario (init, a send
burst, a TX ring overflow, RX, unplug/replug, CheckForHang ticks, reset,
shutdown, halt) on both binaries. The only differences are 3 fewer
`NdisReadConfiguration` calls (the removed switches), the DMA ring
addresses (the adapter structure is smaller), the version string, and the
live-counter word at MID1M, which now holds the RX-resync count instead of
build 7's always-zero bounce tick. Everything else matches in order and
value: all port writes, all NDIS calls, the interrupt mask and the
TX/RX handling. Version "2.0 build 10"; 16,384 bytes.

## Changelog

- **2.0 build 10**: release of build 8 without the experiments (above).
- **1.7 build 9** (test only): PHY power-down link-drop experiment. Not needed.
- **1.6 build 8**: **the upload fix**. A TX_POLL written while the MAC is
  busy can get lost, which leaves the last frames of a burst queued. It
  showed as a TX descriptor pointer stuck at the ring base. Now there's a
  TX_DONE interrupt with a re-kick. Also live counters (`R6040DBG /REGS`).
- **1.5 build 7** (test only): media-bounce experiment, no effect.
- **1.4 build 6** (test only): LateAn / MBCR experiments, no effect; a
  TX-underrun CMOS flag.
- **1.3 build 5**: keeps the MAC's PHY poller (MPSCCR) across the MAC
  reset; MAC register snapshots in the registry; `R6040DBG /REGS`.
- **1.2 build 4**: renegotiates the link at start; `R6040PhyBoot` /
  `R6040Link`.
- **1.1 build 3**: robustness fixes (ISR order, RX resync, bounded DPC),
  media sense, MAC counters, version identification.
- **1.0 build 2**: the first working build. Shutdown and restart are
  clean. (Build 1 crashed at load because of wlink's relocation table.)

Development notes for each build follow.

## Build 3 changes (1.1, `N3`)

Robustness, compared against Linux r6040.c and FreeBSD vte(4):

- **ISR order like Linux**: mask MIER, then read MISR (reading acknowledges).
  If the interrupt is not ours (shared IRQ), MIER is restored.
- **Bounded DPC**: at most 8 passes of at most 32 frames. Leftover work
  re-asserts the interrupt as soon as MIER is restored, so an interrupt storm
  can't trap the DPC.
- **RX resync**: if the MAC reports RX work while our next descriptor is
  still owned by the MAC (the two indexes are out of step), the driver jumps
  to the oldest completed frame instead of stalling reception for good.
- **RX overflow** (no free descriptor / FIFO full): the whole ring is
  processed and re-armed, and the MAC's own event counters (read every ~2 s)
  feed `OID_GEN_RCV_NO_BUFFER`, `OID_802_3_RCV_OVERRUN`, runt and too-long
  statistics.
- **TX**: descriptors are reclaimed in `Send` and in `CheckForHang`, so
  `TX_DONE` is no longer an interrupt source. It's enabled only while the
  ring is full, to resume NDIS (`NdisMSendResourcesAvailable`). If that
  interrupt is lost, `CheckForHang` resumes NDIS anyway. Sends are still
  never pending.
- **No TX-hang reset while the link is down** (the MAC may hold frames
  without a link).
- **Auto-negotiation retry** (`R6040AnRestart`): restarts auto-negotiation
  after ~10 s without link when it never completed (the switch-compatibility
  workaround). Only with `ConnectionType` 0.
- **RDC PHY tuning** (`R6040PhyTweak`, off by default): the internal PHY
  register writes from RDC's Linux driver ("improve performance").

Link state:

- `NDIS_STATUS_MEDIA_CONNECT` / `..._DISCONNECT` are indicated on a link
  change (`R6040MediaSense`), never during Initialize.
  `OID_GEN_MEDIA_CONNECT_STATUS` and `OID_GEN_LINK_SPEED` report the real
  state and 10/100.
- `ConnectionType` 0-4 is honoured as before: forced modes switch
  auto-negotiation off, and MCR0 duplex follows.

Efficiency:

- The TX bounce copy uses dword moves (`rep movsd` plus a byte tail).
- About half the interrupts on transmit-heavy traffic, because `TX_DONE` no
  longer interrupts.

Diagnostics and install:

- With `R6040Diag` = 0, Initialize also clears DriverEntry's record, so the
  CMOS stays untouched.
- `OWNNET` adds the AUTOEXEC.BAT line only with `DIAG`, and `UNDIAG.BAT`
  removes it.
- `INSTALL CLEAN` prepares a Device Manager install on machines that never had
  RDC's driver. `R6040.INF` now writes the same registry layout as RDC's
  installer (`DevLoader *ndis,*ntkern,*ndis`, `DeviceVxDs` + `NTMPDriver`).
- All batches write at most one `[rename]` header into WININIT.INI.

Version identification:

- The INF device name reads "RDC R6040 Fast Ethernet (open R6040.SYS 2.0
  build 10)".
- The driver writes `R6040Version` = "2.0 build 10" into the adapter's
  registry key at every start (visible with regedit, or in `netreg.txt`
  from NETSEND), and `OID_GEN_VENDOR_DESCRIPTION` carries the same string.
  This also covers swapped installs, where Device Manager keeps RDC's name.
  The file sizes differ as well: build 2 is 11,776 bytes, build 3 is 13,312.

Build fix: `pefix.py` now handles wlink's 4 KB relocation windows that start
at the section address: offsets in the upper part wrap. Build 2 was too small
to hit that case. The build fails if any fix-up would land outside the image.

## Build 4 changes (1.2, `N4`)

N3 on the eBOX: uploads ran at about 2.7 MB/s after a fresh boot but
5.4 MB/s after a cable replug. The replug let the driver's 10-second
auto-negotiation retry renegotiate the link. At init, builds 2-3 kept the
link the BIOS left behind and trusted ANAR&ANLPAR, which resolved half duplex.

- With `ConnectionType` 0, Initialize now **always restarts
  auto-negotiation and waits for link + "AN complete"** (bounded to 4 s,
  polled every 20 ms), then resolves speed and duplex into MCR0 the same way
  the link-change path does. `R6040AnAtInit` = 0 restores the build 3
  behaviour. Without a cable, init takes the full 4 s and then continues.
- If auto-negotiation completes but ANLPAR shows no partner abilities, the
  duplex comes from the MAC's own PHY status register (MPSR).
- New registry values written at every start (they show in NETSEND's
  `netreg.txt`):
  - `R6040PhyBoot` = the PHY registers as the BIOS left them, e.g.
    `phy=1 bmcr=3100 bmsr=786d anar=01e1 lpa=0021 mpsr=0007`.
  - `R6040Link` = the registers after our init plus the result, e.g.
    `... lpa=45e1 mpsr=0007 -> 100 full an_wait=1800ms`.
- Version "1.2 build 4" (INF name, `R6040Version`); 14,848 bytes.

## Build 5 changes (1.3, `N5`)

N4 showed the BIOS link was already 100 full (`lpa=cde1`), so duplex wasn't
the cause. But `mpsr=0000` both before and after our init: the MAC's own PHY
status poller (MPSCCR) wasn't reporting the link, because our MAC reset
clears MPSCCR. The N3 replug speed-up is still unexplained by the code paths:
the link-change path only rewrites MCR0 duplex (unchanged) and restarts
auto-negotiation, which N4 already did at init.

- MPSCCR is saved before the MAC reset and written back after it. If the
  BIOS left it at 0, it's armed for our PHY (`8000 | phy<<8 | 7`).
  `R6040PhyPoll` = 0 restores build 4's behaviour.
- Init waits up to 1 s for MPSR to report the link and appends
  `mpsr_wait=…ms mpsr=…` to `R6040Link`.
- MAC register snapshots in the registry: `R6040RegsBoot` (before our reset)
  and `R6040Regs` (end of init). Both list io, mcr0, mcr1, mbcr, mticr, mricr,
  mrbsr, mrdcr, mlsr, mier, mecier, mpsccr, mpsr, macsm, mdcsc and macid.
- `R6040DBG /REGS [/PHY] [/IO:hhhh]` dumps the live MAC (and MII) registers
  at any time, also from a Windows DOS box. It finds the MAC by PCI scan
  (ports CF8h/CFCh). Use it before and after a replug to diff the state.
- Version "1.3 build 5"; 15,872 bytes.

## Build 6 changes (1.4, `N6`)

Confirmed on N4: after a ~15 s unplug and replug, uploads take 4.6 s instead of
12.9 s for 32 MB, while downloads stay the same. An emulator state diff
(`test/statediff.py`: fresh init vs unplug with sends held, then replug)
shows no lasting driver or register difference. MIER is 0x07 in both, and
only the TX ring position differs. So the difference is chip-internal: the
replug takes the MAC's TX engine through "frames queued, no link", which
N4's init-time renegotiation never does, because no frames are queued then
and interrupts aren't live yet. Build 6 adds switches to reproduce or rule
out the causes:

- `R6040LateAn` = 1 (default 0): renegotiates once, ~4 s after start, with
  the MAC running, interrupts live and traffic flowing, which is the closest a
  driver can get to a replug.
- `R6040Mbcr` = hex (default `012A`, the Linux value; FreeBSD uses `1F1A`,
  whose SDRAM bus-request timer is 0x1F instead of 1): the MAC bus-control
  register. Starved TX DMA would slow only TX.
- TX underruns and late collisions (descriptor bits plus the MAC's counter
  MECNT4) set CMOS warning bit 7, which `R6040DBG` shows live, and feed
  `OID_802_3_XMIT_UNDERRUN` / `..._LATE_COLLISIONS`.
- `NSET.BAT name value` writes a switch into the adapter key
  (Class\Net\0000) via `regedit /s`.
- Version "1.4 build 6" (same 15,872-byte size as build 5: check
  `R6040Version`).

## Build 7 changes (1.5, `N7`)

N6 register dumps after a fresh boot (slow) and after a replug (fast) are
identical apart from the moving ring pointers. So the replug effect isn't in
the chip: it's in how the protocols react to the DISCONNECT/CONNECT
indications (new since build 3). NDIS.VXD itself only arms its
power-management media-disconnect timer on DISCONNECT and doesn't touch the
send path.

- `R6040MediaBounce` = 1 (**default in build 7**): the driver indicates
  `NDIS_STATUS_MEDIA_DISCONNECT` ~4 s after start and `..._CONNECT` 2 s later,
  once, while the cable stays in, the same sequence a replug produces. It
  always ends connected, even with `R6040MediaSense` = 0, and is skipped if
  there's no link at start. `NSET R6040MediaBounce 0` switches it off.
- Version "1.5 build 7"; 16,384 bytes.

## Build 8 changes (1.6, `N8`)

N7: the media bounce didn't help (boot upload still 12.8 s); `R6040LateAn`
and `R6040Mbcr` 1F1A didn't either. The N6 register dumps hold the clue: the
TX descriptor pointer MTDSA0 read F2A0 in the slow state, which is the ring
base (descriptor 0) right after a 32 MB upload, and F400 (descriptor 11)
after a replug. Working theory: the MAC finishes a frame and idles, and a
TX_POLL written while it's busy is lost. The last frame of a TCP burst then
waits until the next Send kicks the engine (typically after the PC's
~200 ms delayed-ACK timeout). Builds 3-7 never re-kicked the engine, because
TX_DONE wasn't even an interrupt there. The emulator's "lazy engine" model
reproduces it: 1 of 4 burst frames goes out.

- `R6040TxKick` = 1 (default): the TX_DONE interrupt is always on, and after
  each completion the DPC writes TX_POLL again while frames are still queued.
  `CheckForHang` also re-kicks a ring that made no progress. Sends are still
  never pending. `R6040TxKick` = 0 restores the builds 3-7 behaviour.
- Live driver counters in the unused perfect-filter registers MID1..MID3
  (`R6040DbgRegs` = 1, default). `R6040DBG /REGS` prints them as a
  `driver:` line: ticks (×2 s), the media-bounce tick, link down/up counts
  and the tick of the last change, frames sent, RESOURCES returned, TX
  re-kicks, checks without TX progress ("stale"), and tx_busy at the last
  link change and now. The first word of each slot keeps the multicast bit
  set, so no unicast frame can match these filter entries.
- `R6040MediaBounce` defaults to 0 again (it didn't help).
- Version "1.6 build 8"; 17,408 bytes.

## Build 9 changes (1.7, `N9`)

N7 with `R6040MediaSense` = 0 still sped up after a replug (12.3 s → 4.5 s),
so Windows' reaction is ruled out. With media sense off, the driver's own
link-down/up handling only rewrites MCR0 duplex (unchanged here) and, after
~10 s without link, restarts auto-negotiation. So what matters is the chip
living through a real link loss, typically with frames queued.

- `R6040LinkDrop` = 1 (**default in build 9**): ~4 s after start (second
  CheckForHang), with interrupts live, the PHY is powered down (BMCR bit 11),
  which drops the link for the switch too. A 60-byte test frame to our own
  MAC (EtherType 9000h, loopback test) is queued so the TX engine stalls
  with work pending. One check later (~2 s) the PHY is powered up again with
  auto-negotiation restarted. It happens once, and not at all without link.
  `R6040LinkDrop` = 2 isolates the PHY (BMCR bit 10) instead; 0 = off.
- The steps are logged in the `drop/bounce` word of `R6040DBG /REGS`'s
  `driver:` line as step << 12 | tick: 1xxx PHY down, 2xxx PHY up again,
  3xxx link back.
- To keep the experiments apart, build 9 defaults to `R6040TxKick` = 0. Use
  `NSET R6040TxKick 1` to combine both.
- Version "1.7 build 9"; 17,920 bytes.

## Registry parameters (adapter key)

| Value | Default | Meaning |
| --- | --- | --- |
| `ConnectionType` | 0 | 0 auto, 1 10 half, 2 100 half, 3 10 full, 4 100 full (RDC's values, kept by OWNNET) |
| `R6040Diag` | 1 | CMOS progress bytes on/off |
| `R6040DiagCmos` | 1 | CMOS index of the 3 bytes (index, +2, +4): 1 = RTC alarm bytes 01h/03h/05h, or 52..123 |
| `R6040Shutdown` | 1 | register the shutdown handler |
| `R6040MediaSense` | 1 | indicate link connect/disconnect to the protocols (build 3) |
| `R6040AnRestart` | 1 | restart auto-negotiation after ~10 s without link (build 3) |
| `R6040PhyTweak` | 0 | RDC's internal PHY tuning writes, experimental (build 3) |
| `R6040AnAtInit` | 1 | renegotiate the link at every start and wait for it (build 4) |
| `R6040PhyBoot`, `R6040Link` | written by the driver | PHY state at boot / after init with the negotiated result (build 4) |
| `R6040PhyPoll` | 1 | keep/arm the MAC's PHY status poller MPSCCR across the reset (build 5) |
| `R6040RegsBoot`, `R6040Regs` | written by the driver | MAC register snapshots before our reset / after init (build 5) |
| `R6040TxKick` | 1 | TX_DONE interrupt + TX_POLL re-kick while frames are queued (build 8 fix; 0 = builds 3-7) |
| `R6040DbgRegs` | 1 | live counters in MID1..MID3 for R6040DBG /REGS (build 8) |
| `R6040Version` | written by the driver | the running build, e.g. "2.0 build 10" (since build 3) |

Values are strings in `HKLM\System\CurrentControlSet\Services\Class\Net\0000`
(on the eBOX). They take effect at the next boot.

## Diagnostics (no COM port on the eBOX)

The driver writes three CMOS bytes (default 01h/03h/05h: the RTC alarm
seconds/minutes/hours). The BIOS checksum doesn't cover them, and they do
nothing while the alarm interrupt is off.

- 01h **last checkpoint**: `1x` DriverEntry (10 entered, 11 wrapper, 12
  registered, 1F failed; always written, before the registry can be read; 00
  after a boot = the PE never ran), `2x` Initialize (20 entered … 27 interrupt, 2E
  running, 3s failed at step s), `4x` Halt (40 entered, 41 masked, 42 MAC
  stopped, 43 inside NdisMDeregisterInterrupt, 44 done with it, 45 inside
  NdisMDeregisterAdapterShutdownHandler, 46 freed, 4E done), `50/5E` shutdown
  handler entered/done, `60/6E` reset.
- 03h **events** (sticky): driver-loaded, init-ok, init-failed,
  shutdown-entered/done, halt-entered/done, reset.
- 05h **warnings** (sticky): MAC-reset timeout, MAC-stop timeout, MDIO timeout,
  no PHY, DPC pending at halt, invalid BIOS MAC, TX-hang reset.

`R6040DBG.EXE` decodes them. OWNNET adds it to AUTOEXEC.BAT
(`/LOG C:\M2010\R6040DBG.TXT`), so every boot appends one line describing how
far the previous shutdown got and then clears the bytes. Without arguments it
only displays them; `/C` clears them. If shutdown hangs again, the last
checkpoint shows exactly which call it hung in.

## Build

```
set WATCOM=C:\WATCOM
set INCLUDE=C:\WATCOM\h;C:\WATCOM\h\nt
set PATH=C:\WATCOM\binnt64;C:\WATCOM\binnt;%PATH%
wmake -h -e          (from bash: wmake -h -e </dev/null)
python test\emu_r6040.py r6040.sys
```

`emu_r6040.py` (`pip install unicorn pefile`) loads the PE the way VXDLDR does
(each section in its own block, base relocations applied, imports by name), stubs the 24
NDIS imports (it checks argument counts, stack balance and frees) and models
the R6040: registers, MDIO PHY, TX/RX descriptor DMA, MISR, port 61h and CMOS.
It then runs init, OIDs, filters and hash, send/pad/ring-full/resources, RX
batches with ring wrap, TransferData inside an indication, the shared-IRQ ISR,
hang → reset, shutdown, halt (everything freed, nothing pending), diagnostics
off, no shutdown handler, forced 10 half, stuck MDIO and reset (waits stay
bounded), and the init failure paths. Result: `ALL TESTS PASSED`. It is not a
substitute for the real chip: the descriptor/MDIO semantics are modelled on
the FreeBSD/Linux descriptions.

## Testing builds with TRY (eBOX)

`exchange\out\N<n>\` holds `RDCPCI5.SYS` + `BUILD.TXT`. On the eBOX:
`TRY N3` queues the build (the previous file is saved), then reboot. `BACK N`
restores the previous file, `GOOD N` your known-good set (saved with `KEEP`),
`RESCUE` works from "Command prompt only", and `VER` shows what is installed.
IDs: `N2` = build 2 (confirmed), `N3` = build 3, `N4` = build 4, `N5` = build 5, `N6` = build 6, `N7` = build 7, `N8` = build 8, `N9` = build 9, `N10` = release 2.0, `NRDC` = RDC's original.

## Test plan on the eBOX (build 2, original procedure)

1. On the PC: `python exchange\ftpserver.py` (serves `exchange\out`).
2. On the eBOX (DOS box), with RDC's driver still installed: `GET NOREBOOT`
   if a display build is pending, then `OWNNET`. It downloads the files,
   queues `R6040.SYS` as `RDCPCI5.SYS` in WININIT.INI, creates `NETOFF.TXT`
   (so GET never re-queues RDC's patched driver), adds the R6040DBG line to
   AUTOEXEC.BAT and reboots.
3. After the reboot, check the network: `GET NOREBOOT` (download) and
   `SEND "own driver up"` (upload). Network Neighborhood / ping as usual.
4. **Start → Shut Down → Restart.** Note whether it completes.
5. After the next boot (forced reset if it hung): `NETSEND "restart ok"` or
   `NETSEND "restart hung"`. It uploads `r6040dbg.txt` (the CMOS record of the
   shutdown), `r6040now.txt` and `netreg.txt` to `exchange\in`.
6. Back to RDC's driver at any time: `NETUNDO` (offline). If Windows doesn't
   start: boot "Command prompt only" and
   `copy C:\M2010\NET\RDCPCI5.ORG C:\WINDOWS\SYSTEM\RDCPCI5.SYS`. Safe Mode
   doesn't load network drivers.

Optional A/B tests: set `R6040Shutdown` to 0 or `R6040Diag` to 0 in the
adapter's registry key (`HKLM\System\CurrentControlSet\Services\Class\Net\000n`).
