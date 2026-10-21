/*****************************************************************************
RDC M2010 2D engine support for the VMDisp9x VESA mini-VDD (ring 0).

The display mode itself is still set by the VESA BIOS (INT 10h in V86 mode);
this module only adds what the BIOS cannot give us:
  - detection of the M2010 PCI function and its BARs,
  - the 2D BitBlt engine (screen-to-screen copies and solid fills) used for
    the back->front buffer update and for GDI BitBlt requests,
  - the display start register that the engine takes over once enabled.

Everything here is optional: when the device is missing, the registry disables
it, or the engine ever fails to go idle, the callers fall back to the original
software paths.

Copyright (c) 2026, MIT license (same as the surrounding VMDisp9x code).
Register layout from RDC's Xorg driver (Copyright (C) 2009 RDC Semiconductor,
MIT license).
*****************************************************************************/
#define VESA

#include "winhack.h"
#include "vmm.h"
#include "vxd.h"
#include "vxd_lib.h"
#include "3d_accel.h"
#include "pci.h"
#include "rdc_m2010.h"

#define IO_IN8
#define IO_OUT8
#define IO_IN16
#include "io32.h"

#include "code32.h"

extern FBHDA_t *hda;

#define M2010_VENDOR_ID   0x17F3
#define M2010_DEVICE_ID   0x2010
#define M2010_MMIO_SIZE   0x20000UL   /* DEFAULT_MMIO_SIZE in rdc.h */

/* 2D engine registers, offsets into the MMIO BAR */
#define REG_SRC_BASE      0x8000
#define REG_SRC_PITCH     0x8004
#define REG_DST_BASE      0x8008
#define REG_DST_PITCH     0x800C      /* pitch << 16 | dest height */
#define REG_DST_XY        0x8010
#define REG_SRC_XY        0x8014
#define REG_RECT_XY       0x8018      /* width << 16 | height */
#define REG_FG            0x801C
#define REG_BG            0x8020
#define REG_CLIP1         0x8034
#define REG_CLIP2         0x8038
#define REG_CMD           0x803C      /* writing this starts the operation */
#define REG_1ST_FLIP      0x8040      /* bit 31 enable, bits 3..27 display start */
#define REG_CMDQ_BASE     0x8044
#define REG_CMDQ_WRITE    0x8048
#define REG_CMDQ_READ     0x804C      /* bit 31 = engine busy */

#define CMD_BITBLT        0x00000000UL
#define CMD_COLOR_08      0x00000000UL
#define CMD_COLOR_16      0x00000010UL
#define CMD_COLOR_32      0x00000020UL
#define CMD_PAT_FGCOLOR   0x00000000UL
#define CMD_Y_DEC         0x00100000UL
#define CMD_X_DEC         0x00200000UL

#define FLIP_ENABLE       0x80000000UL
#define FLIP_BASE_MASK    0x0FFFFFF8UL

/* command queue control: 0xF0000000 | VM_CMD_MMIO (2 << 24) */
#define CMDQ_MMIO_MODE    0xF2000000UL

#define ENG_MAX_RECT      0x7FFUL     /* 11-bit width/height fields */
#define ENG_MAX_XY        0xFFFUL     /* 12-bit coordinate fields */
#define ENG_MAX_PITCH     0x1FFFUL
#define ENG_IDLE_SPINS    4000000UL   /* ~ a few hundred ms even on a slow CPU */
#define REG_WRITE_RETRIES 1000

#define ROP_SRCCOPY       0xCC

static PCIAddress     m2010_pci;
static BOOL           m2010_present = FALSE;
static BOOL           m2010_engine  = FALSE;  /* engine enabled for this mode */
static BOOL           m2010_broken  = FALSE;  /* engine timed out: never again */
static BOOL           m2010_pending = FALSE;  /* an operation may be running */
static volatile DWORD *m2010_mmio   = NULL;
static WORD           m2010_io      = 0;      /* BAR2: relocated VGA ports */
static DWORD          m2010_fb_phys = 0;
static DWORD          m2010_vram_cr = 0;      /* VRAM size from CR AA */
static DWORD          m2010_visible = 0;      /* display start we programmed */

/* registry: HKLM\Software\vmdisp9x\vesa */
static DWORD conf_accel    = 1;   /* M2010Accel:    0 = pure VESA behaviour */
static DWORD conf_gdiblt   = 1;   /* M2010GdiBlt:   0 = engine only for back->front */
static DWORD conf_min_area = 256; /* M2010MinArea:  smallest GDI blit (pixels) */
static DWORD conf_wbinvd   = 0;   /* M2010Wbinvd:   flush caches before each op */
static DWORD conf_sync     = 0;   /* M2010SyncCopy: wait after back->front copy */

static DWORD conf_cursor   = 1;   /* M2010HwCursor: 0 = software cursor */
static DWORD conf_refresh  = 0;   /* M2010Refresh:  Hz to force, 0 = Windows/BIOS choice */
static DWORD conf_fgmode   = 1;   /* M2010FgMode:   1 = driver passes device pixels (default), 0 = convert a COLORREF */
static DWORD conf_oplog    = 8;   /* M2010OpLog:    log the first N GDI engine requests */
static DWORD conf_async    = 1;   /* M2010Async:    0 = wait for every GDI engine op (debug) */
static DWORD conf_textfill = 1;   /* M2010TextFill: ExtTextOut opaque rectangles on the engine */
static DWORD conf_rectfill = 1;   /* M2010RectFill: Output(OS_RECTANGLE) solid interiors on the engine */
static DWORD conf_patfill  = 1;   /* M2010PatFill:  8x8 colour pattern brushes (PATCOPY) on the engine */
#ifndef M2010_FLIPSYNC_DEFAULT
#define M2010_FLIPSYNC_DEFAULT 1  /* the "S" test build uses 2 */
#endif
static DWORD conf_flipsync = M2010_FLIPSYNC_DEFAULT; /* M2010FlipSync: page flips timed to the vertical retrace */

static char M2010_conf_path[] = "Software\\vmdisp9x\\vesa";

/* ---- hardware cursor (rdc_cursor.c) ---------------------------------- */
#define CUR_SIZE          64
#define CUR_BUF           0x4000UL                    /* one 64x64 ARGB8888 pattern */
#define CUR_RESERVE       (2 * CUR_BUF)               /* two, alternated (VRAM top) */
#define HWC_CTRL          0x580                       /* HQ cursor (rdc_cursor.h) */
#define HWC_OFFSET        0x584
#define HWC_POSITION      0x588
#define HWC_ADDRESS       0x58C
#define HWC_CTRL_MMIO     0x80000000UL
#define HWC_CTRL_SHOW     0x00000002UL
#define HWC_CTRL_COLOR    0x00000001UL

/* M2010HwInvert: 1 = shapes with inverting pixels (I-beam) on the hardware
 *   cursor, those pixels drawn black; 0 (default) = software cursor for them.
 * M2010CursorColor: 16/32 bpp colour shapes on the hardware cursor. */
static DWORD conf_curinvert = 0;
static DWORD conf_curcolor  = 1;
/* M2010HwCurYAdj: lines added to the cursor y. RDC R0.0.9 adds 1
 *   (RDCSetCursorPosition_HQ y++), T0.0.13 (2013) no longer does; 0 here. */
static DWORD conf_curyadj   = 0;
static DWORD m2010_cur_next = 0;         /* pattern buffer for the next load */

/* ---- pattern brush scratch (M2010_gdi_pattern) -------------------------- */
#define PAT_RESERVE       0x20000UL   /* 16 rows at the largest pitch (8 KB) */
#define PAT_TILE_W        64          /* first copy: up to 64 x 8 pixels */
#define PAT_ROWS          16          /* 8 rows + 8 for any vertical phase */
#define PAT_COLS          (PAT_TILE_W + 8)

static DWORD m2010_pat_off   = 0;      /* VRAM offset of the scratch tile */
static BOOL  m2010_pat_ready = FALSE;
static BYTE  m2010_pat_last[8 * 8 * 4];
static DWORD m2010_pat_key   = 0;      /* bpp << 16 | pitch the tile was built for, 0 = none */

static DWORD m2010_cur_off     = 0;      /* VRAM offset of the pattern */
static BOOL  m2010_cur_ready   = FALSE;  /* pattern memory reserved */
static BOOL  m2010_cur_loaded  = FALSE;  /* a shape is in the pattern memory */
static BOOL  m2010_cur_visible = FALSE;
static int   m2010_cur_hotx    = 0;
static int   m2010_cur_hoty    = 0;
static int   m2010_cur_w       = 0;
static int   m2010_cur_h       = 0;
static int   m2010_cur_x       = 0;      /* last hotspot position */
static int   m2010_cur_y       = 0;

/* ---- refresh rate (option ROM VPIT table, CInt10.c) ------------------ */
#define ROM_PHYS          0x000C0000UL
#define ROM_SIZE          0x8000UL
#define CR_RRATE_ID       0x8D       /* SCRATCH_13, bits 7..1 = refresh ID */
#define RR_ATTR_DISABLE   0x0001
#define MODE_INFO_SIZE    11
#define RRATE_SIZE        19

static BYTE *m2010_rom   = NULL;     /* mapped option ROM shadow */
static DWORD m2010_vpit  = 0;        /* offset of the first MODE_INFO */

/* RRate_ID -> Hz (typedef.h RR60..RR72, TV ids rounded) */
static const BYTE rr_id_hz[16] = {60, 50, 60, 50, 56, 60, 24, 70, 75, 80, 85, 90, 100, 120, 72, 65};

/* ---- register access ----------------------------------------------- */

#define MMIO(_off) (m2010_mmio[(_off) >> 2])

#define SEQ_INDEX  ((WORD)(m2010_io + 0x44))
#define SEQ_DATA   ((WORD)(m2010_io + 0x45))
#define CRTC_INDEX ((WORD)(m2010_io + 0x54))
#define CRTC_DATA  ((WORD)(m2010_io + 0x55))

static void cr_write(BYTE index, BYTE value)
{
	outp(CRTC_INDEX, index);
	outp(CRTC_DATA, value);
}

static BYTE cr_read(BYTE index)
{
	outp(CRTC_INDEX, index);
	return inp(CRTC_DATA);
}

static void cr_mask(BYTE index, BYTE and_mask, BYTE or_value)
{
	cr_write(index, (BYTE)((cr_read(index) & and_mask) | or_value));
}

/* CR 80 = A8h opens the RDC extended register bank (vRDCOpenKey) */
static void cr_unlock()
{
	cr_write(0x80, 0xA8);
}

static void sr_write(BYTE index, BYTE value)
{
	outp(SEQ_INDEX, index);
	outp(SEQ_DATA, value);
}

static BYTE sr_read(BYTE index)
{
	outp(SEQ_INDEX, index);
	return inp(SEQ_DATA);
}

/* RDC extended registers that the VESA BIOS changes for hi-res modes and
 * that a plain VGA register restore does not touch (RDCSave/RDCRestore in
 * rdc_driver.c): PLL CR C0/C1/CF, CR A3, CR BB, SR 58/70/74. Snapshot taken
 * in text mode at boot, put back whenever we leave hi-res. */
static BYTE  vga_save_cr[6];   /* A3 A4 BB C0 C1 CF */
static BYTE  vga_save_sr[3];   /* 58 70 74 */
static BOOL  vga_saved = FALSE;
static BYTE  vga_save_8d;      /* CR 8D refresh ID as the BIOS left it in text mode */
static const BYTE vga_cr_idx[6] = {0xA3, 0xA4, 0xBB, 0xC0, 0xC1, 0xCF};
static const BYTE vga_sr_idx[3] = {0x58, 0x70, 0x74};

static void vga_state_save()
{
	int i;
	cr_unlock();
	for(i = 0; i < 6; i++) vga_save_cr[i] = cr_read(vga_cr_idx[i]);
	for(i = 0; i < 3; i++) vga_save_sr[i] = sr_read(vga_sr_idx[i]);
	vga_save_8d = cr_read(0x8D);
	vga_saved = TRUE;
	dbg_printf("M2010: text mode state CR A3=%X A4=%X BB=%X C0=%X C1=%X CF=%X SR 58=%X 70=%X 74=%X\n",
		vga_save_cr[0], vga_save_cr[1], vga_save_cr[2], vga_save_cr[3], vga_save_cr[4], vga_save_cr[5],
		vga_save_sr[0], vga_save_sr[1], vga_save_sr[2]);
}

void M2010_vga_state_restore()
{
	int i;
	if(!m2010_present || !vga_saved) return;
	M2010_dpms_on();                       /* never leave for DOS with the monitor off */
	cr_unlock();
	for(i = 3; i < 6; i++) cr_write(vga_cr_idx[i], vga_save_cr[i]);   /* PLL */
	cr_mask(0xA3, (BYTE)~0x20, (BYTE)(vga_save_cr[0] & 0x20));
	cr_write(0xBB, vga_save_cr[2]);
	for(i = 0; i < 3; i++) sr_write(vga_sr_idx[i], vga_save_sr[i]);
	/* the refresh ID chosen for the desktop mode (M2010_refresh_select) must not
	   reach a BIOS VGA/text mode set: shutdown, full-screen DOS, driver exit */
	cr_write(0x8D, vga_save_8d);
	dbg_printf("M2010: extended VGA state restored (CR 8D=%X)\n", vga_save_8d);
}

/* the RDC driver re-reads every engine register until the value sticks */
static BOOL mmio_write(DWORD off, DWORD value)
{
	DWORD i;
	for(i = 0; i < REG_WRITE_RETRIES; i++)
	{
		MMIO(off) = value;
		if(MMIO(off) == value)
		{
			return TRUE;
		}
	}
	dbg_printf("M2010: MMIO %lX refuses value %lX\n", off, value);
	return FALSE;
}

/* The DIB engine's "physical colour" of a solid brush is a Windows COLORREF
 * (0x00BBGGRR) at every depth; the engine's FG register wants the device
 * pixel. M2010FgMode=1 passes the value through unchanged (debug only). */
static DWORD fg_encode(DWORD color)
{
	DWORD r = color & 0xFF, g = (color >> 8) & 0xFF, b = (color >> 16) & 0xFF;

	if(conf_fgmode == 1) return color;

	switch(hda->bpp)
	{
		case 16: /* RGB 5:6:5 */
			return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
		case 32: /* xRGB 8:8:8:8 */
			return (r << 16) | (g << 8) | b;
		default: /* 8 bpp: palette index in the low byte */
			return color & 0xFF;
	}
}

static DWORD color_bits()
{
	switch(hda->bpp)
	{
		case 8:  return CMD_COLOR_08;
		case 16: return CMD_COLOR_16;
		default: return CMD_COLOR_32;
	}
}

static BOOL engine_wait_idle()
{
	DWORD spins = ENG_IDLE_SPINS;
	DWORD st = 0, wp = 0;

	if(!m2010_engine)
	{
		m2010_pending = FALSE;
		return TRUE;
	}

	do
	{
		st = MMIO(REG_CMDQ_READ);
		wp = MMIO(REG_CMDQ_WRITE);
		if(((st & 0x80000000UL) == 0) && ((st & 0x3FFFFUL) == (wp & 0x3FFFFUL)))
		{
			m2010_pending = FALSE;
			return TRUE;
		}
	} while(--spins);

	dbg_printf("M2010: engine never went idle (state=%lX wp=%lX), acceleration off\n", st, wp);
	m2010_broken = TRUE;
	M2010_engine_disable();
	return FALSE;
}

/* before touching the engine registers: previous op done, caches flushed */
static BOOL engine_begin()
{
	if(!m2010_engine) return FALSE;
	if(m2010_pending)
	{
		if(!engine_wait_idle()) return FALSE;
	}
	if(conf_wbinvd)
	{
		_asm { wbinvd };
	}
	return TRUE;
}

/* one engine rectangle (all limits already satisfied by the caller) */
static BOOL engine_rect_copy(DWORD cmd, DWORD src_base, DWORD dst_base,
	DWORD sx, DWORD sy, DWORD dx, DWORD dy, DWORD w, DWORD h)
{
	DWORD pitch = hda->pitch;

	if(!mmio_write(REG_SRC_PITCH, pitch << 16)) return FALSE;
	if(!mmio_write(REG_DST_PITCH, (pitch << 16) | ENG_MAX_RECT)) return FALSE;
	if(!mmio_write(REG_SRC_BASE,  src_base)) return FALSE;
	if(!mmio_write(REG_DST_BASE,  dst_base)) return FALSE;
	if(!mmio_write(REG_DST_XY,    ((dx & ENG_MAX_XY) << 16) | (dy & ENG_MAX_XY))) return FALSE;
	if(!mmio_write(REG_SRC_XY,    ((sx & ENG_MAX_XY) << 16) | (sy & ENG_MAX_XY))) return FALSE;
	if(!mmio_write(REG_RECT_XY,   ((w & ENG_MAX_RECT) << 16) | (h & ENG_MAX_RECT))) return FALSE;
	/* an I/O read serializes any posted CPU writes before the engine starts */
	cr_read(0x80);
	MMIO(REG_CMD) = cmd;
	m2010_pending = TRUE;
	return TRUE;
}

static BOOL engine_rect_fill(DWORD cmd, DWORD dst_base, DWORD x, DWORD y,
	DWORD w, DWORD h, DWORD color)
{
	DWORD pitch = hda->pitch;

	if(!mmio_write(REG_DST_PITCH, (pitch << 16) | ENG_MAX_RECT)) return FALSE;
	if(!mmio_write(REG_FG,        fg_encode(color))) return FALSE;
	if(!mmio_write(REG_DST_BASE,  dst_base)) return FALSE;
	if(!mmio_write(REG_DST_XY,    ((x & ENG_MAX_XY) << 16) | (y & ENG_MAX_XY))) return FALSE;
	if(!mmio_write(REG_RECT_XY,   ((w & ENG_MAX_RECT) << 16) | (h & ENG_MAX_RECT))) return FALSE;
	cr_read(0x80);
	MMIO(REG_CMD) = cmd;
	m2010_pending = TRUE;
	return TRUE;
}


/* ---- VGA state dump (diagnostics for the DOS full-screen problem) -------- */
static BYTE *vga_textmem = NULL;

static BYTE vga_idx_read(WORD index_port, WORD data_port, BYTE index)
{
	outp(index_port, index);
	return inp(data_port);
}

void M2010_dump_vga(const char *tag)
{
	BYTE v[16];
	int i;
	if(!m2010_present) return;
	dbg_printf("VGA[%s] MISC=%X ST1=%X\n", tag, inp((WORD)(m2010_io + 0x4C)), inp((WORD)(m2010_io + 0x5A)));
	for(i = 0; i < 8; i++) v[i] = vga_idx_read(SEQ_INDEX, SEQ_DATA, (BYTE)i);
	dbg_printf("VGA[%s] SR00-07: %X %X %X %X %X %X %X %X\n", tag, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
	for(i = 0; i < 9; i++) v[i] = vga_idx_read((WORD)(m2010_io + 0x4E), (WORD)(m2010_io + 0x4F), (BYTE)i);
	dbg_printf("VGA[%s] GR00-08: %X %X %X %X %X %X %X %X %X\n", tag, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8]);
	cr_unlock();
	dbg_printf("VGA[%s] CR A3=%X A4=%X A6=%X A7=%X A8=%X BB=%X C0=%X C1=%X CF=%X D0=%X DF=%X\n", tag,
		cr_read(0xA3), cr_read(0xA4), cr_read(0xA6), cr_read(0xA7), cr_read(0xA8), cr_read(0xBB),
		cr_read(0xC0), cr_read(0xC1), cr_read(0xCF), cr_read(0xD0), cr_read(0xDF));
	dbg_printf("VGA[%s] SR 1E=%X 1F=%X 20=%X 21=%X 32=%X 4F=%X 50=%X 58=%X 70=%X 74=%X\n", tag,
		sr_read(0x1E), sr_read(0x1F), sr_read(0x20), sr_read(0x21), sr_read(0x32), sr_read(0x4F), sr_read(0x50),
		sr_read(0x58), sr_read(0x70), sr_read(0x74));
	/* attribute controller: reset flip-flop with status 1, index with PAS bit */
	inp((WORD)(m2010_io + 0x5A));
	for(i = 0; i < 8; i++)
	{
		inp((WORD)(m2010_io + 0x5A));
		outp((WORD)(m2010_io + 0x40), (BYTE)(0x20 | i));
		v[i] = inp((WORD)(m2010_io + 0x41));
	}
	inp((WORD)(m2010_io + 0x5A));
	outp((WORD)(m2010_io + 0x40), 0x30); v[8] = inp((WORD)(m2010_io + 0x41));
	inp((WORD)(m2010_io + 0x5A));
	outp((WORD)(m2010_io + 0x40), 0x20); /* leave with PAS set (display on) */
	dbg_printf("VGA[%s] AR00-07: %X %X %X %X %X %X %X %X  AR10=%X\n", tag, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8]);
	/* DAC entries 0, 7, 15 */
	outp((WORD)(m2010_io + 0x47), 0);
	for(i = 0; i < 3; i++) v[i] = inp((WORD)(m2010_io + 0x49));
	outp((WORD)(m2010_io + 0x47), 7);
	for(i = 3; i < 6; i++) v[i] = inp((WORD)(m2010_io + 0x49));
	outp((WORD)(m2010_io + 0x47), 15);
	for(i = 6; i < 9; i++) v[i] = inp((WORD)(m2010_io + 0x49));
	dbg_printf("VGA[%s] DAC0=%X,%X,%X DAC7=%X,%X,%X DAC15=%X,%X,%X\n", tag, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8]);
	if(vga_textmem == NULL)
	{
		vga_textmem = (BYTE*)_MapPhysToLinear(0xB8000, 0x1000, 0);
		if((DWORD)vga_textmem == 0xFFFFFFFFUL) vga_textmem = NULL;
	}
	if(vga_textmem)
	{
		dbg_printf("VGA[%s] B8000: %X %X %X %X %X %X %X %X %X %X %X %X %X %X %X %X\n", tag,
			vga_textmem[0], vga_textmem[1], vga_textmem[2], vga_textmem[3], vga_textmem[4], vga_textmem[5], vga_textmem[6], vga_textmem[7],
			vga_textmem[8], vga_textmem[9], vga_textmem[10], vga_textmem[11], vga_textmem[12], vga_textmem[13], vga_textmem[14], vga_textmem[15]);
	}
}

/* text rows and the plane-2 font glyph of 'A' (read through the VGA memory
 * window at A0000 with the sequencer/graphics controller temporarily in
 * planar read mode; registers are put back afterwards) */
static BYTE *vga_planemem = NULL;

void M2010_dump_text_and_font(const char *tag)
{
	BYTE sr2, sr4, gr4, gr5, gr6, g[16];
	int i;
	if(!m2010_present) return;
	if(vga_textmem == NULL)
	{
		vga_textmem = (BYTE*)_MapPhysToLinear(0xB8000, 0x1000, 0);
		if((DWORD)vga_textmem == 0xFFFFFFFFUL) vga_textmem = NULL;
	}
	if(vga_planemem == NULL)
	{
		vga_planemem = (BYTE*)_MapPhysToLinear(0xA0000, 0x10000, 0);
		if((DWORD)vga_planemem == 0xFFFFFFFFUL) vga_planemem = NULL;
	}
	if(vga_textmem)
	{
		BYTE *r0 = vga_textmem, *r23 = vga_textmem + 23*160, *r24 = vga_textmem + 24*160;
		dbg_printf("TXT[%s] row0: %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X\n", tag,
			r0[0], r0[1], r0[2], r0[3], r0[4], r0[5], r0[6], r0[7], r0[8], r0[9], r0[10], r0[11], r0[12], r0[13], r0[14], r0[15]);
		dbg_printf("TXT[%s] row23: %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X\n", tag,
			r23[0], r23[1], r23[2], r23[3], r23[4], r23[5], r23[6], r23[7], r23[8], r23[9], r23[10], r23[11], r23[12], r23[13], r23[14], r23[15]);
		dbg_printf("TXT[%s] row24: %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X\n", tag,
			r24[0], r24[1], r24[2], r24[3], r24[4], r24[5], r24[6], r24[7], r24[8], r24[9], r24[10], r24[11], r24[12], r24[13], r24[14], r24[15]);
	}
	if(vga_planemem)
	{
		sr2 = sr_read(0x02); sr4 = sr_read(0x04);
		gr4 = vga_idx_read((WORD)(m2010_io + 0x4E), (WORD)(m2010_io + 0x4F), 4);
		gr5 = vga_idx_read((WORD)(m2010_io + 0x4E), (WORD)(m2010_io + 0x4F), 5);
		gr6 = vga_idx_read((WORD)(m2010_io + 0x4E), (WORD)(m2010_io + 0x4F), 6);
		sr_write(0x02, 0x04);                         /* plane 2 write enable (unused) */
		sr_write(0x04, 0x06);                         /* planar, odd/even off */
		outp((WORD)(m2010_io + 0x4E), 4); outp((WORD)(m2010_io + 0x4F), 2);     /* read map: plane 2 */
		outp((WORD)(m2010_io + 0x4E), 5); outp((WORD)(m2010_io + 0x4F), 0);     /* read mode 0 */
		outp((WORD)(m2010_io + 0x4E), 6); outp((WORD)(m2010_io + 0x4F), 0x05);  /* A0000 64K, graphics */
		for(i = 0; i < 16; i++) g[i] = vga_planemem[0x41 * 32 + i];             /* glyph 'A', 8x16 */
		outp((WORD)(m2010_io + 0x4E), 6); outp((WORD)(m2010_io + 0x4F), gr6);
		outp((WORD)(m2010_io + 0x4E), 5); outp((WORD)(m2010_io + 0x4F), gr5);
		outp((WORD)(m2010_io + 0x4E), 4); outp((WORD)(m2010_io + 0x4F), gr4);
		sr_write(0x04, sr4); sr_write(0x02, sr2);
		dbg_printf("FONT[%s] 'A': %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\n", tag,
			g[0], g[1], g[2], g[3], g[4], g[5], g[6], g[7], g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
	}
	if(vga_textmem)
	{
		/* the row the text cursor is on (rows 0/23/24 alone miss what DOS printed) */
		DWORD start = ((DWORD)cr_read(0x0C) << 8) | cr_read(0x0D);
		DWORD cur   = ((DWORD)cr_read(0x0E) << 8) | cr_read(0x0F);
		DWORD row   = (cur >= start) ? (cur - start) / 80 : 0;
		BYTE *r;
		if(row > 49) row = 0;
		r = vga_textmem + (start + row * 80) * 2;
		if((start + row * 80) * 2 + 32 <= 0x1000)
		{
			dbg_printf("TXT[%s] cursor row %ld: %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X\n", tag, row,
				r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8], r[9], r[10], r[11], r[12], r[13], r[14], r[15]);
		}
	}
}

/* ---- text-mode font keeper (full-screen DOS fix) ----------------------
 *
 * After Alt+Enter the screen is black with a blinking cursor: the switch
 * itself leaves a valid font in plane 2 (FONT[hires-disable]), but some
 * time later the plane is all zeros (FONT[at-log]). The VDD restores the DOS
 * VM's own virtual video state after our hooks, and that state has no font,
 * because a windowed VM's INT 10h mode set is eaten (VESA_check_int10h).
 *
 * The font the BIOS loaded at boot (intact at Device_Init) is kept here, and
 * for a few seconds after every switch to a VGA mode it is written back into
 * plane 2 whenever the hardware is in a text mode and the font is blank.
 * Registry: M2010FontFix = 0 turns this off.
 */
#define FONT_BYTES      8192        /* 256 glyphs x 32 bytes, font block 0 */
#define FONT_TICK_MS    100
#define FONT_TICKS      30          /* watch for 3 s after a switch */

static BYTE  font_saved[FONT_BYTES];
static BOOL  font_valid   = FALSE;
static DWORD conf_fontfix = 1;
static DWORD font_timer   = 0;
static DWORD font_ticks   = 0;
static DWORD font_fixes   = 0;

#define GR_INDEX ((WORD)(m2010_io + 0x4E))
#define GR_DATA  ((WORD)(m2010_io + 0x4F))

static BYTE gr_read(BYTE index)
{
	outp(GR_INDEX, index);
	return inp(GR_DATA);
}

static void gr_write(BYTE index, BYTE value)
{
	outp(GR_INDEX, index);
	outp(GR_DATA, value);
}

typedef struct
{
	BYTE seq_idx, gr_idx, sr2, sr4, gr1, gr3, gr4, gr5, gr6, gr8;
} planar_state_t;

/* plane 2 through the 64K window at A0000, odd/even and chain-4 off */
static void planar_enter(planar_state_t *s)
{
	s->seq_idx = inp(SEQ_INDEX);
	s->gr_idx  = inp(GR_INDEX);
	s->sr2 = sr_read(0x02); s->sr4 = sr_read(0x04);
	s->gr1 = gr_read(0x01); s->gr3 = gr_read(0x03); s->gr4 = gr_read(0x04);
	s->gr5 = gr_read(0x05); s->gr6 = gr_read(0x06); s->gr8 = gr_read(0x08);
	sr_write(0x02, 0x04);   /* write plane 2 */
	sr_write(0x04, 0x06);   /* extended memory, odd/even off, chain-4 off */
	gr_write(0x01, 0x00);   /* no set/reset */
	gr_write(0x03, 0x00);   /* no rotate, replace */
	gr_write(0x04, 0x02);   /* read plane 2 */
	gr_write(0x05, 0x00);   /* write mode 0, read mode 0, no odd/even */
	gr_write(0x06, 0x05);   /* A0000 64K, graphics addressing */
	gr_write(0x08, 0xFF);   /* all bits */
}

static void planar_leave(planar_state_t *s)
{
	gr_write(0x08, s->gr8); gr_write(0x06, s->gr6); gr_write(0x05, s->gr5);
	gr_write(0x04, s->gr4); gr_write(0x03, s->gr3); gr_write(0x01, s->gr1);
	sr_write(0x04, s->sr4); sr_write(0x02, s->sr2);
	outp(GR_INDEX, s->gr_idx);
	outp(SEQ_INDEX, s->seq_idx);
}

static BOOL font_map()
{
	if(vga_planemem == NULL)
	{
		vga_planemem = (BYTE*)_MapPhysToLinear(0xA0000, 0x10000, 0);
		if((DWORD)vga_planemem == 0xFFFFFFFFUL) vga_planemem = NULL;
	}
	return vga_planemem != NULL;
}

static BOOL glyph_blank(const BYTE *glyph)
{
	int i;
	for(i = 0; i < 16; i++)
		if(glyph[i] != 0) return FALSE;
	return TRUE;
}

/* boot: plane 2 still holds the BIOS font */
static void font_capture()
{
	planar_state_t s;
	if(!font_map()) return;
	planar_enter(&s);
	memcpy(font_saved, vga_planemem, FONT_BYTES);
	planar_leave(&s);
	font_valid = !glyph_blank(font_saved + 'A' * 32) && !glyph_blank(font_saved + '0' * 32);
	dbg_printf("M2010: boot font %s ('A' %02X %02X %02X %02X)\n", font_valid ? "saved" : "NOT usable",
		font_saved['A' * 32 + 4], font_saved['A' * 32 + 5], font_saved['A' * 32 + 6], font_saved['A' * 32 + 7]);
}

/* text mode on the hardware: alphanumeric GR06 and no extended depth in CR A3 */
static BOOL hw_text_mode()
{
	return (gr_read(0x06) & 0x01) == 0 && (cr_read(0xA3) & 0x0F) == 0;
}

static BOOL font_check_restore(const char *why)
{
	planar_state_t s;
	BOOL blank;
	if(!font_valid || !conf_fontfix || !font_map()) return FALSE;
	cr_unlock();
	if(!hw_text_mode()) return FALSE;
	planar_enter(&s);
	blank = glyph_blank(vga_planemem + 'A' * 32) && glyph_blank(vga_planemem + '0' * 32) &&
	        glyph_blank(vga_planemem + 'M' * 32);
	if(blank)
	{
		memcpy(vga_planemem, font_saved, FONT_BYTES);
	}
	planar_leave(&s);
	if(blank)
	{
		font_fixes++;
		dbg_printf("M2010: font was blank (%s), restored (#%ld)\n", why, font_fixes);
	}
	return blank;
}

void __stdcall font_timer_tick()
{
	extern BOOL mode_changing;
	font_timer = 0;
	if(!mode_changing)
		font_check_restore("timer");
	if(font_ticks > 0)
	{
		font_ticks--;
		M2010_font_timer_arm();
	}
}

static void __declspec(naked) font_timer_cb()
{
	_asm pushad
	_asm cld
	_asm call font_timer_tick
	_asm popad
	_asm ret
}

void M2010_font_timer_arm()
{
	DWORD cb = (DWORD)font_timer_cb;
	DWORD handle = 0;
	if(font_timer != 0) return;
	_asm push esi
	_asm mov eax, FONT_TICK_MS
	_asm xor edx, edx
	_asm mov esi, [cb]
	VMMCall(Set_Global_Time_Out);
	_asm mov [handle], esi
	_asm pop esi
	font_timer = handle;
}

/* called on every switch to a VGA (DOS) mode */
void M2010_font_watch(const char *why)
{
	if(!m2010_present || !font_valid || !conf_fontfix) return;
	font_check_restore(why);
	font_ticks = FONT_TICKS;
	M2010_font_timer_arm();
}

/* ---- shutdown tracer ------------------------------------------------------
 *
 * Windows stalls at "Windows is shutting down" while the RDC R6040 Ethernet
 * driver is loaded. To see how far shutdown gets, this VxD (always loaded)
 * records, from the first shutdown sign on, every VxD control message it
 * receives and a 250 ms heartbeat, each with the R6040's MCR0/MCR1/MIER
 * (plain read-only registers: RDC's shutdown handler zeroes MCR0 and MIER,
 * a MAC reset shows in MCR1). The record lives in 4 KB of VRAM, which is
 * system RAM on the M2010 (UMA) and survives a reset; the next boot copies
 * it into the debug log ("PREV-SHUTDOWN"). Registry: M2010ShutTrace = 0 off.
 */
#define TRACE_RESERVE   0x1000UL
#define TRACE_MAGIC     0x52545344UL   /* 'DSTR' */
#define TRACE_TEXT      64
#define TRACE_HB_MS     250

typedef struct
{
	DWORD magic;
	DWORD len;        /* text bytes used */
	DWORD hb_count;   /* heartbeats so far */
	DWORD hb_last;    /* ms since arming at the last heartbeat */
	DWORD hb_regs;    /* MCR0 << 16 | MIER at the last heartbeat */
	DWORD last_ev;    /* last control message */
	DWORD t0;         /* system time when armed */
} trace_hdr_t;

/* Release defaults: tracer off, exit report off, USB quiesce off. Debug
 * (DBGMEM) builds keep the tracer on; everything is switchable in the registry. */
#ifdef DBGMEM
static DWORD conf_shuttrace = 1;       /* M2010ShutTrace */
#else
static DWORD conf_shuttrace = 0;
#endif
static DWORD conf_exitreport = 0;      /* M2010ExitReport: 1 = 8 s text report at each exit stage */
static DWORD conf_exitvga    = 1;      /* M2010ExitVga: font + palette for DOS at Sys_Critical_Exit */
static DWORD conf_usbquiesce;          /* defined with the USB exit code below */
static void  usb_scan();
static BYTE *trace_mem      = NULL;
static BOOL  trace_armed    = FALSE;
static DWORD trace_timer    = 0;
static WORD  r6040_io       = 0;
static DWORD trace_lastregs = 0xFFFFFFFFUL;

static DWORD sys_time()
{
	DWORD t = 0;
	_asm push eax
	VMMCall(Get_System_Time);
	_asm mov [t], eax
	_asm pop eax
	return t;
}

#ifdef DBGMEM
/* cursor call trace (debug builds): every SetCursor/hide/show reaching the
 * VxD, with ms and the framebuffer accesses since the previous entry, for the
 * pointer blinking in windowed DirectDraw. The first 400 entries only. */
static DWORD curtr_n = 0, curtr_acc = 0, curtr_last = 0;
void M2010_trace_access()
{
	curtr_acc++;
}
void M2010_trace_cursor(const char *what, DWORD a, DWORD b)
{
	DWORD t = sys_time();
	if(curtr_n >= 400) return;
	curtr_n++;
	dbg_printf("CUR %ld +%ldms acc=%ld %s %lX %lX vis=%d hw=%d\n", t, t - curtr_last,
		curtr_acc, what, a, b, (int)m2010_cur_visible, (int)m2010_cur_loaded);
	curtr_last = t;
	curtr_acc = 0;
}
#endif

static void tr_ch(char c)
{
	trace_hdr_t *h = (trace_hdr_t*)trace_mem;
	if(h->len < TRACE_RESERVE - TRACE_TEXT - 1)
	{
		trace_mem[TRACE_TEXT + h->len] = c;
		h->len++;
	}
}

static void tr_str(const char *s)
{
	while(*s) tr_ch(*s++);
}

static void tr_hex(DWORD v, int digits)
{
	while(digits-- > 0)
	{
		BYTE n = (BYTE)((v >> (digits * 4)) & 0xF);
		tr_ch((char)(n < 10 ? '0' + n : 'A' + n - 10));
	}
}

static void tr_dec(DWORD v)
{
	char buf[12];
	int n = 0;
	do { buf[n++] = (char)('0' + v % 10); v /= 10; } while(v);
	while(n < 6) buf[n++] = ' ';
	while(n) tr_ch(buf[--n]);
}

static DWORD r6040_regs(WORD *mcr1)
{
	WORD mcr0 = 0xFFFF, mier = 0xFFFF;
	*mcr1 = 0xFFFF;
	if(r6040_io)
	{
		mcr0  = inpw(r6040_io + 0x00);
		*mcr1 = inpw(r6040_io + 0x04);
		mier  = inpw(r6040_io + 0x40);
	}
	return ((DWORD)mcr0 << 16) | mier;
}

static void tr_line(const char *what, DWORD arg)
{
	trace_hdr_t *h = (trace_hdr_t*)trace_mem;
	WORD mcr1;
	DWORD regs = r6040_regs(&mcr1);
	tr_ch('+'); tr_dec(sys_time() - h->t0); tr_str(" ms ");
	tr_str(what);
	if(arg != 0xFFFFFFFFUL) { tr_ch(' '); tr_hex(arg, 2); }
	tr_str(" MCR0="); tr_hex(regs >> 16, 4);
	tr_str(" MCR1="); tr_hex(mcr1, 4);
	tr_str(" MIER="); tr_hex(regs & 0xFFFF, 4);
	tr_ch('\n');
	trace_lastregs = regs;
}

static const char *ctrl_name(DWORD type)
{
	switch(type)
	{
		case 0x04: return "Sys_VM_Terminate";
		case 0x05: return "System_Exit";
		case 0x06: return "Sys_Critical_Exit";
		case 0x0A: return "VM_Terminate";
		case 0x0B: return "VM_Not_Executeable";
		case 0x0C: return "Destroy_VM";
		case 0x0F: return "Set_Device_Focus";
		case 0x10: return "Begin_Message_Mode";
		case 0x11: return "End_Message_Mode";
		case 0x12: return "Reboot_Processor";
		case 0x17: return "Device_Reboot_Notify";
		case 0x18: return "Crit_Reboot_Notify";
		case 0x19: return "Close_VM_Notify";
		case 0x1A: return "Power_Event";
		case 0x1C: return "Sys_Dynamic_Device_Exit";
		case 0x22: return "PNP_New_Devnode";
		case 0x24: return "Sys_VM_Terminate2";
		case 0x25: return "System_Exit2";
		case 0x26: return "Sys_Critical_Exit2";
		case 0x27: return "VM_Terminate2";
		case 0x29: return "Destroy_VM2";
		case 0x2D: return "Device_Reboot_Notify2";
		case 0x2E: return "Crit_Reboot_Notify2";
		case 0x2F: return "Close_VM_Notify2";
		case 0x32: return "Kernel32_Shutdown";
		case 0x36: return "Sys_Power_Down";
	}
	return "ctrl";
}

/* While shutdown stalls the box can only be powered off (no reset button),
 * which loses the VRAM record. So once the card is back in a text mode the
 * last trace lines are also drawn at the bottom of the screen (white on
 * blue) on every heartbeat, to be read off the monitor. Press Esc to remove
 * the shutdown logo, or set Logo=0 in MSDOS.SYS. */
#define TRACE_SCREEN_ROWS 14
#define TRACE_STALL_MS    20000      /* no shutdown progress this long: force text mode */

/* the shutdown logo is a VGA graphics mode and covers the text screen, so
 * after TRACE_STALL_MS without a control message the tracer programs a
 * standard 80x25 text mode itself (registers only, no BIOS), reloads the
 * boot font and draws the trace. Nothing else is running by then. */
static const BYTE std_sr[5]  = {0x03, 0x00, 0x03, 0x00, 0x02};
static const BYTE std_cr[25] = {0x5F, 0x4F, 0x50, 0x82, 0x55, 0x81, 0xBF, 0x1F, 0x00, 0x4F, 0x0D, 0x0E,
                                0x00, 0x00, 0x00, 0x00, 0x9C, 0x8E, 0x8F, 0x28, 0x1F, 0x96, 0xB9, 0xA3, 0xFF};
static const BYTE std_gr[9]  = {0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x0E, 0x00, 0xFF};
static const BYTE std_ar[21] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07, 0x38, 0x39, 0x3A, 0x3B,
                                0x3C, 0x3D, 0x3E, 0x3F, 0x0C, 0x00, 0x0F, 0x08, 0x00};
/* extended timing/depth registers as the BIOS left them in text mode at boot */
static const BYTE txt_cr_idx[6] = {0xA3, 0xA4, 0xAC, 0xAD, 0xAE, 0xAF};
static BYTE txt_cr_val[6];
static BYTE txt_sr_val[16];          /* SR30..SR3F */
static BOOL txt_saved   = FALSE;
static BOOL trace_forced = FALSE;
static BOOL exit_reported = FALSE;   /* exit report is on screen: no more redraws */
static BOOL scr_dirty     = FALSE;   /* we drew on the text screen (trace or report) */
static BOOL exit_clean    = FALSE;   /* exit_vga_cleanup ran: hands off the screen */
static DWORD trace_last_ev_ms = 0;

static void txt_state_save()
{
	int i;
	cr_unlock();
	if(!hw_text_mode()) return;
	for(i = 0; i < 6; i++)  txt_cr_val[i] = cr_read(txt_cr_idx[i]);
	for(i = 0; i < 16; i++) txt_sr_val[i] = sr_read((BYTE)(0x30 + i));
	txt_saved = TRUE;
	dbg_printf("M2010: text ext regs CR A3=%X A4=%X AC=%X AD=%X AE=%X AF=%X SR30=%X SR33=%X SR34=%X SR35=%X\n",
		txt_cr_val[0], txt_cr_val[1], txt_cr_val[2], txt_cr_val[3], txt_cr_val[4], txt_cr_val[5],
		txt_sr_val[0], txt_sr_val[3], txt_sr_val[4], txt_sr_val[5]);
}

static void force_text_mode()
{
	planar_state_t s;
	int i;

	M2010_engine_disable();
	M2010_cursor_hide();
	cr_unlock();
	M2010_vga_state_restore();
	if(txt_saved)
	{
		for(i = 0; i < 6; i++)  cr_write(txt_cr_idx[i], txt_cr_val[i]);
		for(i = 0; i < 16; i++) sr_write((BYTE)(0x30 + i), txt_sr_val[i]);
	}
	else
	{
		cr_write(0xA3, 0x00);
		cr_write(0xA4, 0x00);
	}

	sr_write(0x00, 0x01);                                    /* sequencer reset */
	outp((WORD)(m2010_io + 0x42), 0x67);                     /* MISC */
	for(i = 1; i < 5; i++) sr_write((BYTE)i, std_sr[i]);
	sr_write(0x00, 0x03);

	cr_write(0x11, (BYTE)(cr_read(0x11) & 0x7F));            /* unlock CR0-7 */
	for(i = 0; i < 25; i++) cr_write((BYTE)i, std_cr[i]);
	for(i = 0; i < 9; i++)  gr_write((BYTE)i, std_gr[i]);

	inp((WORD)(m2010_io + 0x5A));                            /* attribute flip-flop */
	for(i = 0; i < 21; i++)
	{
		outp((WORD)(m2010_io + 0x40), (BYTE)i);
		outp((WORD)(m2010_io + 0x40), std_ar[i]);
	}
	inp((WORD)(m2010_io + 0x5A));
	outp((WORD)(m2010_io + 0x40), 0x20);                     /* video on */

	outp((WORD)(m2010_io + 0x48), 0);                        /* EGA palette in DAC 0-63 */
	for(i = 0; i < 64; i++)
	{
		outp((WORD)(m2010_io + 0x49), (BYTE)(((i >> 2) & 1) * 0x2A + ((i >> 5) & 1) * 0x15));
		outp((WORD)(m2010_io + 0x49), (BYTE)(((i >> 1) & 1) * 0x2A + ((i >> 4) & 1) * 0x15));
		outp((WORD)(m2010_io + 0x49), (BYTE)(( i       & 1) * 0x2A + ((i >> 3) & 1) * 0x15));
	}

	if(font_valid && font_map())
	{
		planar_enter(&s);
		memcpy(vga_planemem, font_saved, FONT_BYTES);
		planar_leave(&s);
	}
	if(vga_textmem)
	{
		for(i = 0; i < 80 * 25; i++) { vga_textmem[i*2] = ' '; vga_textmem[i*2 + 1] = 0x07; }
	}
}

static void trace_screen()
{
	trace_hdr_t *h = (trace_hdr_t*)trace_mem;
	const char *text = (const char*)trace_mem + TRACE_TEXT;
	DWORD start, row, i, pos, line_start[TRACE_SCREEN_ROWS];
	int nlines = 0;
	BYTE *scr;

	if(vga_textmem == NULL || h->len == 0 || exit_reported || exit_clean) return;
	cr_unlock();
	if(!hw_text_mode()) return;
	if((cr_read(0x01) + 1) != 80) return;               /* 80-column modes only */

	/* last TRACE_SCREEN_ROWS complete lines, newest last */
	pos = h->len;
	while(pos > 0 && nlines < TRACE_SCREEN_ROWS)
	{
		DWORD e = pos - 1;                                /* the '\n' ending this line */
		DWORD b = e;
		while(b > 0 && text[b - 1] != '\n') b--;
		line_start[TRACE_SCREEN_ROWS - 1 - nlines] = b;
		nlines++;
		pos = b;
	}

	start = ((DWORD)cr_read(0x0C) << 8) | cr_read(0x0D);
	if((start + 25 * 80) * 2 > 0x1000) return;
	scr = vga_textmem + start * 2;
	scr_dirty = TRUE;

	/* title row, then the lines */
	row = 25 - TRACE_SCREEN_ROWS - 1;
	{
		static const char title[] = " M2010 shutdown trace - heartbeat ";
		DWORD c = 0, hb = h->hb_count;
		char num[12];
		int n = 0;
		for(i = 0; title[i]; i++, c++) { scr[(row*80 + c)*2] = title[i]; scr[(row*80 + c)*2 + 1] = 0x4F; }
		do { num[n++] = (char)('0' + hb % 10); hb /= 10; } while(hb);
		while(n) { scr[(row*80 + c)*2] = num[--n]; scr[(row*80 + c)*2 + 1] = 0x4F; c++; }
		for(; c < 80; c++) { scr[(row*80 + c)*2] = ' '; scr[(row*80 + c)*2 + 1] = 0x4F; }
	}
	for(i = 0; i < TRACE_SCREEN_ROWS; i++)
	{
		DWORD c = 0;
		row = 25 - TRACE_SCREEN_ROWS + i;
		if(i >= (DWORD)(TRACE_SCREEN_ROWS - nlines))
		{
			DWORD p = line_start[i];
			while(p < h->len && text[p] != '\n' && c < 80)
			{
				scr[(row*80 + c)*2] = text[p++];
				scr[(row*80 + c)*2 + 1] = 0x1F;
				c++;
			}
		}
		for(; c < 80; c++) { scr[(row*80 + c)*2] = ' '; scr[(row*80 + c)*2 + 1] = 0x1F; }
	}
}

/* vmware/pci.c's 8/16-bit readers always read the dword's low bytes; these
   extract the right field from a 32-bit read */
static WORD cfg16(PCIAddress *a, WORD off)
{
	return (WORD)(PCI_ConfigRead32(a, (WORD)(off & 0xFC)) >> ((off & 2) * 8));
}

static BYTE cfg8(PCIAddress *a, WORD off)
{
	return (BYTE)(PCI_ConfigRead32(a, (WORD)(off & 0xFC)) >> ((off & 3) * 8));
}

/* ---- exit report ----------------------------------------------------------
 * Restart / "Restart in MS-DOS mode" freeze right after Windows has left
 * (keyboard dead, cursor blinking): likely a device still signalling an
 * interrupt that real-mode DOS does not service. At Sys_Critical_Exit, the
 * last VxD message before real mode, the screen is put in text mode and the
 * interrupt state is printed; if the machine then freezes, it stays visible.
 * Nothing is acknowledged or cleared here (R6040 MISR is read-to-clear and
 * is deliberately not read), so the report does not change the outcome. */
static void scr_put(DWORD row, DWORD col, const char *s, BYTE attr)
{
	while(*s && col < 80)
	{
		vga_textmem[(row*80 + col)*2]     = *s++;
		vga_textmem[(row*80 + col)*2 + 1] = attr;
		col++;
	}
}

static char *hex_to(char *p, DWORD v, int digits)
{
	while(digits-- > 0)
	{
		BYTE n = (BYTE)((v >> (digits * 4)) & 0xF);
		*p++ = (char)(n < 10 ? '0' + n : 'A' + n - 10);
	}
	return p;
}

static char *str_to(char *p, const char *s)
{
	while(*s) *p++ = *s++;
	return p;
}

/* ~8 s, timed by the port 61h refresh toggle (15 us); capped in case it never toggles */
static void exit_hold()
{
	DWORD toggles = 0, spins = 0;
	BYTE last = (BYTE)(inp(0x61) & 0x10);
	while(toggles < 530000UL && spins < 40000000UL)
	{
		BYTE now = (BYTE)(inp(0x61) & 0x10);
		if(now != last) { toggles++; last = now; }
		spins++;
	}
}

static DWORD exit_stages = 0;   /* bit per stage already shown */

static void exit_report(DWORD stage_bit, const char *stage)
{
	char line[160], *p;
	BYTE irr1, isr1, imr1, irr2, isr2, imr2, elcr1, elcr2;
	DWORD row = 3, dev, fn;

	if((exit_stages & stage_bit) || vga_textmem == NULL) return;
	exit_stages |= stage_bit;
	exit_reported = TRUE;
	scr_dirty = TRUE;
	if(trace_mem) tr_line("EXIT REPORT", 0xFFFFFFFFUL);
	force_text_mode();

	p = line;
	p = str_to(p, " M2010 EXIT REPORT at ");
	p = str_to(p, stage);
	p = str_to(p, " - shown 8 s. Freeze here? photograph it ");
	*p = '\0';
	scr_put(0, 0, line, 0x4F);

	outp(0x20, 0x0A); irr1 = inp(0x20);
	outp(0x20, 0x0B); isr1 = inp(0x20);
	outp(0x20, 0x0A); imr1 = inp(0x21);
	outp(0xA0, 0x0A); irr2 = inp(0xA0);
	outp(0xA0, 0x0B); isr2 = inp(0xA0);
	outp(0xA0, 0x0A); imr2 = inp(0xA1);
	elcr1 = inp(0x4D0); elcr2 = inp(0x4D1);
	p = line;
	p = str_to(p, "PIC1 IRR="); p = hex_to(p, irr1, 2);
	p = str_to(p, " ISR=");     p = hex_to(p, isr1, 2);
	p = str_to(p, " IMR=");     p = hex_to(p, imr1, 2);
	p = str_to(p, "  PIC2 IRR="); p = hex_to(p, irr2, 2);
	p = str_to(p, " ISR=");     p = hex_to(p, isr2, 2);
	p = str_to(p, " IMR=");     p = hex_to(p, imr2, 2);
	p = str_to(p, "  ELCR=");   p = hex_to(p, elcr1, 2); *p++ = ' '; p = hex_to(p, elcr2, 2);
	*p = '\0';
	scr_put(1, 0, line, 0x1F);

	p = line;
	p = str_to(p, "R6040 I/O="); p = hex_to(p, r6040_io, 4);
	if(r6040_io)
	{
		p = str_to(p, " MCR0="); p = hex_to(p, inpw(r6040_io + 0x00), 4);
		p = str_to(p, " MCR1="); p = hex_to(p, inpw(r6040_io + 0x04), 4);
		p = str_to(p, " MIER="); p = hex_to(p, inpw(r6040_io + 0x40), 4);
	}
	p = str_to(p, "  INTx=1: signalling");
	*p = '\0';
	scr_put(2, 0, line, 0x1F);

	for(dev = 0; dev < 32 && row < 19; dev++)
	{
		for(fn = 0; fn < 8 && row < 19; fn++)
		{
			PCIAddress a;
			WORD ven, did, cmd, stat;
			BYTE line_irq, pin, hdr;
			a.bus = 0; a.device = (BYTE)dev; a.function = (BYTE)fn; a.padding = 0;
			ven = cfg16(&a, 0x00);
			if(ven == 0xFFFF || ven == 0x0000)
			{
				if(fn == 0) break;
				continue;
			}
			hdr = cfg8(&a, 0x0E);
			did  = cfg16(&a, 0x02);
			cmd  = cfg16(&a, 0x04);
			stat = cfg16(&a, 0x06);
			line_irq = cfg8(&a, 0x3C);
			pin  = cfg8(&a, 0x3D);
			if(pin != 0)
			{
				p = line;
				p = str_to(p, "00:"); p = hex_to(p, dev, 2); *p++ = '.'; p = hex_to(p, fn, 1);
				*p++ = ' '; p = hex_to(p, ven, 4); *p++ = ':'; p = hex_to(p, did, 4);
				p = str_to(p, " IRQ="); p = hex_to(p, line_irq, 2);
				p = str_to(p, " pin="); p = hex_to(p, pin, 1);
				p = str_to(p, " CMD="); p = hex_to(p, cmd, 4);
				p = str_to(p, " STAT="); p = hex_to(p, stat, 4);
				p = str_to(p, (stat & 0x0008) ? "  INTx=1 <<<" : "  INTx=0");
				*p = '\0';
				scr_put(row++, 0, line, (stat & 0x0008) ? 0x4E : 0x1F);
			}
			if(fn == 0 && !(hdr & 0x80)) break;   /* single-function device */
		}
	}
	scr_put(24, 0, " (INTx needs PCI 2.3; devices without it always show 0) ", 0x17);
	exit_hold();
}

void __stdcall trace_timer_tick()
{
	trace_hdr_t *h = (trace_hdr_t*)trace_mem;
	WORD mcr1;
	DWORD regs;
	trace_timer = 0;
	if(!trace_armed || trace_mem == NULL) return;
	regs = r6040_regs(&mcr1);
	h->hb_count++;
	h->hb_last = sys_time() - h->t0;
	h->hb_regs = regs;
	if(regs != trace_lastregs || (h->hb_count % 20) == 0)
		tr_line("heartbeat", h->hb_count);
	if(!trace_forced && h->hb_last - trace_last_ev_ms > TRACE_STALL_MS)
	{
		trace_forced = TRUE;
		tr_line("STALL: no progress for 20 s, forcing text mode", 0xFFFFFFFFUL);
		force_text_mode();
	}
	trace_screen();
	M2010_trace_timer_arm();
}

static void __declspec(naked) trace_timer_cb()
{
	_asm pushad
	_asm cld
	_asm call trace_timer_tick
	_asm popad
	_asm ret
}

void M2010_trace_timer_arm()
{
	DWORD cb = (DWORD)trace_timer_cb;
	DWORD handle = 0;
	if(trace_timer != 0) return;
	_asm push esi
	_asm mov eax, TRACE_HB_MS
	_asm xor edx, edx
	_asm mov esi, [cb]
	VMMCall(Set_Global_Time_Out);
	_asm mov [handle], esi
	_asm pop esi
	trace_timer = handle;
}

/* boot: reserve the record below the cursor pattern, report the previous one */
static void trace_init()
{
	PCIAddress nic;
	trace_hdr_t *h;
	RegReadConf(HKEY_LOCAL_MACHINE, M2010_conf_path, "M2010ShutTrace",  &conf_shuttrace);
	RegReadConf(HKEY_LOCAL_MACHINE, M2010_conf_path, "M2010ExitReport", &conf_exitreport);
	RegReadConf(HKEY_LOCAL_MACHINE, M2010_conf_path, "M2010ExitVga",    &conf_exitvga);
	RegReadConf(HKEY_LOCAL_MACHINE, M2010_conf_path, "M2010UsbQuiesce", &conf_usbquiesce);
	dbg_printf("M2010: exit options ShutTrace=%ld ExitReport=%ld ExitVga=%ld UsbQuiesce=%ld\n",
		conf_shuttrace, conf_exitreport, conf_exitvga, conf_usbquiesce);

	/* text-mode registers for force_text_mode (exit report, stall screen) */
	txt_state_save();
	if(conf_usbquiesce) usb_scan();

	if(PCI_FindDevice(0x17F3, 0x6040, &nic))
	{
		DWORD bar0 = PCI_ConfigRead32(&nic, 0x10);
		if(bar0 & 1) r6040_io = (WORD)(bar0 & 0xFFFC);
	}
	/* the exit report draws through B8000 */
	if(conf_exitreport && vga_textmem == NULL)
	{
		vga_textmem = (BYTE*)_MapPhysToLinear(0xB8000, 0x1000, 0);
		if((DWORD)vga_textmem == 0xFFFFFFFFUL) vga_textmem = NULL;
	}

	if(!conf_shuttrace || hda->vram_size <= 2UL*1024*1024) return;

	hda->vram_size -= TRACE_RESERVE;
	trace_mem = (BYTE*)hda->vram_pm32 + hda->vram_size;
	h = (trace_hdr_t*)trace_mem;
	dbg_printf("M2010: shutdown trace at VRAM %lX, R6040 I/O %X\n", hda->vram_size, r6040_io);

	if(h->magic == TRACE_MAGIC && h->len < TRACE_RESERVE - TRACE_TEXT)
	{
		DWORD i, start = 0;
		dbg_printf("PREV-SHUTDOWN: %ld heartbeats, last at +%ld ms (MCR0=%lX MIER=%lX), last ctrl %lX\n",
			h->hb_count, h->hb_last, h->hb_regs >> 16, h->hb_regs & 0xFFFF, h->last_ev);
		/* line by line: dbg_printf has no length-limited %s */
		for(i = 0; i < h->len; i++)
		{
			if(trace_mem[TRACE_TEXT + i] == '\n')
			{
				trace_mem[TRACE_TEXT + i] = '\0';
				dbg_printf("PREV-SHUTDOWN %s\n", (char*)trace_mem + TRACE_TEXT + start);
				start = i + 1;
			}
		}
	}
	else
	{
		dbg_printf("PREV-SHUTDOWN: no record (first run, record cleared, or RAM lost on power-off)\n");
	}
	h->magic = 0;
}

/* first sign of shutdown: start a fresh record and the heartbeat */
void M2010_trace_arm(const char *why)
{
	trace_hdr_t *h = (trace_hdr_t*)trace_mem;
	if(trace_mem == NULL || trace_armed) return;
	h->len = 0;
	h->hb_count = 0;
	h->hb_last = 0;
	h->hb_regs = 0;
	h->last_ev = 0;
	h->t0 = sys_time();
	h->magic = TRACE_MAGIC;
	trace_armed = TRUE;
	trace_forced = FALSE;
	trace_last_ev_ms = 0;
	tr_line("ARMED by", 0xFFFFFFFFUL);
	tr_str("  "); tr_str(why); tr_ch('\n');
	M2010_trace_timer_arm();
}

void M2010_trace_event(const char *what)
{
	if(!trace_armed) return;
	trace_last_ev_ms = sys_time() - ((trace_hdr_t*)trace_mem)->t0;
	tr_line(what, 0xFFFFFFFFUL);
}

/* ---- USB controllers at exit ---------------------------------------------
 * With the USB host controllers disabled, "Restart in MS-DOS mode" works;
 * with them enabled Windows freezes on its way to real mode. The Win98 USB
 * stack (NUSB EHCI driver) does not stop its controllers, so they keep
 * running and signalling interrupts that DOS cannot service. At
 * Sys_Critical_Exit every USB host controller is quietened here: interrupts
 * off, schedules stopped, pending status acknowledged. The BIOS re-initialises
 * them on the next boot. It did not cure the NUSB EHCI restart freeze (NUSB
 * hangs in its own exit code first), so it is off by default since build 27.
 * Registry: M2010UsbQuiesce = 1 turns it on. */
static DWORD conf_usbquiesce = 0;

typedef struct
{
	PCIAddress     a;
	BYTE           progif;          /* 0x20 EHCI, 0x10 OHCI, 0x00 UHCI */
	volatile BYTE *m;               /* EHCI/OHCI registers, mapped at boot */
	WORD           io;              /* UHCI I/O base */
} usb_hc_t;

static usb_hc_t usb_hcs[8];
static int      usb_count = 0;

/* boot: find the USB host controllers and map their registers now, while
   the memory manager is fully available */
static void usb_scan()
{
	DWORD dev, fn;
	for(dev = 0; dev < 32; dev++)
	{
		for(fn = 0; fn < 8; fn++)
		{
			PCIAddress a;
			DWORD cls, bar;
			WORD ven;
			BYTE hdr, progif;
			a.bus = 0; a.device = (BYTE)dev; a.function = (BYTE)fn; a.padding = 0;
			ven = cfg16(&a, 0x00);
			if(ven == 0xFFFF || ven == 0x0000)
			{
				if(fn == 0) break;
				continue;
			}
			hdr = cfg8(&a, 0x0E);
			cls = PCI_ConfigRead32(&a, 0x08);
			progif = (BYTE)(cls >> 8);
			if((cls >> 16) == 0x0C03 && usb_count < 8)
			{
				usb_hc_t *h = &usb_hcs[usb_count];
				h->a = a; h->progif = progif; h->m = NULL; h->io = 0;
				if(progif == 0x20 || progif == 0x10)
				{
					bar = PCI_ConfigRead32(&a, 0x10) & 0xFFFFFF00UL;
					if(bar != 0)
					{
						h->m = (volatile BYTE*)_MapPhysToLinear(bar, 0x1000, 0);
						if((DWORD)h->m == 0xFFFFFFFFUL) h->m = NULL;
					}
				}
				else if(progif == 0x00)
				{
					bar = PCI_ConfigRead32(&a, 0x20);
					if(bar & 1) h->io = (WORD)(bar & 0xFFE0);
				}
				dbg_printf("M2010: USB host 00:%02X.%d prog-if %02X regs %lX io %X\n",
					dev, fn, progif, (DWORD)h->m, h->io);
				usb_count++;
			}
			if(fn == 0 && !(hdr & 0x80)) break;
		}
	}
}

/* exit: interrupts off, schedules stopped, pending status acknowledged */
static int usb_quiesce(DWORD row)
{
	int i, n = 0;
	char line[160], *p;

	for(i = 0; i < usb_count; i++)
	{
		usb_hc_t *h = &usb_hcs[i];
		WORD cmd = cfg16(&h->a, 0x04);
		const char *kind = "skipped (decoding off)";

		if(h->progif == 0x20 && h->m && (cmd & 0x0002))
		{
			volatile DWORD *op = (volatile DWORD*)(h->m + h->m[0]);   /* CAPLENGTH */
			DWORD spin;
			op[2] = 0;                                                  /* USBINTR */
			op[0] &= ~1UL;                                              /* USBCMD.RS */
			for(spin = 0; spin < 100000UL && !(op[1] & 0x1000UL); spin++)
				inp(0x80);                                              /* wait HCHalted */
			op[1] = 0x3F;                                               /* ack USBSTS */
			op[16] = 0;                                                 /* CONFIGFLAG: ports to companions */
			if(op[1] & 0x1000UL)                                        /* halted: HCRESET is allowed */
			{
				op[0] |= 2UL;
				for(spin = 0; spin < 100000UL && (op[0] & 2UL); spin++)
					inp(0x80);
			}
			{
				/* USBLEGSUP (EECP in HCCPARAMS): release the OS-owned semaphore so the
				   BIOS can run its USB keyboard emulation again in DOS / during reboot */
				BYTE eecp = (BYTE)(*(volatile DWORD*)(h->m + 8) >> 8);
				if(eecp >= 0x40)
				{
					DWORD leg = PCI_ConfigRead32(&h->a, eecp);
					if((leg & 0xFF) == 1 && (leg & 0x01000000UL))
						PCI_ConfigWrite32(&h->a, eecp, leg & ~0x01000000UL);
				}
			}
			kind = "EHCI stopped, reset, back to BIOS";
			n++;
		}
		else if(h->progif == 0x10 && h->m && (cmd & 0x0002))
		{
			volatile DWORD *r = (volatile DWORD*)h->m;
			r[5] = 0xC000007FUL;                                        /* HcInterruptDisable: MIE + all */
			r[3] = 0x0000007FUL;                                        /* ack HcInterruptStatus */
			r[1] &= ~0x000000C0UL;                                      /* HcControl.HCFS = UsbReset */
			kind = "OHCI stopped, reset";
			n++;
		}
		else if(h->progif == 0x00 && h->io && (cmd & 0x0001))
		{
			outp((WORD)(h->io + 4), 0); outp((WORD)(h->io + 5), 0);     /* USBINTR */
			outp((WORD)(h->io + 0), 0); outp((WORD)(h->io + 1), 0);     /* USBCMD: stop */
			outp((WORD)(h->io + 2), 0x3F);                               /* ack USBSTS */
			kind = "UHCI stopped";
			n++;
		}
		p = line;
		p = str_to(p, "USB 00:"); p = hex_to(p, h->a.device, 2); *p++ = '.'; p = hex_to(p, h->a.function, 1);
		*p++ = ' '; p = str_to(p, kind);
		p = str_to(p, "  STAT now="); p = hex_to(p, cfg16(&h->a, 0x06), 4);
		*p = '\0';
		if(exit_reported && vga_textmem && row < 24) scr_put(row++, 0, line, 0x2F);
		if(trace_mem) tr_line(line, 0xFFFFFFFFUL);
	}
	return n;
}

/* ---- what DOS inherits at Sys_Critical_Exit -------------------------------
 * After "Restart in MS-DOS mode" DOS came up red, and the final "It's now safe
 * to turn off" screen came up black. Two causes, both handled here, the last
 * moment our code runs before real mode, and only when the card is already in
 * a text mode (the shutdown logo, a graphics mode, is left alone):
 *  - the exit report's red title row (attribute 4F) was still in B8000: DOS
 *    scrolls with the attribute under the cursor, so the whole screen turned
 *    red. A screen we drew on (report, trace lines) is cleared to grey on
 *    black, cursor home.
 *  - the standard text palette (AR 00-0F, DAC 00-3F) and the boot font are
 *    put back, so text is visible even when nothing reloads them.
 * Registry: M2010ExitVga = 0 turns this off. */
static void exit_vga_cleanup()
{
	planar_state_t s;
	int i;

	if(!conf_exitvga || !m2010_present) return;
	cr_unlock();
	if(!hw_text_mode())
	{
		dbg_printf("M2010: exit: not in text mode, VGA left as is\n");
		if(trace_mem) tr_line("exit VGA cleanup skipped (graphics mode)", 0xFFFFFFFFUL);
		return;
	}

	exit_clean = TRUE;
	if(scr_dirty && vga_textmem)
	{
		DWORD start = ((DWORD)cr_read(0x0C) << 8) | cr_read(0x0D);
		if((start + 80 * 25) * 2 <= 0x1000)
		{
			for(i = 0; i < 80 * 25; i++)
			{
				vga_textmem[(start + i) * 2]     = ' ';
				vga_textmem[(start + i) * 2 + 1] = 0x07;
			}
			cr_write(0x0E, (BYTE)(start >> 8));            /* text cursor to the top left */
			cr_write(0x0F, (BYTE)start);
		}
	}

	/* attribute controller palette 0-15 (standard mode 3 values) */
	inp((WORD)(m2010_io + 0x5A));
	for(i = 0; i < 16; i++)
	{
		outp((WORD)(m2010_io + 0x40), (BYTE)i);
		outp((WORD)(m2010_io + 0x40), std_ar[i]);
	}
	inp((WORD)(m2010_io + 0x5A));
	outp((WORD)(m2010_io + 0x40), 0x20);                     /* video on */

	/* EGA colours in DAC 0-63, 6-bit values as the BIOS loads them */
	outp((WORD)(m2010_io + 0x48), 0);
	for(i = 0; i < 64; i++)
	{
		outp((WORD)(m2010_io + 0x49), (BYTE)(((i >> 2) & 1) * 0x2A + ((i >> 5) & 1) * 0x15));
		outp((WORD)(m2010_io + 0x49), (BYTE)(((i >> 1) & 1) * 0x2A + ((i >> 4) & 1) * 0x15));
		outp((WORD)(m2010_io + 0x49), (BYTE)(( i       & 1) * 0x2A + ((i >> 3) & 1) * 0x15));
	}

	if(font_valid && font_map())
	{
		planar_enter(&s);
		memcpy(vga_planemem, font_saved, FONT_BYTES);
		planar_leave(&s);
	}
	dbg_printf("M2010: exit: text palette and boot font restored%s\n", scr_dirty ? ", our text cleared" : "");
	if(trace_mem) tr_line("exit VGA cleanup: palette + font", 0xFFFFFFFFUL);
}

/* every VxD control message (called from VXD_control) */
void __stdcall M2010_trace_ctrl(DWORD type)
{
	BOOL vm_term  = (type == 0x04 || type == 0x24);   /* Sys_VM_Terminate(2) */
	BOOL sys_exit = (type == 0x05 || type == 0x25);   /* System_Exit(2) */
	BOOL crit     = (type == 0x06 || type == 0x26);   /* Sys_Critical_Exit(2) */

	if(!m2010_present) return;

	if(trace_mem != NULL)
	{
		if(type == 0x32 && trace_armed)
		{
			/* armed earlier by something that was not a shutdown (e.g. a mode change) */
			trace_armed = FALSE;
		}
		if(!trace_armed && (type == 0x32 || vm_term || sys_exit || crit))  /* Kernel32_Shutdown or exit */
			M2010_trace_arm(ctrl_name(type));
		/* skip the chatty ones: DeviceIoControl, threads, processes */
		if(trace_armed && !(type == 0x23 || (type >= 0x1D && type <= 0x21) || type == 0x33 || type == 0x34))
		{
			((trace_hdr_t*)trace_mem)->last_ev = type;
			trace_last_ev_ms = sys_time() - ((trace_hdr_t*)trace_mem)->t0;
			tr_line(ctrl_name(type), type);
			if(!vm_term && !sys_exit && !crit) trace_screen();
		}
	}

	/* the three last stages before real mode */
	if(vm_term)
	{
		if(conf_exitreport) exit_report(1, "Sys_VM_Terminate");
	}
	else if(sys_exit)
	{
		if(conf_exitreport) exit_report(2, "System_Exit");
		if(conf_usbquiesce && !(exit_stages & 0x100))
		{
			exit_stages |= 0x100;
			usb_quiesce(19);
			if(conf_exitreport) exit_hold();
		}
	}
	else if(crit)
	{
		if(conf_exitreport) exit_report(4, "Sys_Critical_Exit");
		if(conf_usbquiesce && !(exit_stages & 0x200))
		{
			exit_stages |= 0x200;
			usb_quiesce(19);
			if(conf_exitreport) exit_hold();
		}
		if(!(exit_stages & 0x400))
		{
			exit_stages |= 0x400;
			exit_vga_cleanup();
		}
	}
}

/* going back to the desktop: no plane writes during the hires mode set */
void M2010_font_unwatch()
{
	DWORD handle = font_timer;
	font_ticks = 0;
	if(handle == 0) return;
	font_timer = 0;
	_asm push esi
	_asm mov esi, [handle]
	VMMCall(Cancel_Time_Out);
	_asm pop esi
}

/* ---- public API ----------------------------------------------------- */

BOOL M2010_init(DWORD vbe_fb_phy)
{
	DWORD bar0, bar1, bar2, pte;
	BYTE  cr_aa;

	RegReadConf(HKEY_LOCAL_MACHINE, M2010_conf_path, "M2010Accel",    &conf_accel);
	RegReadConf(HKEY_LOCAL_MACHINE, M2010_conf_path, "M2010GdiBlt",   &conf_gdiblt);
	RegReadConf(HKEY_LOCAL_MACHINE, M2010_conf_path, "M2010MinArea",  &conf_min_area);
	RegReadConf(HKEY_LOCAL_MACHINE, M2010_conf_path, "M2010Wbinvd",   &conf_wbinvd);
	RegReadConf(HKEY_LOCAL_MACHINE, M2010_conf_path, "M2010SyncCopy", &conf_sync);
	RegReadConf(HKEY_LOCAL_MACHINE, M2010_conf_path, "M2010HwCursor", &conf_cursor);
	RegReadConf(HKEY_LOCAL_MACHINE, M2010_conf_path, "M2010Refresh",  &conf_refresh);
	RegReadConf(HKEY_LOCAL_MACHINE, M2010_conf_path, "M2010FgMode",   &conf_fgmode);
	RegReadConf(HKEY_LOCAL_MACHINE, M2010_conf_path, "M2010OpLog",    &conf_oplog);
	RegReadConf(HKEY_LOCAL_MACHINE, M2010_conf_path, "M2010Async",    &conf_async);
	RegReadConf(HKEY_LOCAL_MACHINE, M2010_conf_path, "M2010FontFix",  &conf_fontfix);
	RegReadConf(HKEY_LOCAL_MACHINE, M2010_conf_path, "M2010TextFill", &conf_textfill);
	RegReadConf(HKEY_LOCAL_MACHINE, M2010_conf_path, "M2010RectFill", &conf_rectfill);
	RegReadConf(HKEY_LOCAL_MACHINE, M2010_conf_path, "M2010HwInvert", &conf_curinvert);
	RegReadConf(HKEY_LOCAL_MACHINE, M2010_conf_path, "M2010HwCurYAdj", &conf_curyadj);
	RegReadConf(HKEY_LOCAL_MACHINE, M2010_conf_path, "M2010CursorColor",  &conf_curcolor);
	RegReadConf(HKEY_LOCAL_MACHINE, M2010_conf_path, "M2010FlipSync",     &conf_flipsync);
	dbg_printf("M2010: FlipSync=%ld\n", conf_flipsync);
	dbg_printf("M2010: TextFill=%ld RectFill=%ld HwInvert=%ld CursorColor=%ld\n",
		conf_textfill, conf_rectfill, conf_curinvert, conf_curcolor);

	if(!conf_accel)
	{
		dbg_printf("M2010: disabled by registry (M2010Accel=0), plain VESA behaviour\n");
		return FALSE;
	}

	if(!PCI_FindDevice(M2010_VENDOR_ID, M2010_DEVICE_ID, &m2010_pci))
	{
		dbg_printf("M2010: PCI 17F3:2010 not found\n");
		return FALSE;
	}

	bar0 = PCI_GetBARAddr(&m2010_pci, 0);
	bar1 = PCI_GetBARAddr(&m2010_pci, 1);
	bar2 = PCI_ConfigRead32(&m2010_pci, 16 + 4*2);

	dbg_printf("M2010: found at %d:%d.%d BAR0=%lX BAR1=%lX BAR2=%lX, VBE LFB=%lX\n",
		m2010_pci.bus, m2010_pci.device, m2010_pci.function, bar0, bar1, bar2, vbe_fb_phy);

	if(bar1 == 0 || (bar2 & PCI_CONF_BAR_IO) == 0 || (bar2 & 0xFFFC) == 0)
	{
		dbg_printf("M2010: unusable BARs, no acceleration\n");
		return FALSE;
	}

	if((bar0 & 0xFFF00000UL) != (vbe_fb_phy & 0xFFF00000UL))
	{
		dbg_printf("M2010: VBE framebuffer is not BAR0, no acceleration\n");
		return FALSE;
	}

	m2010_fb_phys = bar0 & 0xFFF00000UL;
	m2010_io      = (WORD)(bar2 & 0xFFFC);

	m2010_mmio = (volatile DWORD*)_MapPhysToLinear(bar1 & 0xFFFF0000UL, M2010_MMIO_SIZE, 0);
	if(m2010_mmio == NULL || (DWORD)m2010_mmio == 0xFFFFFFFFUL)
	{
		dbg_printf("M2010: MMIO mapping failed\n");
		m2010_mmio = NULL;
		return FALSE;
	}

	cr_unlock();
	cr_aa = cr_read(0xAA);
	switch(cr_aa & 0x07)
	{
		case 0:  m2010_vram_cr =   8UL*1024*1024; break;
		case 1:  m2010_vram_cr =  16UL*1024*1024; break;
		case 2:  m2010_vram_cr =  32UL*1024*1024; break;
		case 3:  m2010_vram_cr =  64UL*1024*1024; break;
		case 4:  m2010_vram_cr = 128UL*1024*1024; break;
		default: m2010_vram_cr =   8UL*1024*1024; break;
	}

	/* diagnostics for the first real-hardware run */
	pte = 0;
	_CopyPageTable(((DWORD)hda->vram_pm32) >> 12, 1, &pte, 0);
	dbg_printf("M2010: CR AA=%X (VRAM %ld MB, VBE says %ld MB), VRAM PTE=%lX\n",
		cr_aa, m2010_vram_cr >> 20, hda->vram_size >> 20, pte);
	pte = 0;
	_CopyPageTable(((DWORD)m2010_mmio) >> 12, 1, &pte, 0);
	dbg_printf("M2010: MMIO PTE=%lX, CR A3=%X CR A4=%X, 1stflip=%lX cmdq=%lX state=%lX\n",
		pte, cr_read(0xA3), cr_read(0xA4), MMIO(REG_1ST_FLIP), MMIO(REG_CMDQ_BASE), MMIO(REG_CMDQ_READ));

	m2010_present = TRUE;
	vga_state_save();
	M2010_dump_vga("boot");
	M2010_dump_text_and_font("boot");
	font_capture();

	/* hardware cursor pattern: 16 KB taken from the top of VRAM before the
	 * VMDisp9x heap is laid out (we are called before FBHDA_update_heap_size) */
	if(conf_cursor && hda->vram_size > 2UL*1024*1024)
	{
		hda->vram_size -= CUR_RESERVE;
		m2010_cur_off = hda->vram_size;
		m2010_cur_ready = TRUE;
		dbg_printf("M2010: cursor pattern at VRAM offset %lX\n", m2010_cur_off);
	}

	/* pattern brush scratch, also taken from the top of VRAM */
	RegReadConf(HKEY_LOCAL_MACHINE, M2010_conf_path, "M2010PatFill", &conf_patfill);
	if(conf_patfill && hda->vram_size > 4UL*1024*1024)
	{
		hda->vram_size -= PAT_RESERVE;
		m2010_pat_off = hda->vram_size;
		m2010_pat_ready = TRUE;
		dbg_printf("M2010: pattern scratch at VRAM offset %lX\n", m2010_pat_off);
	}

	trace_init();

	/* option ROM shadow for the refresh-rate table */
	m2010_rom = (BYTE*)_MapPhysToLinear(ROM_PHYS, ROM_SIZE, 0);
	if(m2010_rom != NULL && (DWORD)m2010_rom != 0xFFFFFFFFUL)
	{
		if(m2010_rom[0] == 0x55 && m2010_rom[1] == 0xAA &&
			*(WORD*)(m2010_rom + 0x40) == M2010_VENDOR_ID)
		{
			DWORD i;
			for(i = 0; i < ROM_SIZE - 4 - MODE_INFO_SIZE; i++)
			{
				if(m2010_rom[i] == 'V' && m2010_rom[i+1] == 'P' &&
					m2010_rom[i+2] == 'I' && m2010_rom[i+3] == 'T')
				{
					m2010_vpit = i + 4;
					break;
				}
			}
		}
		dbg_printf("M2010: ROM %02X%02X vendor %X, VPIT at %lX\n",
			m2010_rom[0], m2010_rom[1], *(WORD*)(m2010_rom + 0x40), m2010_vpit);
		if(m2010_vpit == 0)
		{
			m2010_rom = NULL;
		}
	}
	else
	{
		m2010_rom = NULL;
	}

	return TRUE;
}

/* ---- hardware cursor ------------------------------------------------- */
/*
 * The M2010 (PCI 17F3:2010) has RDC's "HQ" cursor, driven through MMIO
 * (rdc_cursor.c *_HQ, chosen for PCI_CHIP_M2010 by RDCSetHWCaps):
 *   CTRL 0x580: bit 31 = MMIO cursor mode, bit 1 = show, bit 0 = colour
 *   OFFSET 0x584: x skip (5..0), y skip (13..8), last line (26..16)
 *   POSITION 0x588: x (11..0), y (26..16); R0.0.9 added 1 to y, T0.0.13 not
 *   ADDRESS 0x58C: pattern VRAM offset >> 3
 * Writing CTRL back latches a new position. The pattern is 64x64 ARGB8888
 * (16 KB, two buffers alternated per load) in colour mode; every shape is loaded that way (mono shapes become
 * opaque black/white pixels), so the mono format never matters. The CR C2..CD
 * cursor that builds 27-32 programmed is the older A0-stepping one.
 * The cursor cannot invert the screen: shapes with inverting pixels (I-beam)
 * stay in software unless M2010HwInvert=1 draws those pixels black.
 */
BOOL M2010_cursor_available()
{
	return m2010_present && m2010_cur_ready && conf_cursor &&
		(hda->bpp == 8 || hda->bpp == 16 || hda->bpp == 32);
}

static void cursor_program_position()
{
	int x = m2010_cur_x - m2010_cur_hotx;
	int y = m2010_cur_y - m2010_cur_hoty + (int)conf_curyadj;
	int x_off = 0, y_off = 0, y_end;

	if(x < 0) { x_off = -x; x = 0; }
	if(y < 0) { y_off = -y; y = 0; }
	if(x_off >= CUR_SIZE) x_off = CUR_SIZE - 1;
	if(y_off >= CUR_SIZE) y_off = CUR_SIZE - 1;
	y_end = y + (CUR_SIZE - y_off) - 1;

	MMIO(HWC_OFFSET)   = (DWORD)(x_off & 0x3F) | ((DWORD)(y_off & 0x3F) << 8) |
	                     ((DWORD)(y_end & 0x7FF) << 16);
	MMIO(HWC_POSITION) = (DWORD)(x & 0xFFF) | ((DWORD)(y & 0x7FF) << 16);
	MMIO(HWC_CTRL)     = MMIO(HWC_CTRL);        /* latch */
}

static void cursor_program_show(BOOL show)
{
	DWORD ctrl = MMIO(HWC_CTRL) | HWC_CTRL_MMIO | HWC_CTRL_COLOR;
	MMIO(HWC_CTRL) = show ? (ctrl | HWC_CTRL_SHOW) : (ctrl & ~HWC_CTRL_SHOW);
}

/* one colour-cursor XOR pixel (screen format) as RGB888 */
static DWORD cur_rgb888(const BYTE *xor_bits, int x, int y, int w, int bpp)
{
	if(bpp == 16)
	{
		WORD p = ((const WORD*)xor_bits)[y * w + x];
		DWORD r = (p >> 11) & 0x1F, g = (p >> 5) & 0x3F, b = p & 0x1F;
		return ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
	}
	return ((const DWORD*)xor_bits)[y * w + x] & 0xFFFFFFUL;
}

/*
 * Windows cursor: AND=1/XOR=0 transparent, AND=0 opaque XOR colour (mono:
 * 0 black, 1 white), AND=1/XOR!=0 inverts the screen.
 */
BOOL M2010_cursor_load(int w, int h, int hotx, int hoty, int bpp,
	const BYTE *and_bits, const BYTE *xor_bits, DWORD mask_pitch)
{
	DWORD *pat;
	int x, y;

	if(!M2010_cursor_available()) return FALSE;
	if(w <= 0 || h <= 0 || w > CUR_SIZE || h > CUR_SIZE) goto fallback;
	if(bpp != 1 && (!conf_curcolor || (bpp != 16 && bpp != 32) || bpp != (int)hda->bpp)) goto fallback;

	/* inverting pixels: software cursor unless approximated (M2010HwInvert) */
	if(!conf_curinvert)
	{
		for(y = 0; y < h; y++)
		{
			for(x = 0; x < w; x++)
			{
				BYTE a = (and_bits[mask_pitch * y + (x >> 3)] >> (7 - (x & 7))) & 1;
				if(!a) continue;
				if(bpp == 1 ? ((xor_bits[mask_pitch * y + (x >> 3)] >> (7 - (x & 7))) & 1)
				            : (cur_rgb888(xor_bits, x, y, w, bpp) != 0))
					goto fallback;
			}
		}
	}

	/* write the buffer not on screen, then point the cursor at it (RDC: HWC_NUM 2) */
	pat = (DWORD*)(((BYTE*)hda->vram_pm32) + m2010_cur_off + m2010_cur_next * CUR_BUF);
	for(y = 0; y < CUR_SIZE; y++)
	{
		for(x = 0; x < CUR_SIZE; x++)
		{
			DWORD v = 0;                              /* alpha 0: transparent */
			if(x < w && y < h)
			{
				BYTE a = (and_bits[mask_pitch * y + (x >> 3)] >> (7 - (x & 7))) & 1;
				DWORD c = (bpp == 1)
					? (((xor_bits[mask_pitch * y + (x >> 3)] >> (7 - (x & 7))) & 1) ? 0xFFFFFFUL : 0)
					: cur_rgb888(xor_bits, x, y, w, bpp);
				if(!a)        v = 0xFF000000UL | c;
				else if(c)    v = 0xFF000000UL;       /* inverting pixel, drawn black */
			}
			pat[y * CUR_SIZE + x] = v;
		}
	}

	m2010_cur_w = w;
	m2010_cur_h = h;
	m2010_cur_hotx = hotx;
	m2010_cur_hoty = hoty;
	m2010_cur_loaded = TRUE;

	MMIO(HWC_ADDRESS) = ((m2010_cur_off + m2010_cur_next * CUR_BUF) >> 3) & 0xFFFFFFUL;
	m2010_cur_next ^= 1;
	cursor_program_position();
	m2010_cur_visible = TRUE;
	cursor_program_show(TRUE);
	return TRUE;

fallback:
	M2010_cursor_hide();
	m2010_cur_loaded = FALSE;
	return FALSE;
}

void M2010_cursor_move(int x, int y)
{
	m2010_cur_x = x;
	m2010_cur_y = y;
	if(m2010_cur_loaded && m2010_cur_visible)
	{
		cursor_program_position();
	}
}

void M2010_cursor_show()
{
	if(!m2010_cur_loaded) return;
	m2010_cur_visible = TRUE;
	cursor_program_position();
	cursor_program_show(TRUE);
}

void M2010_cursor_hide()
{
	if(!m2010_present) return;
	m2010_cur_visible = FALSE;
	MMIO(HWC_CTRL) = MMIO(HWC_CTRL) & ~HWC_CTRL_SHOW;
	cr_unlock();
	cr_mask(0xCB, 0xFC, 0x00);      /* the A0-style cursor stays off too */
}

/* after a BIOS mode set the cursor registers are not trusted any more */
void M2010_cursor_mode_set()
{
	if(!m2010_present) return;
	m2010_cur_loaded = FALSE;
	M2010_cursor_hide();
}

/* ---- refresh rate ------------------------------------------------------ */

/*
 * Pick the ROM timing entry for w x h closest to the request and leave its
 * ID in CR 8D for the VESA BIOS mode set that follows (OEM_SetRefreshRate).
 * rr_min/rr_max come from Windows (Display Properties / monitor limits);
 * the registry value M2010Refresh overrides them. Returns the Hz chosen or 0.
 */
DWORD M2010_refresh_select(DWORD w, DWORD h, DWORD rr_min, DWORD rr_max)
{
	DWORD off, target, best_hz = 0, best_diff = 0xFFFFFFFFUL;
	BYTE  best_id = 0xFF;

	if(!m2010_present || m2010_rom == NULL) return 0;

	if(conf_refresh >= 40 && conf_refresh <= 200)
	{
		target = conf_refresh;
		rr_min = 0;
	}
	else if(rr_max >= 40 && rr_max <= 200)
	{
		target = rr_max;
		if(rr_min < 40 || rr_min > target) rr_min = 0;
	}
	else
	{
		return 0; /* no preference: keep what the BIOS does by default */
	}

	for(off = m2010_vpit; off + MODE_INFO_SIZE <= ROM_SIZE; )
	{
		WORD  hs = *(WORD*)(m2010_rom + off);
		WORD  vs = *(WORD*)(m2010_rom + off + 2);
		BYTE  cnt = m2010_rom[off + 10];
		DWORD i;

		if(hs == 0xFFFF) break;
		if(cnt > 16) break;

		if(hs == w && vs == h)
		{
			for(i = 0; i < cnt; i++)
			{
				DWORD e = off + MODE_INFO_SIZE + RRATE_SIZE * i;
				BYTE  id = m2010_rom[e + 4];
				WORD  attr = *(WORD*)(m2010_rom + e + 5);
				DWORD hz, diff;

				if(e + RRATE_SIZE > ROM_SIZE) break;
				if(attr & RR_ATTR_DISABLE) continue;
				if(id > 15) continue;
				hz = rr_id_hz[id];
				if(hz < rr_min) continue;

				/* prefer the highest rate not above the target, else the nearest */
				diff = (hz <= target) ? (target - hz) : (hz - target) + 1000;
				if(diff < best_diff)
				{
					best_diff = diff;
					best_hz = hz;
					best_id = id;
				}
			}
			break;
		}
		off += MODE_INFO_SIZE + RRATE_SIZE * cnt;
	}

	if(best_id == 0xFF)
	{
		dbg_printf("M2010: no ROM timing for %ldx%ld near %ld Hz\n", w, h, target);
		return 0;
	}

	cr_unlock();
	cr_mask(CR_RRATE_ID, 0x01, (BYTE)(best_id << 1));
	dbg_printf("M2010: %ldx%ld refresh request %ld-%ld Hz -> ROM id %d (%ld Hz)\n",
		w, h, rr_min, target, best_id, best_hz);
	return best_hz;
}

BOOL M2010_present()
{
	return m2010_present;
}

DWORD M2010_caps()
{
	DWORD caps = 0;
	if(m2010_present) caps |= M2010_CAP_PRESENT;
	if(m2010_engine)  caps |= M2010_CAP_ENGINE;
	if(m2010_engine && conf_gdiblt) caps |= M2010_CAP_GDIBLT;
	if(m2010_engine && conf_gdiblt && conf_textfill) caps |= M2010_CAP_TEXTFILL;
	if(m2010_engine && conf_gdiblt && conf_rectfill) caps |= M2010_CAP_RECTFILL;
	if(m2010_engine && conf_gdiblt && conf_patfill && m2010_pat_ready) caps |= M2010_CAP_PATFILL;
	return caps;
}

DWORD M2010_min_area()
{
	return conf_min_area;
}

void M2010_engine_enable(DWORD visible)
{
	if(!m2010_present || m2010_broken || !conf_accel) return;
	if(m2010_engine) return;

	if(hda->bpp != 8 && hda->bpp != 16 && hda->bpp != 32)
	{
		dbg_printf("M2010: %ld bpp not supported by the engine\n", hda->bpp);
		return;
	}
	if(hda->pitch > ENG_MAX_PITCH || hda->width > ENG_MAX_XY || hda->height > ENG_MAX_XY)
	{
		dbg_printf("M2010: mode too large for the engine\n");
		return;
	}

	/* bEnable2D() from rdc_2dtool.c, MMIO command mode */
	cr_unlock();
	cr_mask(0xA4, 0xFE, 0x01);
	cr_mask(0xA3, (BYTE)~0x20, 0x20);
	MMIO(REG_1ST_FLIP) = (visible & FLIP_BASE_MASK) | FLIP_ENABLE;
	m2010_visible = visible;
	MMIO(REG_CMDQ_BASE) = CMDQ_MMIO_MODE;

	m2010_engine  = TRUE;
	m2010_pending = TRUE;
	if(engine_wait_idle())
	{
		dbg_printf("M2010: 2D engine enabled (%ldx%ldx%ld pitch %ld, start %lX)\n",
			hda->width, hda->height, hda->bpp, hda->pitch, visible);
	}
}

void M2010_engine_disable()
{
	if(!m2010_present) return;
	if(!m2010_engine) return;

	if(!m2010_broken)
	{
		engine_wait_idle();
	}

	/* vDisable2D() from rdc_2dtool.c; display start returns to the CRTC */
	m2010_engine  = FALSE;
	m2010_pending = FALSE;
	cr_unlock();
	cr_mask(0xA4, 0xFE, 0x00);
	cr_mask(0xA3, (BYTE)~0x20, 0x00);
	MMIO(REG_1ST_FLIP) = MMIO(REG_1ST_FLIP) & ~FLIP_ENABLE;
	dbg_printf("M2010: 2D engine disabled\n");
}

void M2010_sync()
{
	if(m2010_engine && m2010_pending)
	{
		engine_wait_idle();
	}
}

/* ---- flip timing ----------------------------------------------------------
 * DirectDraw page flips arrive here through FBHDA_swap. While the engine is on,
 * the scan-out start comes from REG_1ST_FLIP, not from the CRTC registers the
 * VBE 4F07 call sets, so the BIOS's own retrace wait is no help: the write
 * used to land just after the retrace began and showed one frame late, while
 * DirectDraw already drew into the buffer still on screen (flicker in DxDiag's
 * bouncing box test). Input status 1 (relocated 3DAh) bit 3 = in retrace.
 * Registry: M2010FlipSync 1 = leave a retrace in progress, write, then wait
 * for the next retrace to begin (the start address latches there on VGA
 * CRTCs); 2 = wait for the retrace to begin, then write (for a register that
 * takes effect at once); 0 = write immediately (build 29 behaviour). */
static BOOL  flipsync_dead = FALSE;  /* bit 3 never toggled: stop waiting */

#define VGA_STAT1       ((WORD)(m2010_io + 0x5A))
#define STAT1_RETRACE   0x08
#define RETRACE_POLLS   100000UL      /* > 1 frame at ~1 us per port read */

static BOOL wait_retrace(BOOL in)
{
	DWORD n;
	for(n = 0; n < RETRACE_POLLS; n++)
	{
		if(((inp(VGA_STAT1) & STAT1_RETRACE) != 0) == in) return TRUE;
	}
	flipsync_dead = TRUE;
	dbg_printf("M2010: no vertical retrace seen on %X, flip sync off\n", VGA_STAT1);
	return FALSE;
}

BOOL M2010_engine_active()
{
	return m2010_engine;
}

void M2010_set_display_start(DWORD offset)
{
	BOOL sync;
	if(!m2010_engine) return;
	m2010_visible = offset;
	sync = (conf_flipsync != 0) && !flipsync_dead && (m2010_io != 0);

	if(sync && conf_flipsync == 2)
	{
		if(wait_retrace(FALSE)) wait_retrace(TRUE);
	}
	else if(sync)
	{
		wait_retrace(FALSE);
	}

	MMIO(REG_1ST_FLIP) = (offset & FLIP_BASE_MASK) | FLIP_ENABLE;

	if(sync && conf_flipsync != 2 && !flipsync_dead)
	{
		wait_retrace(TRUE);
	}
}

BOOL M2010_copy(DWORD src_surf, DWORD dst_surf,
	DWORD sx, DWORD sy, DWORD dx, DWORD dy, DWORD w, DWORD h, DWORD rop)
{
	DWORD cmd;
	BOOL  xdec = FALSE, ydec = FALSE;
	DWORD cy, cx, ch, cw;

	if(!m2010_engine) return FALSE;
	if(w == 0 || h == 0) return TRUE;
	if(sx + w > ENG_MAX_XY + 1 || dx + w > ENG_MAX_XY + 1) return FALSE;

	if(src_surf == dst_surf)
	{
		if(dy > sy) ydec = TRUE;
		else if(dy == sy && dx > sx) xdec = TRUE;
	}

	cmd = CMD_BITBLT | color_bits() | ((rop & 0xFF) << 8);
	if(xdec) cmd |= CMD_X_DEC;
	if(ydec) cmd |= CMD_Y_DEC;

	if(!engine_begin()) return FALSE;

	/* split into chunks the 11-bit size fields can hold; for decrementing
	 * directions the chunks are visited from the far end so that overlapping
	 * copies stay correct */
	for(cy = 0; cy < h; cy += ENG_MAX_RECT)
	{
		DWORD oy;
		ch = h - cy; if(ch > ENG_MAX_RECT) ch = ENG_MAX_RECT;
		oy = ydec ? (h - cy - ch) : cy;

		for(cx = 0; cx < w; cx += ENG_MAX_RECT)
		{
			DWORD ox, src_base, dst_base, csx, cdx, csy, cdy;
			cw = w - cx; if(cw > ENG_MAX_RECT) cw = ENG_MAX_RECT;
			ox = xdec ? (w - cx - cw) : cx;

			/* like the RDC driver: base points at the first row, Y counts
			 * inside the chunk; X_DEC/Y_DEC start at the far corner */
			src_base = src_surf + hda->pitch * (sy + oy);
			dst_base = dst_surf + hda->pitch * (dy + oy);
			csx = sx + ox; cdx = dx + ox;
			if(xdec) { csx += cw - 1; cdx += cw - 1; }
			csy = cdy = ydec ? (ch - 1) : 0;

			if(m2010_pending && !engine_wait_idle()) return FALSE;
			if(!engine_rect_copy(cmd, src_base, dst_base, csx, csy, cdx, cdy, cw, ch))
			{
				return FALSE;
			}
		}
	}

	if(conf_sync)
	{
		engine_wait_idle();
	}
	return TRUE;
}

BOOL M2010_fill(DWORD dst_surf, DWORD x, DWORD y, DWORD w, DWORD h,
	DWORD color, DWORD rop)
{
	DWORD cmd, cx, cy, cw, ch;

	if(!m2010_engine) return FALSE;
	if(w == 0 || h == 0) return TRUE;
	if(x + w > ENG_MAX_XY + 1) return FALSE;

	cmd = CMD_BITBLT | CMD_PAT_FGCOLOR | color_bits() | ((rop & 0xFF) << 8);

	if(!engine_begin()) return FALSE;

	for(cy = 0; cy < h; cy += ENG_MAX_RECT)
	{
		ch = h - cy; if(ch > ENG_MAX_RECT) ch = ENG_MAX_RECT;
		for(cx = 0; cx < w; cx += ENG_MAX_RECT)
		{
			cw = w - cx; if(cw > ENG_MAX_RECT) cw = ENG_MAX_RECT;
			if(m2010_pending && !engine_wait_idle()) return FALSE;
			if(!engine_rect_fill(cmd, dst_surf + hda->pitch * (y + cy), x + cx, 0, cw, ch, color))
			{
				return FALSE;
			}
		}
	}
	return TRUE;
}

BOOL M2010_gdi_blt(DWORD surf, DWORD sx, DWORD sy, DWORD dx, DWORD dy,
	DWORD w, DWORD h, DWORD color, DWORD rop_flags)
{
	DWORD rop = rop_flags & 0xFF;
	DWORD l, t, r, b;
	BOOL  rs;

	if(!m2010_engine || !conf_gdiblt) return FALSE;
	if(conf_oplog)
	{
		conf_oplog--;
		dbg_printf("M2010 gdi: %s rop %02lX surf %lX dst %ld,%ld %ldx%ld src %ld,%ld color %lX (bpp %ld)\n",
			(rop_flags & M2010_BLT_COPY) ? "copy" : "fill", rop, surf, dx, dy, w, h, sx, sy, color, hda->bpp);
	}
	if(w == 0 || h == 0) return TRUE;

	/* the request must stay inside the GDI surface of the current mode */
	if(dx >= hda->width || dy >= hda->height) return FALSE;
	if(dx + w > hda->width || dy + h > hda->height) return FALSE;
	if(surf + hda->stride > hda->vram_size) return FALSE;

	l = dx; t = dy; r = dx + w; b = dy + h;

	if(rop_flags & M2010_BLT_COPY)
	{
		if(sx >= hda->width || sy >= hda->height) return FALSE;
		if(sx + w > hda->width || sy + h > hda->height) return FALSE;
		if(sx < l) l = sx;
		if(sy < t) t = sy;
		if(sx + w > r) r = sx + w;
		if(sy + h > b) b = sy + h;
	}
	else if((rop_flags & M2010_BLT_FILL) == 0)
	{
		return FALSE;
	}

	/* same protocol as the DIB engine: hide the cursor over the rectangle,
	 * draw, then let FBHDA_access_end() redraw it and refresh the front buffer */
	FBHDA_access_rect(l, t, r, b);

	if(rop_flags & M2010_BLT_COPY)
	{
		rs = M2010_copy(surf, surf, sx, sy, dx, dy, w, h, rop);
	}
	else
	{
		rs = M2010_fill(surf, dx, dy, w, h, color, rop);
	}

	/* the engine may still be running: everyone who touches the surface
	 * with the CPU goes through FBHDA_access_begin/rect, which syncs */
	if(rs && !conf_async)
	{
		rs = engine_wait_idle();
	}

	FBHDA_access_end(0);

	return rs;
}

/* ---- build number in the adapter name ------------------------------------
 * TRY/BACK/GOOD/RESCUE only swap VESAMINI.DRV/.VXD, so the INF strings never
 * change after the first install. At Init_Complete the running VxD writes
 * "<name> - build NNx" into DriverDesc (Display Properties > Advanced) of
 * the display class key that uses vesamini.drv, and into DeviceDesc (Device
 * Manager) of the PCI\VEN_17F3&DEV_2010 devnodes; only when it differs.
 * Registry: M2010SetName = 0 turns this off. */
#if defined(DBGMEM)
#define M2010_BUILD_ID M2010_BUILD_NO "D"
#elif defined(M2010_CUREXP)
#define M2010_BUILD_ID M2010_BUILD_NO "C"     /* experiments on (none in build 33) */
#elif M2010_FLIPSYNC_DEFAULT == 2
#define M2010_BUILD_ID M2010_BUILD_NO "S"     /* flip: wait for retrace, then write */
#else
#define M2010_BUILD_ID M2010_BUILD_NO "R"
#endif
#define M2010_BASE_NAME "RDC M2010 Graphics (Vortex86MX)"

static DWORD conf_setname = 1;
static char  m2010_name[] = M2010_BASE_NAME " - build " M2010_BUILD_ID;

static DWORD __declspec(naked) __cdecl reg_enum_key(DWORD hKey, DWORD idx, char *name, DWORD cch)
{
	VMMJmp(_RegEnumKey);
}

static DWORD __declspec(naked) __cdecl reg_set_value_ex(DWORD hKey, char *name, DWORD reserved,
	DWORD type, BYTE *data, DWORD cb)
{
	VMMJmp(_RegSetValueEx);
}

static DWORD reg_str_len(const char *s)
{
	DWORD n = 0;
	while(s[n]) n++;
	return n;
}

static BOOL reg_str_eq(const char *a, const char *b, BOOL nocase)
{
	while(*a && *b)
	{
		char x = *a++, y = *b++;
		if(nocase)
		{
			if(x >= 'a' && x <= 'z') x -= 32;
			if(y >= 'a' && y <= 'z') y -= 32;
		}
		if(x != y) return FALSE;
	}
	return *a == *b;
}

static void reg_str_cat(char *dst, DWORD size, const char *a, const char *b)
{
	DWORD n = 0;
	while(*a && n + 1 < size) dst[n++] = *a++;
	while(*b && n + 1 < size) dst[n++] = *b++;
	dst[n] = '\0';
}

/* read a REG_SZ value of an open key; "" if missing */
static void reg_get_str(DWORD key, char *name, char *buf, DWORD size)
{
	DWORD type = 0, cb = size - 1;
	buf[0] = '\0';
	if(_RegQueryValueEx(key, name, 0, &type, (BYTE*)buf, &cb) != ERROR_SUCCESS || type != REG_SZ)
		buf[0] = '\0';
	buf[size - 1] = '\0';
}

/* key\value := m2010_name unless it already is; returns 1 when written */
static int reg_put_name(DWORD key, char *value)
{
	char cur[128];
	reg_get_str(key, value, cur, sizeof(cur));
	if(reg_str_eq(cur, m2010_name, FALSE)) return 0;
	if(reg_set_value_ex(key, value, 0, REG_SZ, (BYTE*)m2010_name, reg_str_len(m2010_name) + 1) != ERROR_SUCCESS)
		return 0;
	dbg_printf("M2010: %s \"%s\" -> \"%s\"\n", value, cur, m2010_name);
	return 1;
}

void M2010_set_name()
{
	static char cls_path[] = "System\\CurrentControlSet\\Services\\Class\\Display";
	static char enum_path[] = "Enum\\PCI";
	static char v_desc[] = "DriverDesc";
	static char v_devdesc[] = "DeviceDesc";
	static char v_drv[] = "drv";
	char sub[64], sub2[64], path[200], drv[32];
	DWORD root, key, key2, i, j;
	int written = 0;

	RegReadConf(HKEY_LOCAL_MACHINE, M2010_conf_path, "M2010SetName", &conf_setname);
	if(!conf_setname || !m2010_present) return;

	/* Display class: the instance whose DEFAULT\drv is vesamini.drv */
	if(_RegOpenKey(HKEY_LOCAL_MACHINE, cls_path, &root) == ERROR_SUCCESS)
	{
		for(i = 0; i < 16 && reg_enum_key(root, i, sub, sizeof(sub)) == ERROR_SUCCESS; i++)
		{
			reg_str_cat(path, sizeof(path), sub, "\\DEFAULT");
			if(_RegOpenKey(root, path, &key) != ERROR_SUCCESS) continue;
			reg_get_str(key, v_drv, drv, sizeof(drv));
			_RegCloseKey(key);
			if(!reg_str_eq(drv, "vesamini.drv", TRUE)) continue;
			if(_RegOpenKey(root, sub, &key) == ERROR_SUCCESS)
			{
				written += reg_put_name(key, v_desc);
				_RegCloseKey(key);
			}
		}
		_RegCloseKey(root);
	}

	/* PCI devnodes of 17F3:2010 (Device Manager reads DeviceDesc) */
	if(_RegOpenKey(HKEY_LOCAL_MACHINE, enum_path, &root) == ERROR_SUCCESS)
	{
		for(i = 0; i < 64 && reg_enum_key(root, i, sub, sizeof(sub)) == ERROR_SUCCESS; i++)
		{
			char prefix[18];
			DWORD k;
			for(k = 0; k < 17 && sub[k]; k++)
			{
				char c = sub[k];
				prefix[k] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
			}
			prefix[k] = '\0';
			if(!reg_str_eq(prefix, "VEN_17F3&DEV_2010", FALSE)) continue;
			if(_RegOpenKey(root, sub, &key) != ERROR_SUCCESS) continue;
			for(j = 0; j < 8 && reg_enum_key(key, j, sub2, sizeof(sub2)) == ERROR_SUCCESS; j++)
			{
				if(_RegOpenKey(key, sub2, &key2) == ERROR_SUCCESS)
				{
					written += reg_put_name(key2, v_devdesc);
					_RegCloseKey(key2);
				}
			}
			_RegCloseKey(key);
		}
		_RegCloseKey(root);
	}
	dbg_printf("M2010: adapter name \"%s\" (%d value(s) updated)\n", m2010_name, written);
}

/* ---- pattern brushes ------------------------------------------------------
 * PATCOPY with an 8x8 colour pattern, built only from the proven engine copy:
 * the pattern is written (CPU) into a scratch tile of 72 x 16 pixels in
 * reserved VRAM, so any horizontal/vertical phase (x & 7, y & 7) is a plain
 * copy from it; the first block of up to 64 x 8 pixels comes from the tile,
 * then the destination is doubled to the right and downwards from itself
 * (copies by multiples of 8 pixels keep the phase). The pattern is aligned to
 * the screen origin, like the DIB engine's own pattern fetch.
 * The 16-bit driver passes the 8x8 device pixels (linear address of its
 * buffer). Registry: M2010PatFill = 0 turns this off. */
BOOL M2010_gdi_pattern(DWORD surf, DWORD dx, DWORD dy, DWORD w, DWORD h,
	DWORD pat_lin, DWORD rop)
{
	const BYTE *pat = (const BYTE*)pat_lin;
	DWORD bpp_bytes, key, n, r, c, i, fw, fh;
	BOOL  same, rs;

	if(!m2010_engine || !conf_gdiblt || !conf_patfill || !m2010_pat_ready) return FALSE;
	if((rop & 0xFF) != 0xF0 || pat_lin < 0x10000UL) return FALSE;
	if(w == 0 || h == 0) return TRUE;
	if(dx >= hda->width || dy >= hda->height) return FALSE;
	if(dx + w > hda->width || dy + h > hda->height) return FALSE;
	if(surf + hda->stride > hda->vram_size) return FALSE;
	if(hda->bpp != 8 && hda->bpp != 16 && hda->bpp != 32) return FALSE;
	if(hda->pitch * PAT_ROWS > PAT_RESERVE) return FALSE;

	bpp_bytes = hda->bpp / 8;
	n = 64 * bpp_bytes;
	key = (hda->bpp << 16) | hda->pitch;

	same = (key == m2010_pat_key);
	for(i = 0; same && i < n; i++)
		if(pat[i] != m2010_pat_last[i]) same = FALSE;

	if(!same)
	{
		BYTE *tile = (BYTE*)hda->vram_pm32 + m2010_pat_off;
		M2010_sync();                       /* an earlier op may still read the tile */
		for(r = 0; r < PAT_ROWS; r++)
		{
			BYTE *row = tile + r * hda->pitch;
			const BYTE *src = pat + (r & 7) * 8 * bpp_bytes;
			for(c = 0; c < PAT_COLS; c++)
				for(i = 0; i < bpp_bytes; i++)
					row[c * bpp_bytes + i] = src[(c & 7) * bpp_bytes + i];
		}
		for(i = 0; i < n; i++) m2010_pat_last[i] = pat[i];
		m2010_pat_key = key;
	}

	FBHDA_access_rect(dx, dy, dx + w, dy + h);

	fw = (w < PAT_TILE_W) ? w : PAT_TILE_W;
	fh = (h < 8) ? h : 8;
	rs = M2010_copy(m2010_pat_off, surf, dx & 7, dy & 7, dx, dy, fw, fh, 0xCC);
	while(rs && fw < w)
	{
		n = (w - fw < fw) ? (w - fw) : fw;
		rs = M2010_copy(surf, surf, dx, dy, dx + fw, dy, n, fh, 0xCC);
		fw += n;
	}
	while(rs && fh < h)
	{
		n = (h - fh < fh) ? (h - fh) : fh;
		rs = M2010_copy(surf, surf, dx, dy, dx, dy + fh, w, n, 0xCC);
		fh += n;
	}
	if(rs && !conf_async)
	{
		rs = engine_wait_idle();
	}

	FBHDA_access_end(0);
	return rs;
}

/* ---- monitor power (DPMS) -------------------------------------------------
 * Win98's VDD asks a 4.1 mini-VDD for SET_MONITOR_POWER_STATE (54) and
 * GET_MONITOR_POWER_STATE_CAPS (55). The register convention is not in the
 * DDK headers ("SetMonitorPowerState(DEVNODE, DWORD)"), so the state is
 * taken from the first of ECX, EAX, EDX that holds a single CM_POWERSTATE_Dn
 * bit, and debug builds log all registers. The M2010 side is RDC's own
 * (rdc_vgatool.c RDCDisplayPowerManagementSet): SR01 bit 5 blanks the screen,
 * CR B6 bits 1..0 = 0 on, 1 standby, 2 suspend, 3 off.
 * Registry: M2010Dpms = 0 leaves the entries empty (default 1 since build 29,
 * confirmed with V28C: standby/off and wake work on the eBOX). */
static DWORD conf_dpms = 1;
static DWORD dpms_state = 0;

#define CM_PS_D0   0x01UL
#define CM_PS_D1   0x02UL
#define CM_PS_D2   0x04UL
#define CM_PS_D3   0x08UL
#define CR_SUCCESS_ 0x00UL
#define CR_DEFAULT_ 0x01UL
#define SET_MONITOR_POWER_STATE_       54   /* minivdd.h, 4.1 numbering */
#define GET_MONITOR_POWER_STATE_CAPS_  55

static void dpms_apply(DWORD st)
{
	BYTE sr1;
	if(!m2010_present) return;
	cr_unlock();
	sr1 = sr_read(0x01);
	sr_write(0x01, (BYTE)(st ? (sr1 | 0x20) : (sr1 & ~0x20)));
	cr_mask(0xB6, 0xFC, (BYTE)(st & 3));
	dpms_state = st;
}

void M2010_dpms_on()
{
	if(dpms_state) dpms_apply(0);
}

static BOOL dpms_bit(DWORD v)
{
	return v == CM_PS_D0 || v == CM_PS_D1 || v == CM_PS_D2 || v == CM_PS_D3;
}

DWORD __stdcall dpms_set_proc(DWORD r_eax, DWORD r_ebx, DWORD r_ecx, DWORD r_edx)
{
	DWORD ps, st;
	dbg_printf("M2010: SET_MONITOR_POWER_STATE eax=%lX ebx=%lX ecx=%lX edx=%lX\n", r_eax, r_ebx, r_ecx, r_edx);
	(void)r_ebx;
	if(dpms_bit(r_ecx)) ps = r_ecx;
	else if(dpms_bit(r_eax)) ps = r_eax;
	else if(dpms_bit(r_edx)) ps = r_edx;
	else return CR_DEFAULT_;
	switch(ps)
	{
		case CM_PS_D1: st = 1; break;
		case CM_PS_D2: st = 2; break;
		case CM_PS_D3: st = 3; break;
		default:       st = 0; break;
	}
	dpms_apply(st);
	dbg_printf("M2010: monitor power state %ld\n", st);
	return CR_SUCCESS_;
}

DWORD __stdcall dpms_caps_proc(DWORD r_eax, DWORD r_ebx, DWORD r_ecx, DWORD r_edx)
{
	dbg_printf("M2010: GET_MONITOR_POWER_STATE_CAPS eax=%lX ebx=%lX ecx=%lX edx=%lX\n", r_eax, r_ebx, r_ecx, r_edx);
	(void)r_eax; (void)r_ebx; (void)r_ecx; (void)r_edx;
	return CM_PS_D0 | CM_PS_D1 | CM_PS_D2 | CM_PS_D3;
}

static void __declspec(naked) dpms_set_entry()
{
	_asm {
		push ecx
		push edx
		push edx
		push ecx
		push ebx
		push eax
		call dpms_set_proc
		pop edx
		pop ecx
		clc
		ret
	}
}

static void __declspec(naked) dpms_caps_entry()
{
	_asm {
		push ecx
		push edx
		push edx
		push ecx
		push ebx
		push eax
		call dpms_caps_proc
		pop edx
		pop ecx
		clc
		ret
	}
}

/* Device_Init, after the 4.0 entries */
void M2010_dpms_register(DWORD *table, DWORD length)
{
	RegReadConf(HKEY_LOCAL_MACHINE, M2010_conf_path, "M2010Dpms", &conf_dpms);
	dbg_printf("M2010: Dpms=%ld, mini-VDD table length %ld\n", conf_dpms, length);
	if(!conf_dpms || table == NULL || length <= GET_MONITOR_POWER_STATE_CAPS_) return;
	table[SET_MONITOR_POWER_STATE_]      = (DWORD)dpms_set_entry;
	table[GET_MONITOR_POWER_STATE_CAPS_] = (DWORD)dpms_caps_entry;
}
