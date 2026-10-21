# M2010 display driver — change log

Builds are tested on the eBOX-3300MX by the user. "Confirmed" means confirmed
on that hardware. R = release, D = debug (DBGMEM in-memory log), C = release
with the cursor experiments on.

## Build 33 (V33R, V33D) — 2026-10-21, confirmed on the eBOX (V33D: cursor shapes, DOS round trip, DirectDraw; V33R: automated install + 3 restarts)

- V32R on the eBOX: the hardware cursor was "completely shot" (garbled shape,
  trails/duplicates). The code programmed the CR C2..CD cursor with a 16-bit
  pattern, which RDC's own driver only uses for the A0 stepping; for PCI
  17F3:2010 RDCSetHWCaps selects the MMIO "HQ" cursor (rdc_cursor.c *_HQ).
- Rewritten for the HQ cursor: CTRL 0x580 (bit 31 MMIO mode, bit 1 show,
  bit 0 colour), OFFSET 0x584, POSITION 0x588 (y + 1), ADDRESS 0x58C, CTRL
  written back to latch. Every shape is loaded as a 64x64 ARGB8888 pattern
  (16 KB, the same reservation): mono black/white become opaque pixels, so
  the undocumented mono format is never used. Hiding also clears CR CB.
- The hardware cannot invert: shapes with inverting pixels (I-beam) stay in
  software. New switch `M2010HwInvert=1` puts them on the hardware cursor with
  those pixels black; the old `M2010CursorInvert` value (written as 1 by the
  build-28 INF) is no longer read.
- From RDC's newer T0.0.13 driver (rdc-research/m2010-T0.0.13, found by the
  folder-survey session): the pattern is double-buffered (two 16 KB buffers,
  each load writes the one not on screen, then switches 0x58C), and the y + 1
  of R0.0.9 is gone. y adjust is `M2010HwCurYAdj` (default 0; 1 = R0.0.9).

## Build 32 (V32R, V32D) — 2026-10-21

- **The hardware cursor was never active.** `vxd_mouse.obj` is built without
  `-DVESA`, so every `#ifdef VESA` block in vxd_mouse.c (the whole M2010
  cursor hookup: load, move, show, hide) was compiled out of vesamini.vxd;
  the link map marked `M2010_cursor_load/move/show` unreferenced. The pointer
  was always VMDisp9x's software cursor, so the "HW I-beam / colour cursor"
  results of builds 27-28 were the software cursor. Found while tracing the
  pointer blinking in DxDiag's windowed DirectDraw tests (the software cursor
  is erased and redrawn around every DirectDraw access to the screen).
- New `vxd_mouse_vesa.c` wrapper (`#define VESA` + vxd_mouse.c, like the other
  *_vesa.c files) is linked into vesamini.vxd instead. The M2010 cursor code
  now runs for the first time: `M2010HwCursor=0` returns to the software
  cursor; mono shapes, inverting pixels (M2010CursorInvert) and 16/32 bpp
  colour shapes (M2010CursorColor) as before, each falling back to software.
- Debug builds log every cursor call (`CUR` lines: ms, framebuffer accesses
  since the last entry, load/hide/show/invalidate, first 400).

## Build 31 (V31R, V31S, V31D) — 2026-10-21

- V30R confirmed: the DxDiag bouncing box no longer flickers, but the mouse
  pointer did. In that mode the pointer is the software cursor, which
  FBHDA_swap erased from the old page before the flip and drew on the new
  page after it; the retrace wait in between hid it for most of every frame.
  With the M2010 engine on, the cursor now moves right after the wait: erase
  from the page just hidden, then draw on the page just shown.

## Build 30 (V30R, V30S, V30D) — 2026-10-21

- **DirectDraw flips timed to the vertical retrace** (`M2010FlipSync`, default
  1). Reported with V29R: DxDiag's full-screen DirectDraw test (bouncing box)
  flickers. While the 2D engine is on, scan-out start comes from
  `REG_1ST_FLIP`, so the VBE 4F07 "set during vertical retrace" call only
  delayed things: our MMIO write landed just after the retrace started and
  showed a frame late, while DirectDraw already drew into the buffer still on
  screen. Now `M2010_set_display_start` polls input status 1 (relocated 3DAh,
  bit 3): mode 1 = leave a retrace in progress, write, wait for the next
  retrace to begin (VGA-style latch); mode 2 = wait for the retrace, then
  write (immediate register); 0 = build 29 behaviour. The BIOS call uses the
  no-wait subfunction while the engine is on, so a flip costs at most one
  frame. If bit 3 never toggles, the sync turns itself off (debug log line).
- V30S = V30R with mode 2 as the default (build name "30S"), to find out how
  the M2010 latches the register without regedit.

## Build 29 (V29R, V29D) — 2026-10-21

- **Monitor power-off on by default** (`M2010Dpms` default 1 in every build;
  the INF now writes `M2010Dpms=1`). Confirmed with V28C on the eBOX:
  standby/off and wake work, so the guessed register convention is right.
- No other code change: V29R's VxD differs from V28C's only in the build
  string ("28C" → "29R"); the .DRV is identical to build 28. With no
  experiments left there is no C build.
- **Package**: dist/vga-release (29R) and dist/vga-debug (29D), INF DriverVer
  4.2026.0.29 / "... - build 29", zipped as
  RDC-M2010-Win98SE-build29(-debug).zip. The build-28 zips are kept.

## Build 28 (V28R, V28D, V28C) — 2026-10-20

- **Pattern brushes on the engine** (`M2010PatFill`, default 1). PATCOPY
  BitBlts with an 8x8 pattern: colour pattern brushes and dithered colours
  (the DIB engine's own `dpBrushBits`) and brushes made from mono bitmaps
  (`PATTERNMONO`: 0 bits text colour, 1 bits background colour). The 16-bit
  driver hands the 64 device pixels to the VxD (`OP_M2010_PATBLT`); the VxD
  writes them into a 72x16-pixel tile in 128 KB of reserved VRAM (only when the
  pattern, depth or pitch changed) and fills the rectangle with engine copies:
  one block of up to 64x8 from the tile at the screen phase (x & 7, y & 7),
  then doubling to the right and downwards. Only the copy command is used; the
  M2010's pattern registers are not (RDC's colour-pattern register indexing in
  rdc_accel.c is visibly wrong). Hatched brushes stay with the DIB engine.
- **Cursor**: `M2010CursorInvert` and `M2010CursorColor` default to 1
  (confirmed with V27C: the M2010 mono pattern does AND/XOR, so the I-beam
  inverts in hardware).
- **Monitor power-off** (`M2010Dpms`, experiment, 1 only in C builds): the VxD
  fills the mini-VDD 4.1 entries SET_MONITOR_POWER_STATE (54) and
  GET_MONITOR_POWER_STATE_CAPS (55) when the VDD's table is long enough. The
  power state is taken from the first of ECX/EAX/EDX holding a single
  CM_POWERSTATE_Dn bit (the register convention is not in the DDK headers;
  debug builds log all registers). D1/D2/D3 = standby/suspend/off with RDC's
  registers (SR01 bit 5 blank, CR B6 bits 1..0), D0 = on. The monitor is
  switched on again before any VGA/DOS/exit state restore.
- **Assessed and left out**, with reasons in dist/README.md: glyph colour
  expansion (RDC ships it disabled), hatched brushes, polygon scanlines (a ring
  transition per scanline costs more than the CPU), DIB-to-screen (no
  host-to-VRAM path; VRAM is system RAM), DirectDraw engine blits (they live in
  the prebuilt VMHal9x DLL).
- **Package**: dist/vga-release (28R) and dist/vga-debug (28D) refreshed with
  the build-28 INF (DriverVer 4.2026.0.28, "... - build 28"), SETRATES.EXE,
  zipped as RDC-M2010-Win98SE-build28(-debug).zip.

## Build 27 (V27R, V27D, V27C) — 2026-10-20

- **Release defaults.** The exit report (8 s hold at three exit stages, forced
  text mode) is opt-in: `M2010ExitReport=1`. The shutdown tracer is off in
  release builds and on in debug builds (`M2010ShutTrace`). USB quiesce is off
  (`M2010UsbQuiesce=1` to turn it on). All three no longer depend on each other:
  the exit report and USB quiesce work with the tracer off.
- **Red DOS screen after "Restart in MS-DOS mode".** The exit report's red title
  row (attribute 4Fh) stayed in text memory; DOS scrolls with the attribute
  under the cursor, so the whole screen went red. At Sys_Critical_Exit, if the
  card is in a text mode, the driver clears any text it drew (report, trace
  lines) to grey on black, homes the cursor, and restores the standard text
  palette (AR 00-0F, EGA colours in DAC 00-3F). `M2010ExitVga=0` turns it off.
- **Black "safe to turn off" screen.** The same step writes the boot font back
  into plane 2 at the last exit stage.
- **More GDI on the 2D engine** (`dibcall.c`):
  - `ExtTextOut` opaque rectangles (text backgrounds, and ExtTextOut used as a
    fast fill). Follows DIBENG's own rules, read from the Win98 DDK debug
    DIBENG.DLL: rectangles are drawn when `lpOpaqueRect` is set and ETO_OPAQUE
    or a packed glyph format is used; packed formats pass a zero-terminated
    list; each is clipped to `lpClipRect`, which ETO_CLIPPED first narrows to
    the first rectangle. The glyphs then go to the DIB engine without the
    rectangles. `M2010TextFill=0` turns it off.
  - `Output(OS_RECTANGLE)` without a pen, solid brush, R2_COPYPEN (interior
    x1..x2-2, y1..y2-2 as GDI defines it). `M2010RectFill=0` turns it off.
  - Anything unusual, small (below `M2010MinArea`) or failing falls back to the
    DIB engine unchanged.
- **Cursor experiments** (off by default, on in V27C):
  - `M2010CursorInvert=1`: the mono pattern word is AND (bit 15) | XOR colour
    (RGB444), so inverting pixels are written as 8FFFh. RDC's drivers never use
    that combination; whether the M2010 inverts is unknown. If the I-beam
    becomes invisible, it does not.
  - `M2010CursorColor=1`: 16/32 bpp colour cursors in the ARGB4444 mode
    (CR CB bit 0, as `RDCLoadCursorARGB`). ARGB has no XOR, so colour cursors
    with inverting pixels stay in software.
- **Build number in the adapter name.** At Init_Complete the VxD writes
  "RDC M2010 Graphics (Vortex86MX) - build 27R/D/C" into the display class
  key's DriverDesc and the PCI devnode's DeviceDesc (only when different), so
  Device Manager shows the build actually running after TRY/BACK/GOOD/RESCUE.
  `M2010SetName=0` turns it off. INF: `DriverVer` 4.2026.0.27, device name
  "... - build 27", all new switches listed.

## Build 26 (V26D) — USB quiesce at System_Exit

EHCI halt + HCRESET + release of the USBLEGSUP OS-owned semaphore, OHCI reset,
at System_Exit. Worked as coded but did not fix the restart freeze: NUSB's EHCI
driver hangs in its own exit code first. Confirmed workaround: disable the two
"Enhanced Host Controller" entries in Device Manager.

## Build 25 — correct PCI config reads

`vmware/pci.c` `PCI_ConfigRead16/8` ignore `offset & 3`; build 24 misread every
device. Local `cfg16`/`cfg8` built on `PCI_ConfigRead32`. The report showed the
freeze happens after our System_Exit and before Sys_Critical_Exit.

## Build 24 — USB scan, stage reports

USB host controllers mapped at boot and stopped at Sys_Critical_Exit; exit
reports held 8 s at Sys_VM_Terminate, System_Exit and Sys_Critical_Exit.

## Build 23 — exit report

On-screen report at Sys_Critical_Exit: PIC IRR/ISR/IMR/ELCR, every PCI
device's IRQ/CMD/STAT/INTx, R6040 registers. Confirmed: with the USB
controllers disabled, "Restart in MS-DOS mode" works (DOS screen red — fixed in
build 27).

## Build 22 — CR 8D restore

The refresh ID chosen for the desktop (CR 8Dh) was still set when the BIOS set
mode 3 at exit. The boot value is saved and restored in
`M2010_vga_state_restore` (PRE_HIRES_TO_VGA, HIRES disable, Device_Exit).

## Build 21 — refresh rate from HKCC

The VDD reports MONITOR_INFO_NOT_VALID, so the requested rate is read from
`HKCC\Display\Settings\RefreshRate` in `VESA_setmode_phy`. Confirmed: the rate
changes. INF gained the MODES rate lists; `tools/setrates.c` patches an
installed adapter.

## Build 20 — forced text mode after a stall

After 20 s without a control message the tracer programs 80x25 text mode by
registers, reloads the boot font and draws the trace.

## Build 19 — trace on screen

The last 14 trace lines are drawn at the bottom of the text screen on every
heartbeat (the eBOX has no reset button, so the VRAM record is lost).

## Build 18 — shutdown tracer

Control messages and a 250 ms heartbeat with R6040 MCR0/MCR1/MIER in a 4 KB
VRAM record, printed as PREV-SHUTDOWN on the next boot.

## Build 17 — font keeper

Full-screen DOS was black with a blinking cursor (blank plane-2 font). The boot
font is saved and written back for 3 s after every switch to a VGA mode when a
text mode shows a blank font. Confirmed: fixes Alt+Enter DOS and the way back.

## Builds 1–16

M2010 detection, 2D engine for the back→front update and GDI fills/copies, HW
mono cursor, ROM refresh table, colour fixes at 16/32 bpp (confirmed), debug
log (DBGMEM + m2010log). Build 16 / U1 (upstream VMDisp9x) A/B test showed the
shutdown hang was not the display driver (it was RDC's R6040 driver, replaced by
our own R6040.SYS).
