# RDC M2010 display driver for the eBOX-3300MX — Windows 98 SE, build 33

Hardware: RDC M2010 integrated graphics (PCI `17F3:2010`) of the Vortex86MX SoC,
option ROM "RDC GPU 0.00.20".

This is the VMDisp9x 1.2025.0.119 VESA minidriver rebuilt from source with
hardware support for the M2010 added. Every feature below can be switched off
in the registry; with `M2010Accel=0` you get the plain VESA driver back.

- **Device detection**: the mini-VDD finds `17F3:2010`, maps the MMIO block
  (BAR1) and the relocated VGA ports (BAR2), reads the real VRAM size (CR AA)
  and leaves the hardware alone if the VBE framebuffer is not BAR0.
- **2D engine**: the back→front screen update, GDI solid fills (any ROP without
  a source, solid brushes), screen-to-screen copies (window moves, scrolling),
  and since build 27 the **background rectangles of text** (menus, list views,
  edit boxes, title bars: `ExtTextOut` opaque rectangles) and **Rectangle()
  interiors without an outline**; since build 28 also **pattern brushes**
  (colour patterns, dithered colours, brushes made from mono bitmaps such as
  desktop patterns) for PATCOPY fills: the 8x8 pattern is written once into a
  small VRAM tile and the engine copies it into place (`M2010PatFill`).
  Glyphs, hatched brushes, DIB blits and stretching still go through the DIB
  engine (see "What was looked at and left out").
- **Hardware cursor** (build 33): shapes up to 64x64 on the M2010's MMIO
  ("HQ") cursor as a double-buffered ARGB8888 pattern, mono and 16/32 bpp
  colour shapes (`M2010HwCursor`, `M2010CursorColor`). The cursor cannot
  invert the screen, so shapes with inverting pixels (the text I-beam) stay
  with the software cursor (`M2010HwInvert=1` draws those pixels black
  instead). Builds 27-32 never actually ran the hardware cursor: the VESA
  VxD was linked with a mouse object built without `-DVESA`.
- **DirectDraw page flips** (build 30-31): the M2010 display-start register
  is written in step with the vertical retrace (`M2010FlipSync`), and the
  software cursor moves to the new page right after it, so full-screen
  DirectDraw no longer flickers.
- **Monitor power-off (DPMS)**, confirmed with build 28C and on by default
  since build 29: Windows' monitor
  power requests (screen saver energy settings, Power Management "Turn off
  monitor") are answered through the mini-VDD 4.1 entries and switch the
  M2010 to standby/suspend/off with RDC's own registers (SR01 bit 5, CR B6)
  (`M2010Dpms`). Moving the mouse turns it back on; the monitor is always
  switched on before DOS or exit.
- **Refresh rate**: Display Properties → Settings → Advanced → Adapter →
  Refresh rate. The driver reads the choice from
  `HKCC\Display\Settings\RefreshRate` (Windows reports the monitor info as not
  valid, so the usual VDD fields are empty), matches it against the option
  ROM's timing table and passes the ROM's rate ID to the VESA BIOS in CR 8Dh.
  The ROM offers 640x480 60/72/75/85, 800x600 56/60/72/75/85, 1024x768
  60/70/75/85 and 1280x1024 60 Hz. The boot value of CR 8Dh is put back before
  every BIOS VGA mode set (full-screen DOS, exit), otherwise restart could hang.
- **Full-screen DOS** (Alt+Enter) and back: the boot font is kept and written
  back into plane 2 whenever a text mode comes up with a blank font
  (`M2010FontFix`).
- **Exit to DOS / shutdown screens** (build 27): at the last exit stage the
  text palette (attribute controller 0-15, DAC 0-63) and the boot font are
  restored when the card is in a text mode, so "Restart in MS-DOS mode" no
  longer comes up red and the final "safe to turn off" text is readable
  (`M2010ExitVga`).
- **Build number in the adapter name**: the running driver writes
  "RDC M2010 Graphics (Vortex86MX) - build 33R" (R release, D debug,
  C experiments) into Device Manager (DeviceDesc) and Display
  Properties → Advanced (DriverDesc) at every boot, so the name always matches
  the files actually installed, also after `TRY`/`BACK`/`GOOD`/`RESCUE`, which
  only swap the .DRV/.VXD (`M2010SetName`).

## Testing builds on the eBOX (TRY system)

Builds are folders in `exchange\out` on the PC (served over FTP). On the eBOX:

```
TRY V33R        download build V33R and install it at the next boot
BACK V          the display driver TRY replaced
GOOD V          your known-good set (saved with KEEP V)
RESCUE V        from "Command prompt only" if Windows does not start
```

Device Manager → Display adapters then shows which build is running.

## Folders

| Folder | Use |
| --- | --- |
| `vga-release/` | Full install package, build 33R: INF + driver + VMDisp9x support DLLs + `m2010log.exe` + `SETRATES.EXE`. Zipped as `RDC-M2010-Win98SE-build33.zip`. |
| `vga-debug/` | Same with build 33D: debug VxD with a 32 KB **in-memory log**, read with `m2010log.exe`. Zipped as `RDC-M2010-Win98SE-build33-debug.zip`. |
| `RDC-M2010.INF` | Current INF (build 33 strings and switches), same as in both folders. |

## Installation (first time)

1. Copy `vga-release` (or `vga-debug`, or unzip one of the zips) to the eBOX.
2. Device Manager → Display adapters → the current adapter → Driver → Update
   Driver → "Display a list…" → Have Disk → select `RDC-M2010.INF` →
   "RDC M2010 Graphics (Vortex86MX) - build 33".
3. Reboot. Pick resolution, colours and refresh rate in Display Properties.

The INF writes the registry switches below. Switches set by an older INF stay
in the registry until you reinstall; e.g. the build-27 INF wrote
`M2010CursorInvert`/`M2010CursorColor` = `0`, which keeps those cursor
features off even in build 28 until set to `1` (or the build-28 INF is
installed). Likewise `M2010Dpms` is only written by the build-29 INF; the
build-29 driver defaults to `1` when the value is missing. Build 33 reads
`M2010HwInvert` instead of `M2010CursorInvert`, so an old `1` there does not
switch on the black I-beam.

An adapter installed from an INF older than build 21 has no refresh-rate list:
run `SETRATES.EXE` once (it adds the ROM's rates to the installed adapter's
mode keys; `SETRATES /undo` removes them), then reboot.

## Known issue: Restart freezes with the NUSB USB 2.0 driver

With NUSB installed, Start → Restart and "Restart in MS-DOS mode" freeze in
real mode (blinking cursor, keyboard dead). The display and network drivers
both finish their exit cleanly; the freeze is in NUSB's EHCI (USB 2.0) driver
exit code. Stopping the controllers ourselves (`M2010UsbQuiesce=1`) does not
help. **Workaround (confirmed)**: Device Manager → Universal Serial Bus
controllers → disable the two "Enhanced Host Controller" (USB 2.0) entries and
keep the OHCI ones. USB keyboard/mouse keep working at USB 1.1 speed.

## Getting the log (debug builds)

In a windowed MS-DOS prompt: `m2010log > c:\m2010.txt` (or `SEND` on the
eBOX to upload it). `M2010:` lines show detection, engine state, refresh
decisions, the exit options and the adapter-name update.

## Registry switches

`HKEY_LOCAL_MACHINE\Software\vmdisp9x\vesa` (string values, read at boot):

| Value | Default | Meaning |
| --- | --- | --- |
| `M2010Accel` | `1` | `0` = do not touch the M2010 at all (plain VESA driver behaviour) |
| `M2010GdiBlt` | `1` | `0` = engine only for the back→front update; all GDI in software |
| `M2010TextFill` | `1` | `0` = text background rectangles back to the DIB engine (build 27) |
| `M2010RectFill` | `1` | `0` = Rectangle() interiors back to the DIB engine (build 27) |
| `M2010PatFill` | `1` | `0` = pattern brushes back to the DIB engine; read at boot (it reserves 128 KB of VRAM) (build 28) |
| `M2010MinArea` | `256` | smallest fill/copy (pixels) worth a ring transition |
| `M2010Async` | `1` | `0` = wait for every GDI engine operation (debug) |
| `M2010HwCursor` | `1` | `0` = always use the software cursor |
| `M2010HwInvert` | `0` | `1` = inverting shapes (I-beam) on the hardware cursor, inverting pixels drawn black (build 33) |
| `M2010HwCurYAdj` | `0` | lines added to the cursor y; `1` = RDC R0.0.9 behaviour (build 33) |
| `M2010FlipSync` | `1` | DirectDraw flips: `1` = write, then wait for retrace; `2` = wait, then write; `0` = immediate (build 30) |
| `M2010CursorColor` | `1` | `0` = colour cursors back to the software cursor |
| `M2010Dpms` | `1` | `0` = don't answer Windows' monitor power requests (standby/suspend/off) (build 28C, default since 29) |
| `M2010Refresh` | `0` | `60`/`70`/`72`/`75`/`85` = force this rate; `0` = follow Display Properties |
| `M2010FontFix` | `1` | `0` = no font keeper for full-screen DOS |
| `M2010ExitVga` | `1` | `0` = leave palette/font alone at the last exit stage |
| `M2010SetName` | `1` | `0` = do not write the build into the adapter name |
| `M2010ExitReport` | `0` | `1` = 8 s on-screen report (PIC, PCI IRQ state) at Sys_VM_Terminate, System_Exit and Sys_Critical_Exit (debug) |
| `M2010ShutTrace` | `0` release, `1` debug | shutdown tracer: control messages + heartbeat in a VRAM record, shown on screen after a 20 s stall |
| `M2010UsbQuiesce` | `0` | `1` = stop all USB host controllers at exit (did not fix NUSB) |
| `M2010Wbinvd` | `0` | `1` = flush CPU caches before each engine operation |
| `M2010SyncCopy` | `0` | `1` = wait for every back→front copy (debug) |
| `M2010FgMode` | `1` | `0` = treat fill colours as COLORREF (debug only) |
| `M2010OpLog` | `8` | log the first N GDI engine requests (debug builds) |
| `HWDoubleBuffer` | `2` | VMDisp9x: `0` = never flip, `1` = flip, `2` = flip with vsync |
| `VRAMLimit` | `128` | VMDisp9x: MB of VRAM made available |

If the machine does not boot to the desktop: `RESCUE V` from "Command prompt
only", or Safe Mode → regedit → `M2010Accel` = `0`.

## What was looked at and left out (build 28)

- **Glyphs through the engine's colour expansion**: the M2010 has
  CPU-to-screen and screen-to-screen colour-expand commands, but RDC's own
  Xorg driver ships both turned off (`ENG_CAP_ALL` leaves them out), and the
  DIB engine's only hook for drawing the rendered text (`DrawTextBitmap`
  through `ExtTextOutExt`) has an undocumented buffer layout. Too much risk
  for a modest gain, since text backgrounds already use the engine.
- **Hatched brushes**: their background depends on the background mode and
  a transparency mask; they stay with the DIB engine.
- **Polygon/region fills**: GDI already breaks regions into rectangles
  (PatBlt, accelerated). Polygons arrive one scanline per `Output` call, and a
  ring transition per scanline costs more than the CPU fill.
- **DIB-to-screen** (SetDIBitsToDevice, StretchDIBits, BitBlt from memory): the
  engine has no host-to-VRAM transfer path, and VRAM is system RAM on the
  Vortex86MX, so the CPU copy is already the fastest way.
- **DirectDraw**: blits and flips are implemented in the prebuilt
  `vmhal9x.dll` (VMHal9x, source at github.com/JHRobotics/vmhal9x). Flips already end in the
  VxD's display-start code, which programs the M2010 when its engine owns the
  display start. Engine blits for DirectDraw would need VMHal9x rebuilt from
  source with a new VxD call.

## Source

`../vga-src/` — VMDisp9x source with `rdc_m2010.c/.h` (engine, cursor, refresh,
font keeper, exit handling, adapter name; ring 0), hooks in `vxd_vesa.c`,
`vxd_mouse.c`, `vxd_main.c`, `vxd_vdd.c`, the GDI hooks (`BitBlt` incl. pattern
brushes, `ExtTextOut`, `Output`) in `dibcall.c`, refresh plumbing in `init.c`/`modes.c`, ring-3 call
wrappers in `pm16_calls.c`, tools in `tools/` (`m2010log.c`, `setrates.c`).
History: `../vga-src/CHANGES.md`. Build with Open Watcom 2.0 (always delete
`*.obj` first; the makefile does not track all dependencies):

```
set WATCOM=C:\WATCOM
set INCLUDE=C:\WATCOM\h;C:\WATCOM\h\nt;C:\WATCOM\h\win
set PATH=C:\WATCOM\binnt64;%PATH%
del *.obj
wmake -h -e vesamini.drv vesamini.vxd              (release, "33R")
wmake -h -e DBGMEM=1 vesamini.drv vesamini.vxd     (in-memory log, "33D")
wmake -h -e "FLAGS=-DDRV_VER_BUILD=119 -DHWBLT -DM2010_CUREXP" vesamini.drv vesamini.vxd   ("33C", no experiments left)
cd tools && wcl386 -q -bt=nt -l=nt m2010log.c
```

The build number lives in `rdc_m2010.h` (`M2010_BUILD_NO`).

Licenses: VMDisp9x and the RDC Xorg register definitions are MIT (see
`license.txt`).

## R6040-Own-Win98SE-2.0.zip / r6040-own/ - our own Ethernet driver (use this)

Open-source R6040.SYS 2.0 (build 10, MIT) that replaces RDC's RDCPCI5.SYS.
Confirmed on the eBOX (build 8 behaviour, 2026-10-20): it fixes the Windows 98
shutdown hang, and after a fresh boot a 32 MB round trip takes 9.0 s down /
6.1 s up (RDC-era builds: 12.8 s up). To install, run `INSTALL` from the
folder; it swaps the file under the existing RDC entry. A clean install via
`R6040.INF` (`INSTALL CLEAN`) also works. **Fallback: `C:\M2010\NETUNDO`**
(offline) or, from DOS, `copy C:\M2010\NET\RDCPCI5.ORG
C:\WINDOWS\SYSTEM\RDCPCI5.SYS`. RDC's package (`R6040-Ethernet-Win98SE.zip`)
is still needed as the base install and the fallback. See
`r6040-own/README.TXT`; sources and the changelog are in `net/r6040vxd`.
