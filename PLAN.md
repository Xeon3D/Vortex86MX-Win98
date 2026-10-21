# eBOX-3300MX (Vortex86MX) — Windows 98 SE VGA + Ethernet drivers

Target board: DMP EBOX-3300MX, RDC Vortex86MX SoC.
- Graphics: RDC M2010, PCI `17F3:2010`, option ROM "RDC GPU 0.00.20" (32 KiB, VBE 2.0+ with
  `VBE2`/`VESA` info block and a VBE 3.0 `PMID` protected-mode block). Framebuffer is a
  shared-memory aperture (UMA) at BAR0, MMIO (2D engine, 128 KiB) at BAR1, relocated VGA
  ports at BAR2 (CRTC index/data = BAR2+0x54/0x55).
- Ethernet: RDC R6040 MAC, PCI `17F3:6040`, 256-byte I/O window, MDIO PHY.

## What we start from (verified)

| Input | Status |
| --- | --- |
| `rdc-research/win98-m2010-vesa` (VMDisp9x 1.2025.0.119 VESA minidriver, INF for 17F3:2010) | **Works on the box** (user-confirmed). Our VGA baseline. |
| `rdc-research/m2010/src` (RDC Xorg driver, MIT) | Authoritative register-level knowledge: 2D engine, HW cursor, CBIOS mode tables. |
| `rdc-research/win98-m2010-direct` | CBIOS port that compiles; untested. Only its build fixes (`control.c` stdint, `fixlink`) are reused. |
| `DDK` / `Win98 DDK` | Byte-identical copies of the July 1999 Win98 DDK. Used for headers (`dibeng.inc`, `ndis.h`) and documentation only; the driver builds with Open Watcom 2.0. |
| `xpethdrv/RDCPCI5.sys` 9.10.08 (XP) | NDIS 5.0 miniport, imports only `NDIS.SYS` + 4 HAL port I/O + `KeTickCount`/`DbgPrint`. |
| DMP `R6040_Win98.zip` (official) | Same driver family, build 8.01.08 (Aug 2008) for Win98; INF has a Win9x section (`DevLoader=*ndis`). **Our Ethernet baseline.** |
| `DOS ETH DRV` | NDIS2 `RDCPCI.DOS` 1.03, packet driver `R6040PD.COM`, ODI driver. DOS/real-mode fallback. |

## VGA driver plan

Architecture (from VMDisp9x): 16-bit `vesamini.drv` (DIB-engine minidriver, ring 3) +
32-bit `vesamini.vxd` (mini-VDD, ring 0). GDI draws into a **back buffer in VRAM**; after
every DIB-engine operation the VxD copies the dirty rectangle to the visible front buffer
with `memcpy` (`VESA_copy_rect`), or flips the display start if the BIOS supports VBE
function 07h. Mode set goes through the real VBE BIOS (`INT 10h/4F02h`) in V86 mode.

Phases (each one is shippable, each keeps the previous behaviour reachable via registry):

1. **M2010 hardware detection + 2D-engine back→front copy (VxD only).**
   PCI scan for 17F3:2010, map BAR1 MMIO, read VRAM size from CR AA, enable the 2D engine
   (CR A4[0], CR A3[5], MMIO 0x8040[31], MMIO command mode 0x8044 = 0xF2000000), and replace
   the per-operation `memcpy` with a hardware screen-to-screen BitBlt. Disable the engine
   before any VGA/DOS mode switch, re-enable after each VBE mode set.
   Registry: `HKLM\Software\vmdisp9x\vesa\M2010Accel` (0 = pure VESA behaviour).
2. **GDI acceleration (DRV→VxD).** Hook `BitBlt` in the 16-bit driver (`HWBLT`): solid
   PATCOPY/BLACKNESS/WHITENESS fills and SRCCOPY screen-to-screen copies go to the engine via
   a new VxD service; everything else falls through to the DIB engine. Cursor exclusion uses
   the existing `FBHDA_access_rect/end` protocol.
3. **Hardware cursor** (CR C2..CD, pattern at CR C8..CA, 64x64, 2 bytes/px mono+AND or ARGB),
   replacing the VxD software cursor; removes the save/restore blits on every mouse move.
4. **Refresh-rate control + DPMS** through the CBIOS tables in the option ROM
   (`SetDisplay1RefreshRate` before `4F02h`; SEQ 01/CR B6 for DPMS).

Out of scope for now: TV-out (SAA7105), LCD scaler, video overlay, 3D.

## Ethernet plan

1. **Package the official RDC NDIS 5 driver for Win98 SE** with a cleaned INF: Win9x-only
   sections, hardware ID `PCI\VEN_17F3&DEV_6040` without the `SUBSYS_00000000` restriction,
   same registry parameters (speed/duplex, flow control). Alternative `.sys` builds (XP
   9.10.08, W2K 11.14.08) shipped in `alt-sys/` for A/B testing.
2. **DOS/real-mode fallback**: NDIS2 `RDCPCI.DOS` with `PROTOCOL.INI` sample, packet driver.
3. (Later, if the official driver misbehaves) open-source NDIS miniport written from the
   Linux `r6040.c` / FreeBSD `vte` register knowledge.

## Status (2026-10-18)

- VGA phases 1-4 implemented and built as build 2 (`dist/vga-release`, `dist/vga-debug`):
  `vga-src/rdc_m2010.c` + hooks. The eBOX has no COM port, so the debug build keeps a
  32 KB in-memory log read by `tools/m2010log.exe` (DeviceIoControl `OP_M2010_LOG`).
  Untested on hardware.
- Ethernet step 1 and 2 packaged (`net/`). Untested on hardware.
- Next after hardware feedback: colour/inverting hardware cursor (ARGB4444 mode),
  `ExtTextOut` opaque-rectangle fill, possibly an open-source R6040 NDIS miniport.

## Build

Open Watcom 2.0 (`C:\WATCOM`):

```
set WATCOM=C:\WATCOM
set INCLUDE=C:\WATCOM\h;C:\WATCOM\h\nt;C:\WATCOM\h\win
set PATH=C:\WATCOM\binnt64;%PATH%
cd vga-src
wmake -h -e vesamini.drv vesamini.vxd
```

`fixlink.exe` (built from `fixlink/fixlink.c`) patches the NE/LE headers that wlink gets
wrong; the DDK's 16-bit `RC.EXE` is not needed.
