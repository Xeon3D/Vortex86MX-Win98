# RDC R6040 Ethernet for the eBOX-3300MX — Windows 98 SE

The Vortex86MX's Ethernet MAC is the RDC R6040 (PCI `17F3:6040`). RDC never
wrote a Windows 9x NDIS 3 VxD for it; instead they shipped one NDIS 5.0 miniport
`.sys` that Windows 98/ME load through `NDIS.VXD` exactly like Windows 2000/XP do.
DM&P published that build for Windows 98 (`R6040_Win98.zip`, Aug 2008); it is the
driver in `R6040-Win98SE/`.

## R6040-Win98SE/ — install this

| File | Origin |
| --- | --- |
| `RDCPCI5.sys` | RDC NDIS 5.0 miniport, version 8.01.08.0101 (01-Aug-2008), the official Win98 build |
| `rdcndi.dll` | RDC's NDI installer DLL (property page for speed/duplex and flow control) |
| `NETRDC.INF` | Win9x-only INF: hardware ID `PCI\VEN_17F3&DEV_6040` (the original demanded `SUBSYS_00000000`, which an eBOX BIOS may not report) |
| `Netrdc.inf.dmp-original` | DM&P's unmodified INF, for reference |
| `RDCPCI5.sys.orig-8.01.08` | Unpatched RDC driver. The shipped `RDCPCI5.sys` is patched (2026-10-19): the MAC-reset wait at 0x10971 gives up after 0x10000 reads of MCR1, and the halt-time receive-pool drain at 0x12B41 gives up after 1000 × `NdisMSleep(10)` and then skips `NdisFreePacketPool` (leaks the pool, avoiding a free while packets are still out). The loop bodies live in the `.text` padding at 0x11F00–0x11F5C; `.text` VirtualSize was raised to 0x1A00 and the PE checksum recomputed. Retry counts: dword at file offset 0x1F01 (reset), 0x1F33 (drain) |

Install: Device Manager → the unknown "PCI Ethernet Controller" → Update Driver →
Have Disk → point at this folder → reboot. Then set TCP/IP as usual.

Imports of `RDCPCI5.sys` (checked): `NDIS.SYS` (39 functions, all NDIS 3/4/5 core
calls), `HAL.dll` port I/O (`READ/WRITE_PORT_USHORT/ULONG`), `ntoskrnl.exe`
(`KeTickCount`, `DbgPrint`). All of these are provided by Windows 98 SE's NDIS.VXD
loader for NT-style miniports.

### alt-sys/ — A/B candidates

| Folder | Version | Notes |
| --- | --- | --- |
| `xp-9.10.08` | 9.10.08.0102 (10-Sep-2008) | From the XP package in `xpethdrv/`. Additionally imports `NdisMGetDmaAlignment` and `KeStallExecutionProcessor`. |
| `w2k-11.14.08` | 11.14.08.0104 (14-Nov-2008) | Newest build, from `R6040_Win2000.zip`. Same extra imports as the XP build. |

Only try these if the 8.01.08 build misbehaves: copy one `RDCPCI5.sys` over
`Windows\System\RDCPCI5.SYS` (Safe Mode or DOS) and reboot. The extra imports are
documented NDIS 4/5 and HAL exports that NDIS.VXD normally resolves, but the
8.01.08 build is the one RDC explicitly released for Windows 98.

## R6040-DOS-NDIS2/ — real-mode fallback

`RDCPCI.DOS` 1.03 is RDC's NDIS 2.01 driver with `OEMSETUP.INF`/`PROTOCOL.INI`.
Use it for the Windows 98 real-mode network client (Safe Mode with networking,
network boot disks, Microsoft Network Client 3.0) or if the protected-mode driver
cannot be made to work. Parameters: `SLOT`, `MEDIA_TYPE`
(`AUTODETECT`, `FM_100M_FULL`, `FM_100M_HALF`, `FM_10M_FULL`, `FM_10M_HALF`),
`RX_FLOW`, `TX_FLOW`.

## R6040-DOS-Packet/

`R6040PD.COM` — DOS packet driver (mTCP, WATTCP, etc.). Not used by Windows.

## Troubleshooting

- Yellow "!" with code 10 after install: check that `RDCPCI5.SYS` and `rdcndi.dll`
  landed in `C:\Windows\System`, then try the `alt-sys` builds.
- No link: force "Connection Type" = 100Mbps Full_Duplex (or 10Mbps) in the
  adapter's Advanced properties; the R6040 PHY auto-negotiation is known to be
  touchy with some switches (the Linux driver carries the same workaround).
- MAC address 00:00:00:00:00:00 or FF:FF:FF:FF:FF:FF: the BIOS did not program
  the MAC; set it in the BIOS LAN setup (the DOS driver prints the same warning).

## Source references for a future open-source driver

- Linux: <https://github.com/torvalds/linux/blob/master/drivers/net/ethernet/rdc/r6040.c>
- FreeBSD `vte(4)`: <https://github.com/freebsd/freebsd-src/tree/49e3c256e4619c56acc09660bc3c49b68ec32c20/sys/dev/vte>

Both describe the 256-byte I/O register window, the 32-byte descriptors, the MDIO
PHY access and the multicast hash; copies are in the session scratchpad
(`ref/linux`, `ref/freebsd`).

## r6040vxd/ - our own open-source driver (R6040.SYS)

`r6040vxd/` holds an MIT-licensed NDIS 3.0 miniport written from the FreeBSD
vte(4)/Linux r6040 register descriptions, designed so that halt and shutdown
can never wait for anything: synchronous copy-sends, copy-indicated receives,
bounded hardware waits, no spin locks, and optional CMOS progress bytes for
debugging the shutdown. It is an NT-style `.SYS` (not a VxD): Win98's NDIS.VXD
offers the miniport API only to NTKERN-loaded `.SYS` drivers (details in
`r6040vxd/README.md`). Test install on the eBOX: `OWNNET` (swaps it in as
`RDCPCI5.SYS`), undo: `NETUNDO`.
