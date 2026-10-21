# Vortex86MX-Win98

Windows 98 SE drivers for the **DM&P eBOX-3300MX** (DM&P Vortex86MX SoC):

| Device | Driver | Status |
| --- | --- | --- |
| RDC M2010 graphics (PCI `17F3:2010`) | `vga-src/`: VMDisp9x VESA minidriver with native M2010 support (2D engine, MMIO hardware cursor, page flips timed to the retrace, refresh rates, monitor power-off, DOS full-screen fixes) | build 33 |
| RDC R6040 Ethernet (PCI `17F3:6040`) | `net/r6040vxd/`: R6040.SYS, an open NDIS 3.0 miniport that replaces RDC's RDCPCI5.SYS (which hangs Windows 98 at shutdown) | 2.0 (build 10) |

Everything is tested on a real eBOX-3300MX. Download the drivers from the
[Releases](../../releases) page: `vga-b33` and `net-b10` are the current
ones, and every earlier test build is kept there too.

## Layout

| Path | What |
| --- | --- |
| `vga-src/` | Display driver source (VMDisp9x 1.2025.0.119 + `rdc_m2010.c/.h`). History: `vga-src/CHANGES.md`. |
| `dist/README.md` | Display driver user guide: install, registry switches, what was left out and why. |
| `net/r6040vxd/` | Ethernet driver source, INF, install batches, emulator tests (`test/`). Full notes and changelog in its README. |
| `net/README.md` | Notes on RDC's own R6040 drivers (not included here; get them from DM&P). |
| `exchange/` | Lab tooling: anonymous FTP server for the PC and the eBOX batch files (`GET`, `TRY`, `BACK`, `KEEP`, `GOOD`, `SEND`, `NETSEND`, `BIGTEST`, ...). |
| `testauto/` | Work in progress: on-box test agent + PC harness (`TEST-AUTOMATION.md`). |
| `PLAN.md` | The original project plan and hardware facts. |

## Building

Open Watcom 2.0 (`C:\WATCOM`), on Windows:

```
set WATCOM=C:\WATCOM
set INCLUDE=C:\WATCOM\h;C:\WATCOM\h\nt;C:\WATCOM\h\win
set PATH=C:\WATCOM\binnt64;%CD%\vga-src;%PATH%
cd vga-src
del *.obj
wmake -h -e vesamini.drv vesamini.vxd             (release)
wmake -h -e DBGMEM=1 vesamini.drv vesamini.vxd    (debug, in-memory log)
cd ..\net\r6040vxd
wmake -h -e
```

Python 3 is needed for the post-link fixers (`vga-src/tools/nefix.py`,
`net/r6040vxd/pefix.py`).

## Credits and licences

MIT (see `LICENSE`). The display driver is based on
[VMDisp9x](https://github.com/JHRobotics/vmdisp9x) by JHRobotics (MIT); the
release packages also contain JHRobotics' prebuilt
[VMHal9x](https://github.com/JHRobotics/vmhal9x) DirectDraw DLLs (MIT).
Register-level knowledge of the M2010 comes from RDC's open-source Xorg
driver. RDC's proprietary RDCPCI5.SYS and other third-party files are not
redistributed here.

The drivers were developed with Claude Code.
