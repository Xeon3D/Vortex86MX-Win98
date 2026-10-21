/*****************************************************************************
RDC M2010 (Vortex86MX integrated graphics) hardware support for the VMDisp9x
VESA mini-VDD. Ring-0 (VxD) side.

Register knowledge comes from RDC's MIT-licensed Xorg driver (rdc_2dtool.h,
rdc_vgatool.h, rdc_cursor.c). The 2D engine register block is at MMIO
(PCI BAR1) + 0x8000, the extended CRTC registers are reached through the
relocated VGA ports at PCI BAR2 (+0x54 index, +0x55 data).

Copyright (c) 2026, MIT license (same as the surrounding VMDisp9x code).
*****************************************************************************/
#ifndef __RDC_M2010_H__INCLUDED__
#define __RDC_M2010_H__INCLUDED__

/* M2010_CAP_* and M2010_BLT_* are shared with the 16-bit driver: 3d_accel.h */

/* build number shown in the adapter name ("... - build 27R"); bump per build */
#define M2010_BUILD_NO "33"

/* Init_Complete: put the build into the adapter's DriverDesc/DeviceDesc */
void  M2010_set_name();

/* PATCOPY with an 8x8 colour pattern (device pixels at pat_lin, linear) */
BOOL  M2010_gdi_pattern(DWORD surf, DWORD dx, DWORD dy, DWORD w, DWORD h,
                        DWORD pat_lin, DWORD rop);

/* monitor power (DPMS) through the mini-VDD 4.1 entries */
void  M2010_dpms_register(DWORD *table, DWORD length);
void  M2010_dpms_on();

/* called once from VESA_init_hw() after the VBE framebuffer is mapped */
BOOL  M2010_init(DWORD vbe_fb_phy);

BOOL  M2010_present();
DWORD M2010_caps();
DWORD M2010_min_area();

/* 2D engine state follows the display mode: enable after a VBE mode set
 * (visible = byte offset of the scan-out surface), disable before any VGA/DOS
 * transition and before the next mode set. */
void  M2010_engine_enable(DWORD visible);
void  M2010_engine_disable();

/* wait until the engine finished every queued operation */
void  M2010_sync();

/* program the display start address (needed while the engine owns it),
 * timed to the vertical retrace (M2010FlipSync) */
void  M2010_set_display_start(DWORD offset);
BOOL  M2010_engine_active();

/* cursor call trace into the in-memory log (debug builds only) */
#ifdef DBGMEM
void  M2010_trace_cursor(const char *what, DWORD a, DWORD b);
void  M2010_trace_access();
#else
#define M2010_trace_cursor(what, a, b)
#define M2010_trace_access()
#endif

/* screen-to-screen copy between two surfaces of the current mode's pitch.
 * Returns FALSE (and touches nothing) when the engine is not active. */
BOOL  M2010_copy(DWORD src_surf, DWORD dst_surf,
                 DWORD sx, DWORD sy, DWORD dx, DWORD dy,
                 DWORD w, DWORD h, DWORD rop);

/* solid fill with a ROP (pattern = color) */
BOOL  M2010_fill(DWORD dst_surf, DWORD x, DWORD y, DWORD w, DWORD h,
                 DWORD color, DWORD rop);

/* GDI request from the 16-bit driver (OP_M2010_BLT): includes cursor
 * exclusion and the back->front update, finishes synchronously. */
BOOL  M2010_gdi_blt(DWORD surf, DWORD sx, DWORD sy, DWORD dx, DWORD dy,
                    DWORD w, DWORD h, DWORD color, DWORD rop_flags);

/* hardware cursor: 64x64 mono pattern in reserved VRAM (CR C2..CD) */
BOOL  M2010_cursor_available();
BOOL  M2010_cursor_load(int w, int h, int hotx, int hoty, int bpp,
                        const BYTE *and_bits, const BYTE *xor_bits, DWORD mask_pitch);
void  M2010_cursor_move(int x, int y);
void  M2010_cursor_show();
void  M2010_cursor_hide();
void  M2010_cursor_mode_set();

/* put the boot-time (text mode) RDC extended registers back */
void  M2010_vga_state_restore();
void  M2010_dump_vga(const char *tag);
void  M2010_dump_text_and_font(const char *tag);
void  M2010_font_watch(const char *why);
void  M2010_font_timer_arm();
void  M2010_font_unwatch();
void  M2010_trace_arm(const char *why);
void  M2010_trace_event(const char *what);
void  M2010_trace_timer_arm();
void  __stdcall M2010_trace_ctrl(DWORD type);

/* refresh rate: choose a ROM timing for the next VBE mode set, returns Hz */
DWORD M2010_refresh_select(DWORD w, DWORD h, DWORD rr_min, DWORD rr_max);

#endif /* __RDC_M2010_H__INCLUDED__ */
